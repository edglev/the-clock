#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "agent_ble.hpp"
#include "agent_features.hpp"
#include "agent_gitlab.hpp"

#define NVS_NAMESPACE "gitlab"
#define GITLAB_URL_LEN 128
#define GITLAB_PAT_LEN 256
#define HTTP_BUFFER_LEN 131072
#define POLL_INTERVAL_MS 60000u
#define RETRY_INTERVAL_MS 5000u

static const char *TAG = "agent_gitlab";

typedef struct {
    char base_url[GITLAB_URL_LEN];
    char pat[GITLAB_PAT_LEN];
} gitlab_config_t;

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} response_buffer_t;

static gitlab_config_t s_config = {};
static bool s_configured;
static bool s_have_baseline;
static uint32_t s_sequence;
static uint32_t s_last_poll_ms;
static bool s_last_poll_succeeded;
static char s_previous_ids[AGENT_MR_MAX_COUNT][AGENT_MR_ID_LEN + 1] = {};
static uint8_t s_previous_count;
static int s_last_http_status;
static bool s_auth_failed;

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    memset(dst, 0, dst_size);
    if (!src) return;
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++) {
        char c = src[i];
        dst[i] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
    }
}

static bool load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t url_len = sizeof(s_config.base_url);
    size_t pat_len = sizeof(s_config.pat);
    bool ok = nvs_get_str(handle, "url", s_config.base_url, &url_len) == ESP_OK &&
              nvs_get_str(handle, "pat", s_config.pat, &pat_len) == ESP_OK &&
              s_config.base_url[0] && s_config.pat[0];
    nvs_close(handle);
    s_configured = ok;
    return ok;
}

static void config_handler(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "gitlab_url");
    cJSON *pat = cJSON_GetObjectItemCaseSensitive(root, "gitlab_pat");
    if (!cJSON_IsString(url) || !url->valuestring || !url->valuestring[0] ||
        !cJSON_IsString(pat) || !pat->valuestring || !pat->valuestring[0] ||
        strlen(url->valuestring) >= sizeof(s_config.base_url) ||
        strlen(pat->valuestring) >= sizeof(s_config.pat)) {
        cJSON_Delete(root);
        return;
    }

    gitlab_config_t next = {};
    copy_text(next.base_url, sizeof(next.base_url), url->valuestring);
    copy_text(next.pat, sizeof(next.pat), pat->valuestring);
    size_t len = strlen(next.base_url);
    while (len > 0 && next.base_url[len - 1] == '/') next.base_url[--len] = '\0';

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        esp_err_t err = nvs_set_str(handle, "url", next.base_url);
        if (err == ESP_OK) err = nvs_set_str(handle, "pat", next.pat);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
        if (err == ESP_OK) {
            s_config = next;
            s_configured = true;
            s_last_poll_ms = 0;
            ESP_LOGI(TAG, "GitLab PAT saved for %s", s_config.base_url);
        }
    }
    cJSON_Delete(root);
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    response_buffer_t *response = (response_buffer_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && response && event->data_len > 0) {
        size_t available = response->capacity - response->len - 1;
        size_t copy_len = (size_t)event->data_len < available ? (size_t)event->data_len : available;
        memcpy(response->data + response->len, event->data, copy_len);
        response->len += copy_len;
        response->data[response->len] = '\0';
    }
    return ESP_OK;
}

static char *gitlab_get(const char *url)
{
    response_buffer_t response = {};
    response.capacity = HTTP_BUFFER_LEN;
    response.data = (char *)calloc(response.capacity, 1);
    if (!response.data) return NULL;

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 20000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(response.data);
        return NULL;
    }
    esp_http_client_set_header(client, "PRIVATE-TOKEN", s_config.pat);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    s_last_http_status = status;
    if (status == 401 || status == 403) s_auth_failed = true;
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "GitLab GET failed: %s HTTP %d", esp_err_to_name(err), status);
        free(response.data);
        return NULL;
    }
    return response.data;
}

static void publish_auth_invalid(void)
{
    agent_merge_request_status_t status = {};
    status.sequence = ++s_sequence;
    status.auth_invalid = true;
    status.valid = true;
    agent_ble_set_merge_requests(&status);
}

static bool completed_review_state(const char *state)
{
    return state && (!strcmp(state, "reviewed") || !strcmp(state, "requested_changes") || !strcmp(state, "approved"));
}

static bool waiting_merge_request(cJSON *mr)
{
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(mr, "draft")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(mr, "work_in_progress"))) return true;
    cJSON *pipeline = cJSON_GetObjectItemCaseSensitive(mr, "head_pipeline");
    cJSON *pipeline_state = cJSON_IsObject(pipeline) ? cJSON_GetObjectItemCaseSensitive(pipeline, "status") : NULL;
    const char *ps = cJSON_IsString(pipeline_state) ? pipeline_state->valuestring : "";
    if (!strcmp(ps, "failed") || !strcmp(ps, "canceled") || !strcmp(ps, "running") || !strcmp(ps, "pending")) return true;
    cJSON *merge_state = cJSON_GetObjectItemCaseSensitive(mr, "detailed_merge_status");
    const char *ms = cJSON_IsString(merge_state) ? merge_state->valuestring : "";
    return !strcmp(ms, "conflict") || !strcmp(ms, "need_rebase") ||
           !strcmp(ms, "discussions_not_resolved") || !strcmp(ms, "ci_must_pass") ||
           !strcmp(ms, "checking") || !strcmp(ms, "preparing");
}

static const char *merge_request_status(cJSON *mr)
{
    cJSON *merge_state = cJSON_GetObjectItemCaseSensitive(mr, "detailed_merge_status");
    const char *state = cJSON_IsString(merge_state) ? merge_state->valuestring : "";
    if (!strcmp(state, "mergeable")) return "Ready";
    if (!strcmp(state, "not_approved") || !strcmp(state, "approvals_syncing")) return "Needs approval";
    return "Needs review";
}

static bool reviewer_completed(uint64_t project_id, uint64_t iid, uint64_t user_id)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/v4/projects/%llu/merge_requests/%llu/reviewers",
             s_config.base_url, (unsigned long long)project_id, (unsigned long long)iid);
    char *body = gitlab_get(url);
    if (!body) return false;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }
    bool completed = false;
    cJSON *reviewer = NULL;
    cJSON_ArrayForEach(reviewer, root) {
        cJSON *user = cJSON_GetObjectItemCaseSensitive(reviewer, "user");
        cJSON *id = cJSON_IsObject(user) ? cJSON_GetObjectItemCaseSensitive(user, "id") : NULL;
        cJSON *state = cJSON_GetObjectItemCaseSensitive(reviewer, "state");
        if (cJSON_IsNumber(id) && (uint64_t)id->valuedouble == user_id) {
            completed = completed_review_state(cJSON_IsString(state) ? state->valuestring : "");
            break;
        }
    }
    cJSON_Delete(root);
    return completed;
}

static bool poll_gitlab(void)
{
    s_auth_failed = false;
    char url[256];
    snprintf(url, sizeof(url), "%s/api/v4/user", s_config.base_url);
    char *body = gitlab_get(url);
    if (!body) {
        if (s_last_http_status == 401 || s_last_http_status == 403) {
            publish_auth_invalid();
            return true;
        }
        return false;
    }
    cJSON *user = cJSON_Parse(body);
    free(body);
    cJSON *user_id_json = cJSON_GetObjectItemCaseSensitive(user, "id");
    uint64_t user_id = cJSON_IsNumber(user_id_json) ? (uint64_t)user_id_json->valuedouble : 0;
    cJSON_Delete(user);
    if (!user_id) return false;

    snprintf(url, sizeof(url), "%s/api/v4/merge_requests?scope=reviews_for_me&state=opened&order_by=updated_at&sort=desc&per_page=100",
             s_config.base_url);
    body = gitlab_get(url);
    if (!body) {
        if (s_last_http_status == 401 || s_last_http_status == 403) {
            publish_auth_invalid();
            return true;
        }
        return false;
    }
    cJSON *mrs = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(mrs)) {
        cJSON_Delete(mrs);
        return false;
    }

    agent_merge_request_status_t next = {};
    cJSON *mr = NULL;
    cJSON_ArrayForEach(mr, mrs) {
        if (next.count >= AGENT_MR_MAX_COUNT || waiting_merge_request(mr)) continue;
        cJSON *project = cJSON_GetObjectItemCaseSensitive(mr, "project_id");
        cJSON *iid = cJSON_GetObjectItemCaseSensitive(mr, "iid");
        if (!cJSON_IsNumber(project) || !cJSON_IsNumber(iid)) continue;
        uint64_t project_id = (uint64_t)project->valuedouble;
        uint64_t mr_iid = (uint64_t)iid->valuedouble;
        if (reviewer_completed(project_id, mr_iid, user_id)) continue;

        agent_merge_request_t *item = &next.items[next.count++];
        snprintf(item->id, sizeof(item->id), "%llu:%llu",
                 (unsigned long long)project_id, (unsigned long long)mr_iid);
        cJSON *title = cJSON_GetObjectItemCaseSensitive(mr, "title");
        copy_text(item->title, sizeof(item->title), cJSON_IsString(title) ? title->valuestring : "Merge request");
        cJSON *refs = cJSON_GetObjectItemCaseSensitive(mr, "references");
        cJSON *full = cJSON_IsObject(refs) ? cJSON_GetObjectItemCaseSensitive(refs, "full") : NULL;
        copy_text(item->reference, sizeof(item->reference), cJSON_IsString(full) ? full->valuestring : "Merge request");
        copy_text(item->status, sizeof(item->status), merge_request_status(mr));
    }
    cJSON_Delete(mrs);

    if (s_auth_failed) {
        publish_auth_invalid();
        return true;
    }

    bool has_new = false;
    if (s_have_baseline) {
        for (int i = 0; i < next.count; i++) {
            bool known = false;
            for (int j = 0; j < s_previous_count; j++) {
                if (!strcmp(next.items[i].id, s_previous_ids[j])) {
                    known = true;
                    break;
                }
            }
            if (!known) has_new = true;
        }
    }
    s_previous_count = next.count;
    memset(s_previous_ids, 0, sizeof(s_previous_ids));
    for (int i = 0; i < next.count; i++) copy_text(s_previous_ids[i], sizeof(s_previous_ids[i]), next.items[i].id);
    s_have_baseline = true;
    next.sequence = ++s_sequence;
    next.new_items = has_new;
    next.valid = true;
    agent_ble_set_merge_requests(&next);
    ESP_LOGI(TAG, "%u merge requests need review%s", next.count, has_new ? " (new)" : "");
    return true;
}

static void gitlab_task(void *arg)
{
    while (1) {
        wifi_ap_record_t ap = {};
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t interval = s_last_poll_succeeded ? POLL_INTERVAL_MS : RETRY_INTERVAL_MS;
        if (s_configured && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
            (s_last_poll_ms == 0 || (uint32_t)(now - s_last_poll_ms) >= interval)) {
            s_last_poll_ms = now;
            s_last_poll_succeeded = poll_gitlab();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void agent_gitlab_init(void)
{
    load_config();
    if (!agent_feature_is_enabled(AGENT_FEATURE_GITLAB)) {
        ESP_LOGI(TAG, "GitLab screen disabled");
        return;
    }
    agent_ble_add_config_handler(config_handler);
    xTaskCreate(gitlab_task, "gitlab", 12288, NULL, 4, NULL);
}

bool agent_gitlab_is_configured(void)
{
    return agent_feature_is_enabled(AGENT_FEATURE_GITLAB) && s_configured;
}
