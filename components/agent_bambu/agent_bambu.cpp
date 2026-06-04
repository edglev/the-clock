#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "agent_bambu.hpp"
#include "agent_ble.hpp"

static const char *TAG = "agent_bambu";

#define NVS_NAMESPACE "bambu_wifi"
#define TOKEN_MAX_LEN 3072
#define MQTT_MESSAGE_MAX_LEN 16384

typedef struct {
    char ssid[33];
    char wifi_password[65];
} wifi_network_t;

typedef struct {
    wifi_network_t networks[AGENT_BAMBU_MAX_WIFI_NETWORKS];
    uint8_t network_count;
    char region[8];
    char mqtt_broker[80];
    char access_token[TOKEN_MAX_LEN];
    char user_id[64];
    char device_id[64];
    char device_name[32];
} bambu_config_t;

static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static agent_bambu_status_t s_status = {};
static bambu_config_t s_config = {};
static bool s_configured = false;
static bool s_wifi_initialized = false;
static bool s_wifi_started = false;
static bool s_wifi_connected = false;
static int s_wifi_index = 0;
static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool s_cloud_connected = false;
static cJSON *s_print_snapshot = NULL;
static char *s_mqtt_message = NULL;
static int s_mqtt_message_len = 0;

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

static void set_status(agent_bambu_state_t state, const char *detail)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.configured = s_configured;
    s_status.wifi_connected = s_wifi_connected;
    s_status.cloud_connected = s_cloud_connected;
    s_status.fallback_active = !g_ble_connected && s_configured;
    s_status.state = state;
    s_status.wifi_count = s_config.network_count;
    if (s_config.network_count > 0 && s_wifi_index >= 0 && s_wifi_index < s_config.network_count) {
        copy_text(s_status.ssid, sizeof(s_status.ssid), s_config.networks[s_wifi_index].ssid);
    } else {
        copy_text(s_status.ssid, sizeof(s_status.ssid), "");
    }
    copy_text(s_status.printer, sizeof(s_status.printer), s_config.device_name[0] ? s_config.device_name : s_config.device_id);
    if (detail) copy_text(s_status.detail, sizeof(s_status.detail), detail);
    portEXIT_CRITICAL(&s_status_mux);
}

static bool nvs_get_str_fixed(nvs_handle_t h, const char *key, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return false;
    dst[0] = '\0';
    size_t len = dst_size;
    return nvs_get_str(h, key, dst, &len) == ESP_OK && dst[0] != '\0';
}

static bool config_has_cloud(const bambu_config_t *cfg)
{
    return cfg && cfg->network_count > 0 &&
           cfg->mqtt_broker[0] && cfg->access_token[0] && cfg->user_id[0] && cfg->device_id[0];
}

static bool save_config_to_nvs(const bambu_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open config NVS");
        return false;
    }

    nvs_set_u8(h, "wifi_count", cfg->network_count);
    for (int i = 0; i < AGENT_BAMBU_MAX_WIFI_NETWORKS; i++) {
        char ssid_key[8];
        char pass_key[8];
        snprintf(ssid_key, sizeof(ssid_key), "ssid%d", i);
        snprintf(pass_key, sizeof(pass_key), "pass%d", i);
        if (i < cfg->network_count) {
            nvs_set_str(h, ssid_key, cfg->networks[i].ssid);
            nvs_set_str(h, pass_key, cfg->networks[i].wifi_password);
        } else {
            nvs_erase_key(h, ssid_key);
            nvs_erase_key(h, pass_key);
        }
    }
    nvs_set_str(h, "region", cfg->region);
    nvs_set_str(h, "mqtt", cfg->mqtt_broker);
    nvs_set_str(h, "token", cfg->access_token);
    nvs_set_str(h, "user", cfg->user_id);
    nvs_set_str(h, "device", cfg->device_id);
    nvs_set_str(h, "name", cfg->device_name);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit config: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool load_config(void)
{
    bambu_config_t *cfg = (bambu_config_t *)calloc(1, sizeof(bambu_config_t));
    if (!cfg) {
        s_configured = false;
        set_status(AGENT_BAMBU_ERROR, "Config allocation failed");
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_configured = false;
        set_status(AGENT_BAMBU_NOT_CONFIGURED, "Not configured");
        free(cfg);
        return false;
    }
    uint8_t count = 0;
    nvs_get_u8(h, "wifi_count", &count);
    if (count > AGENT_BAMBU_MAX_WIFI_NETWORKS) count = AGENT_BAMBU_MAX_WIFI_NETWORKS;
    for (int i = 0; i < count; i++) {
        char ssid_key[8];
        char pass_key[8];
        snprintf(ssid_key, sizeof(ssid_key), "ssid%d", i);
        snprintf(pass_key, sizeof(pass_key), "pass%d", i);
        if (nvs_get_str_fixed(h, ssid_key, cfg->networks[cfg->network_count].ssid, sizeof(cfg->networks[0].ssid)) &&
            nvs_get_str_fixed(h, pass_key, cfg->networks[cfg->network_count].wifi_password, sizeof(cfg->networks[0].wifi_password))) {
            cfg->network_count++;
        }
    }

    bool ok = nvs_get_str_fixed(h, "mqtt", cfg->mqtt_broker, sizeof(cfg->mqtt_broker)) &&
              nvs_get_str_fixed(h, "token", cfg->access_token, sizeof(cfg->access_token)) &&
              nvs_get_str_fixed(h, "user", cfg->user_id, sizeof(cfg->user_id)) &&
              nvs_get_str_fixed(h, "device", cfg->device_id, sizeof(cfg->device_id)) &&
              cfg->network_count > 0;
    nvs_get_str_fixed(h, "region", cfg->region, sizeof(cfg->region));
    nvs_get_str_fixed(h, "name", cfg->device_name, sizeof(cfg->device_name));
    nvs_close(h);

    if (ok) {
        s_config = *cfg;
        s_configured = true;
        if (s_wifi_index < 0 || s_wifi_index >= s_config.network_count) s_wifi_index = 0;
        set_status(AGENT_BAMBU_STANDBY, "BLE primary");
    } else {
        s_configured = false;
        set_status(AGENT_BAMBU_NOT_CONFIGURED, "Not configured");
    }
    free(cfg);
    return ok;
}

static bool json_string(cJSON *root, const char *key, char *dst, size_t dst_size, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        if (required) ESP_LOGW(TAG, "Missing config field %s", key);
        return !required;
    }
    if (strlen(item->valuestring) >= dst_size) {
        ESP_LOGW(TAG, "Config field %s is too long", key);
        return false;
    }
    copy_text(dst, dst_size, item->valuestring);
    return true;
}

static int find_network(const bambu_config_t *cfg, const char *ssid)
{
    if (!cfg || !ssid || !ssid[0]) return -1;
    for (int i = 0; i < cfg->network_count; i++) {
        if (strcmp(cfg->networks[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static bool upsert_network(bambu_config_t *cfg, const char *ssid, const char *password)
{
    if (!cfg || !ssid || !ssid[0] || !password) return false;
    int index = find_network(cfg, ssid);
    if (index < 0) {
        if (cfg->network_count >= AGENT_BAMBU_MAX_WIFI_NETWORKS) {
            ESP_LOGW(TAG, "Wi-Fi network list is full");
            return false;
        }
        index = cfg->network_count++;
    }
    copy_text(cfg->networks[index].ssid, sizeof(cfg->networks[index].ssid), ssid);
    copy_text(cfg->networks[index].wifi_password, sizeof(cfg->networks[index].wifi_password), password);
    return true;
}

static bool save_config_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Invalid config JSON");
        return false;
    }

    bambu_config_t *cfg = (bambu_config_t *)calloc(1, sizeof(bambu_config_t));
    if (!cfg) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Config allocation failed");
        return false;
    }
    *cfg = s_config;

    char ssid[33] = {};
    char wifi_password[65] = {};
    bool ok = json_string(root, "ssid", ssid, sizeof(ssid), true) &&
              json_string(root, "wifi_password", wifi_password, sizeof(wifi_password), true) &&
              upsert_network(cfg, ssid, wifi_password) &&
              json_string(root, "region", cfg->region, sizeof(cfg->region), false) &&
              json_string(root, "mqtt_broker", cfg->mqtt_broker, sizeof(cfg->mqtt_broker), true) &&
              json_string(root, "access_token", cfg->access_token, sizeof(cfg->access_token), true) &&
              json_string(root, "user_id", cfg->user_id, sizeof(cfg->user_id), true) &&
              json_string(root, "device_id", cfg->device_id, sizeof(cfg->device_id), true) &&
              json_string(root, "device_name", cfg->device_name, sizeof(cfg->device_name), false);
    cJSON_Delete(root);
    if (!ok) {
        free(cfg);
        return false;
    }

    if (!save_config_to_nvs(cfg)) {
        free(cfg);
        return false;
    }

    s_config = *cfg;
    s_configured = config_has_cloud(&s_config);
    if (s_wifi_index < 0 || s_wifi_index >= s_config.network_count) s_wifi_index = 0;
    set_status(g_ble_connected ? AGENT_BAMBU_STANDBY : AGENT_BAMBU_WIFI_CONNECTING, "Config saved");
    ESP_LOGI(TAG, "Saved Wi-Fi/Bambu config for %s with %u network(s)",
             cfg->device_name[0] ? cfg->device_name : cfg->device_id,
             cfg->network_count);
    free(cfg);
    return true;
}

static void config_handler(const char *json)
{
    save_config_json(json);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        set_status(AGENT_BAMBU_WIFI_CONNECTING, "Wi-Fi reconnecting");
        if (!g_ble_connected && s_configured) {
            if (s_config.network_count > 1) {
                s_wifi_index = (s_wifi_index + 1) % s_config.network_count;
                wifi_config_t wifi_config = {};
                copy_text((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), s_config.networks[s_wifi_index].ssid);
                copy_text((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), s_config.networks[s_wifi_index].wifi_password);
                wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
                esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            }
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connected = true;
        portENTER_CRITICAL(&s_status_mux);
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
        portEXIT_CRITICAL(&s_status_mux);
        set_status(AGENT_BAMBU_WIFI_CONNECTED, "Wi-Fi connected");
    }
}

static void init_wifi_once(void)
{
    if (s_wifi_initialized) return;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    s_wifi_initialized = true;
}

static void start_wifi(void)
{
    if (!s_configured || s_wifi_started || s_config.network_count == 0) return;
    init_wifi_once();
    if (s_wifi_index < 0 || s_wifi_index >= s_config.network_count) s_wifi_index = 0;

    wifi_config_t wifi_config = {};
    copy_text((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), s_config.networks[s_wifi_index].ssid);
    copy_text((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), s_config.networks[s_wifi_index].wifi_password);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    s_wifi_connected = false;
    set_status(AGENT_BAMBU_WIFI_CONNECTING, "Wi-Fi connecting");
    esp_wifi_connect();
}

static void stop_wifi(void)
{
    if (!s_wifi_started) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_wifi_started = false;
    s_wifi_connected = false;
    portENTER_CRITICAL(&s_status_mux);
    s_status.ip[0] = '\0';
    portEXIT_CRITICAL(&s_status_mux);
    set_status(AGENT_BAMBU_STANDBY, "BLE primary");
}

static const char *json_string_value(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_int_value(cJSON *obj, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) return atoi(item->valuestring);
    return fallback;
}

static void json_temp(char *dst, size_t dst_size, cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        double value = item->valuedouble;
        if (value == (int)value) {
            snprintf(dst, dst_size, "%d", (int)value);
        } else {
            snprintf(dst, dst_size, "%.1f", value);
        }
    } else if (cJSON_IsString(item)) {
        copy_text(dst, dst_size, item->valuestring);
    }
}

static bool has_error(cJSON *print)
{
    return cJSON_GetObjectItemCaseSensitive(print, "print_error") ||
           cJSON_GetObjectItemCaseSensitive(print, "mc_print_error_code") ||
           cJSON_GetObjectItemCaseSensitive(print, "hms");
}

static const char *normalized_state(cJSON *print)
{
    const char *raw = json_string_value(print, "gcode_state");
    if (!raw || !raw[0]) return "unknown";
    char state[24] = {};
    for (size_t i = 0; i < sizeof(state) - 1 && raw[i]; i++) state[i] = (char)toupper((unsigned char)raw[i]);
    if (strstr(state, "RUNNING") || strstr(state, "PREPARE")) return "printing";
    if (strstr(state, "PAUSE")) return has_error(print) ? "error" : "paused";
    if (strstr(state, "FAILED") || strstr(state, "ERROR")) return "error";
    if (strstr(state, "FINISH") || strstr(state, "IDLE")) return "idle";
    return "unknown";
}

static void basename_copy(char *dst, size_t dst_size, const char *path)
{
    if (!path) {
        copy_text(dst, dst_size, "");
        return;
    }
    const char *base = strrchr(path, '/');
    copy_text(dst, dst_size, base ? base + 1 : path);
}

static void fill_printer_status(cJSON *print)
{
    agent_printer_status_t status = {};
    copy_text(status.state, sizeof(status.state), normalized_state(print));
    status.progress_percent = (int16_t)json_int_value(print, "mc_percent", -1);
    int minutes = json_int_value(print, "mc_remaining_time", -1);
    status.eta_seconds = minutes >= 0 ? minutes * 60 : -1;
    status.layer = (int16_t)json_int_value(print, "layer_num", -1);
    status.layers = (int16_t)json_int_value(print, "total_layer_num", -1);
    json_temp(status.nozzle_c, sizeof(status.nozzle_c), print, "nozzle_temper");
    json_temp(status.bed_c, sizeof(status.bed_c), print, "bed_temper");
    json_temp(status.chamber_c, sizeof(status.chamber_c), print, "chamber_temper");
    basename_copy(status.job, sizeof(status.job), json_string_value(print, "gcode_file"));
    copy_text(status.source, sizeof(status.source), "Cloud");

    cJSON *vt = cJSON_GetObjectItemCaseSensitive(print, "vt_tray");
    if (cJSON_IsObject(vt)) {
        copy_text(status.material, sizeof(status.material), json_string_value(vt, "tray_type"));
    }
    agent_ble_set_printer_status(&status);
}

static void color_copy(char *dst, size_t dst_size, const char *src)
{
    if (!src) {
        copy_text(dst, dst_size, "");
        return;
    }
    if (src[0] == '#') src++;
    copy_text(dst, dst_size, src);
    for (size_t i = 0; dst[i]; i++) dst[i] = (char)toupper((unsigned char)dst[i]);
}

static bool fill_ams_status(cJSON *print)
{
    cJSON *ams = cJSON_GetObjectItemCaseSensitive(print, "ams");
    if (!cJSON_IsObject(ams)) return false;

    agent_ams_status_t status = {};
    status.active_slot = (int8_t)json_int_value(ams, "tray_now", -1);
    if (status.active_slot < 0 || status.active_slot >= AGENT_AMS_TRAY_COUNT) status.active_slot = -1;
    cJSON *humidity = cJSON_GetObjectItemCaseSensitive(ams, "humidity");
    if (cJSON_IsNumber(humidity)) snprintf(status.humidity, sizeof(status.humidity), "%d", humidity->valueint);
    else if (cJSON_IsString(humidity)) copy_text(status.humidity, sizeof(status.humidity), humidity->valuestring);

    cJSON *units = cJSON_GetObjectItemCaseSensitive(ams, "ams");
    cJSON *unit = cJSON_IsArray(units) ? cJSON_GetArrayItem(units, 0) : NULL;
    cJSON *trays = cJSON_IsObject(unit) ? cJSON_GetObjectItemCaseSensitive(unit, "tray") : NULL;
    if (!cJSON_IsArray(trays)) return false;

    for (int i = 0; i < AGENT_AMS_TRAY_COUNT; i++) {
        cJSON *tray = cJSON_GetArrayItem(trays, i);
        if (!cJSON_IsObject(tray)) continue;
        copy_text(status.trays[i].material, sizeof(status.trays[i].material), json_string_value(tray, "tray_type"));
        color_copy(status.trays[i].color, sizeof(status.trays[i].color), json_string_value(tray, "tray_color"));
        status.trays[i].remaining_percent = (int8_t)json_int_value(tray, "remain", json_int_value(tray, "remaining_percent", -1));
        if (status.trays[i].remaining_percent > 100) status.trays[i].remaining_percent = 100;
        if (status.trays[i].remaining_percent < -1) status.trays[i].remaining_percent = -1;
        status.trays[i].loaded = status.trays[i].material[0] || status.trays[i].color[0];
    }
    agent_ble_set_ams_status(&status);
    return true;
}

static void merge_object(cJSON *dst, cJSON *src)
{
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, src) {
        cJSON *existing = cJSON_GetObjectItemCaseSensitive(dst, item->string);
        if (existing && cJSON_IsObject(existing) && cJSON_IsObject(item)) {
            merge_object(existing, item);
            continue;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(dst, item->string);
        cJSON_AddItemToObject(dst, item->string, cJSON_Duplicate(item, true));
    }
}

static void clear_stale_errors(cJSON *merged, cJSON *update)
{
    const char *state = json_string_value(update, "gcode_state");
    if (!state) return;
    if (strstr(state, "RUNNING") || strstr(state, "PREPARE") || strstr(state, "FINISH") || strstr(state, "IDLE")) {
        if (!cJSON_GetObjectItemCaseSensitive(update, "print_error")) cJSON_DeleteItemFromObjectCaseSensitive(merged, "print_error");
        if (!cJSON_GetObjectItemCaseSensitive(update, "mc_print_error_code")) cJSON_DeleteItemFromObjectCaseSensitive(merged, "mc_print_error_code");
        if (!cJSON_GetObjectItemCaseSensitive(update, "hms")) cJSON_DeleteItemFromObjectCaseSensitive(merged, "hms");
    }
}

static void handle_bambu_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Invalid Bambu MQTT JSON");
        return;
    }
    cJSON *print = cJSON_GetObjectItemCaseSensitive(root, "print");
    if (!cJSON_IsObject(print)) {
        cJSON_Delete(root);
        return;
    }
    if (!s_print_snapshot) s_print_snapshot = cJSON_CreateObject();
    merge_object(s_print_snapshot, print);
    clear_stale_errors(s_print_snapshot, print);
    fill_printer_status(s_print_snapshot);
    fill_ams_status(s_print_snapshot);
    cJSON_Delete(root);
}

static void publish_pushall(void)
{
    if (!s_mqtt) return;
    char topic[96];
    char payload[160];
    snprintf(topic, sizeof(topic), "device/%s/request", s_config.device_id);
    snprintf(payload, sizeof(payload),
             "{\"pushing\":{\"sequence_id\":\"%lld\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}",
             (long long)(esp_timer_get_time() / 1000000));
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 0, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_cloud_connected = true;
        set_status(AGENT_BAMBU_CLOUD_CONNECTED, "Cloud connected");
        char topic[96];
        snprintf(topic, sizeof(topic), "device/%s/report", s_config.device_id);
        esp_mqtt_client_subscribe(event->client, topic, 0);
        publish_pushall();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_cloud_connected = false;
        set_status(AGENT_BAMBU_CLOUD_CONNECTING, "Cloud reconnecting");
        break;
    case MQTT_EVENT_DATA:
        if (event->current_data_offset == 0) {
            free(s_mqtt_message);
            s_mqtt_message = NULL;
            s_mqtt_message_len = 0;
            if (event->total_data_len <= 0 || event->total_data_len > MQTT_MESSAGE_MAX_LEN) {
                ESP_LOGW(TAG, "Dropping oversized MQTT message: %d", event->total_data_len);
                break;
            }
            s_mqtt_message = (char *)calloc(event->total_data_len + 1, 1);
        }
        if (!s_mqtt_message) break;
        memcpy(s_mqtt_message + event->current_data_offset, event->data, event->data_len);
        s_mqtt_message_len += event->data_len;
        if (s_mqtt_message_len >= event->total_data_len) {
            s_mqtt_message[event->total_data_len] = '\0';
            handle_bambu_json(s_mqtt_message);
            free(s_mqtt_message);
            s_mqtt_message = NULL;
            s_mqtt_message_len = 0;
        }
        break;
    case MQTT_EVENT_ERROR:
        s_cloud_connected = false;
        set_status(AGENT_BAMBU_ERROR, "Cloud error");
        break;
    default:
        break;
    }
}

static void start_mqtt(void)
{
    if (s_mqtt || !s_configured || !s_wifi_connected) return;

    char uri[120];
    char username[72];
    snprintf(uri, sizeof(uri), "mqtts://%s", s_config.mqtt_broker);
    if (strncmp(s_config.user_id, "u_", 2) == 0) {
        snprintf(username, sizeof(username), "%s", s_config.user_id);
    } else {
        snprintf(username, sizeof(username), "u_%s", s_config.user_id);
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri;
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_cfg.credentials.username = username;
    mqtt_cfg.credentials.authentication.password = s_config.access_token;
    mqtt_cfg.credentials.client_id = "agent-viewer-esp32";

    s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt) {
        set_status(AGENT_BAMBU_ERROR, "MQTT init failed");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    s_cloud_connected = false;
    set_status(AGENT_BAMBU_CLOUD_CONNECTING, "Cloud connecting");
    esp_mqtt_client_start(s_mqtt);
}

static void stop_mqtt(void)
{
    if (!s_mqtt) return;
    esp_mqtt_client_stop(s_mqtt);
    esp_mqtt_client_destroy(s_mqtt);
    s_mqtt = NULL;
    s_cloud_connected = false;
    free(s_mqtt_message);
    s_mqtt_message = NULL;
    s_mqtt_message_len = 0;
    set_status(AGENT_BAMBU_STANDBY, "BLE primary");
}

static void bambu_task(void *arg)
{
    int push_ticks = 0;
    while (1) {
        if (!s_configured) {
            load_config();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (g_ble_connected) {
            stop_mqtt();
            stop_wifi();
            set_status(AGENT_BAMBU_STANDBY, "BLE primary");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!s_wifi_started) {
            start_wifi();
        }
        if (s_wifi_connected && !s_mqtt) {
            start_mqtt();
        }
        if (s_cloud_connected && ++push_ticks >= 30) {
            publish_pushall();
            push_ticks = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void agent_bambu_init(void)
{
    load_config();
    agent_ble_set_config_handler(config_handler);
    xTaskCreate(bambu_task, "bambu_cloud", 8192, NULL, 4, NULL);
}

void agent_bambu_get_status(agent_bambu_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_mux);
}

int agent_bambu_get_wifi_networks(agent_bambu_wifi_network_t *out, int max_count)
{
    int count = s_config.network_count;
    if (!out || max_count <= 0) return count;
    if (count > max_count) count = max_count;
    for (int i = 0; i < count; i++) {
        copy_text(out[i].ssid, sizeof(out[i].ssid), s_config.networks[i].ssid);
    }
    return s_config.network_count;
}

bool agent_bambu_delete_wifi_network(int index)
{
    if (index < 0 || index >= s_config.network_count) return false;

    bambu_config_t cfg = s_config;
    for (int i = index; i < cfg.network_count - 1; i++) {
        cfg.networks[i] = cfg.networks[i + 1];
    }
    if (cfg.network_count > 0) {
        cfg.network_count--;
        memset(&cfg.networks[cfg.network_count], 0, sizeof(cfg.networks[cfg.network_count]));
    }

    if (!save_config_to_nvs(&cfg)) return false;

    bool restart_wifi = s_wifi_started;
    if (restart_wifi) {
        stop_mqtt();
        stop_wifi();
    }

    s_config = cfg;
    s_configured = config_has_cloud(&s_config);
    if (s_wifi_index >= s_config.network_count) s_wifi_index = 0;

    if (!s_configured) {
        set_status(AGENT_BAMBU_NOT_CONFIGURED, "No Wi-Fi networks");
    } else {
        set_status(g_ble_connected ? AGENT_BAMBU_STANDBY : AGENT_BAMBU_WIFI_CONNECTING, "Wi-Fi removed");
        if (restart_wifi && !g_ble_connected) {
            start_wifi();
        }
    }
    ESP_LOGI(TAG, "Removed Wi-Fi network, %u saved", s_config.network_count);
    return true;
}
