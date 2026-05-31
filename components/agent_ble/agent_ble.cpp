#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_sm.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "agent_viewer.hpp"
#include "agent_ble.hpp"

extern "C" void ble_store_config_init(void);

static const char *TAG = "agent_ble";

uint8_t g_ble_state = 0;
char    g_ble_stats_text[32] = "Agent ready";
bool    g_ble_connected = false;
bool    g_ble_stats_changed = false;

static uint16_t state_val_handle;
static uint16_t stats_val_handle;
static uint16_t action_val_handle;
static uint16_t name_val_handle;
static uint16_t multi_val_handle;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static ble_addr_t s_connected_peer_addr;
static bool s_have_connected_peer_addr = false;

#define MAX_BONDED_DEVICES 3
#define MAX_NAME_LEN 32
#define NVS_BOND_NAMESPACE "ble_bonds"

static char s_current_peer_name[MAX_NAME_LEN + 1] = "";

static portMUX_TYPE s_instances_mux = portMUX_INITIALIZER_UNLOCKED;
static agent_instance_info_t s_instances[AGENT_MAX_INSTANCES];
static int s_instance_count = 0;
static int s_focused_index = -1;

static portMUX_TYPE s_printer_mux = portMUX_INITIALIZER_UNLOCKED;
static agent_printer_status_t s_printer_status = {};

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char name[MAX_NAME_LEN + 1];
} bonded_device_t;

static bonded_device_t s_bonded_devices[MAX_BONDED_DEVICES];
static int s_bonded_count = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void copy_clean(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) return;
    memset(dst, 0, dst_size);
    size_t i = 0;
    if (src) {
        for (; i < dst_size - 1 && src[i]; i++) {
            char c = src[i];
            dst[i] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
        }
    }
}

static int find_instance_locked(const char *id)
{
    for (int i = 0; i < s_instance_count; i++) {
        if (strncmp(s_instances[i].id, id, AGENT_INSTANCE_ID_LEN) == 0) return i;
    }
    return -1;
}

static int oldest_instance_locked(void)
{
    int oldest = 0;
    for (int i = 1; i < s_instance_count; i++) {
        if (s_instances[i].updated_ms < s_instances[oldest].updated_ms) oldest = i;
    }
    return oldest;
}

static int instance_priority(uint8_t state)
{
    switch (state) {
    case AGENT_STATE_WAITING:  return 0;
    case AGENT_STATE_THINKING: return 1;
    case AGENT_STATE_SUCCESS:  return 2;
    case AGENT_STATE_IDLE:
    default:                   return 3;
    }
}

static int highest_priority_locked(void)
{
    if (s_instance_count == 0) return 3;

    int highest = instance_priority(s_instances[0].state);
    for (int i = 1; i < s_instance_count; i++) {
        int priority = instance_priority(s_instances[i].state);
        if (priority < highest) highest = priority;
    }
    return highest;
}

static bool instance_before_locked(int a, int b, int highest_priority)
{
    int priority_a = instance_priority(s_instances[a].state);
    int priority_b = instance_priority(s_instances[b].state);
    if (priority_a != priority_b) return priority_a < priority_b;

    if (priority_a == highest_priority) {
        if (a == s_focused_index) return true;
        if (b == s_focused_index) return false;
    }

    if (s_instances[a].priority_entered_ms != s_instances[b].priority_entered_ms) {
        return s_instances[a].priority_entered_ms < s_instances[b].priority_entered_ms;
    }
    if (s_instances[a].updated_ms != s_instances[b].updated_ms) {
        return s_instances[a].updated_ms > s_instances[b].updated_ms;
    }
    return strncmp(s_instances[a].id, s_instances[b].id, AGENT_INSTANCE_ID_LEN) < 0;
}

static int select_focused_instance_locked(void)
{
    if (s_instance_count == 0) return -1;

    int highest_priority = highest_priority_locked();
    if (s_focused_index >= 0 && s_focused_index < s_instance_count &&
        instance_priority(s_instances[s_focused_index].state) == highest_priority) {
        return s_focused_index;
    }

    int best = -1;
    for (int i = 0; i < s_instance_count; i++) {
        if (instance_priority(s_instances[i].state) != highest_priority) continue;
        if (best < 0 || instance_before_locked(i, best, highest_priority)) best = i;
    }
    return best;
}

static void reselect_focused_instance_locked(void)
{
    s_focused_index = select_focused_instance_locked();
}

static void sync_legacy_globals_locked(const agent_instance_info_t *inst)
{
    if (!inst) {
        g_ble_state = AGENT_STATE_IDLE;
        copy_clean(g_ble_stats_text, sizeof(g_ble_stats_text), "Agent ready");
        g_ble_stats_changed = true;
        return;
    }

    g_ble_state = inst->state;
    copy_clean(g_ble_stats_text, sizeof(g_ble_stats_text), inst->status[0] ? inst->status : inst->label);
    g_ble_stats_changed = true;
}

static void upsert_instance(const char *id, uint8_t state, const char *label, const char *status,
                            const char *provider, const char *branch, const char *metrics,
                            const char *model, const char *effort)
{
    if (!id || !id[0]) return;
    if (state > AGENT_STATE_SUCCESS) state = AGENT_STATE_IDLE;

    uint32_t updated = now_ms();
    portENTER_CRITICAL(&s_instances_mux);

    int idx = find_instance_locked(id);
    bool existing = idx >= 0;
    int previous_priority = existing ? instance_priority(s_instances[idx].state) : -1;
    if (idx < 0) {
        if (s_instance_count < AGENT_MAX_INSTANCES) {
            idx = s_instance_count++;
        } else {
            idx = oldest_instance_locked();
            existing = false;
        }
    }

    if (!existing && s_focused_index == idx) {
        s_focused_index = -1;
    }

    int new_priority = instance_priority(state);
    copy_clean(s_instances[idx].id, sizeof(s_instances[idx].id), id);
    copy_clean(s_instances[idx].label, sizeof(s_instances[idx].label), label && label[0] ? label : "Agent");
    copy_clean(s_instances[idx].status, sizeof(s_instances[idx].status), status && status[0] ? status : "Idle");
    copy_clean(s_instances[idx].provider, sizeof(s_instances[idx].provider), provider && provider[0] ? provider : "Agent");
    copy_clean(s_instances[idx].branch, sizeof(s_instances[idx].branch), branch ? branch : "");
    copy_clean(s_instances[idx].metrics, sizeof(s_instances[idx].metrics), metrics ? metrics : "");
    copy_clean(s_instances[idx].model, sizeof(s_instances[idx].model), model ? model : "");
    copy_clean(s_instances[idx].effort, sizeof(s_instances[idx].effort), effort ? effort : "");
    s_instances[idx].state = state;
    s_instances[idx].updated_ms = updated;
    if (!existing || previous_priority != new_priority || s_instances[idx].priority_entered_ms == 0) {
        s_instances[idx].priority_entered_ms = updated;
    }
    reselect_focused_instance_locked();
    g_ble_connected = true;
    sync_legacy_globals_locked(s_focused_index >= 0 ? &s_instances[s_focused_index] : NULL);

    portEXIT_CRITICAL(&s_instances_mux);
}

static void delete_instance(const char *id)
{
    if (!id || !id[0]) return;

    portENTER_CRITICAL(&s_instances_mux);
    int idx = find_instance_locked(id);
    if (idx >= 0) {
        bool focused_deleted = s_focused_index == idx;
        for (int i = idx; i < s_instance_count - 1; i++) {
            s_instances[i] = s_instances[i + 1];
        }
        s_instance_count--;
        if (focused_deleted) {
            s_focused_index = -1;
        } else if (s_focused_index > idx) {
            s_focused_index--;
        } else if (s_focused_index >= s_instance_count) {
            s_focused_index = -1;
        }
        reselect_focused_instance_locked();
        sync_legacy_globals_locked(s_focused_index >= 0 ? &s_instances[s_focused_index] : NULL);
    }
    portEXIT_CRITICAL(&s_instances_mux);
}

static void upsert_legacy_instance(void)
{
    upsert_instance("legacy", g_ble_state, s_current_peer_name[0] ? s_current_peer_name : "Agent",
                    g_ble_stats_text, "Agent", "", "", "", "");
}

static int split_tab_fields(char *payload, char **fields, int max_fields)
{
    if (!payload || !fields || max_fields <= 0) return 0;

    int count = 1;
    fields[0] = payload;
    for (char *p = payload; *p; p++) {
        if (*p == '\t') {
            *p = '\0';
            if (count >= max_fields) return count + 1;
            fields[count++] = p + 1;
        }
    }
    return count;
}

static int parse_int_field(const char *text, int fallback)
{
    if (!text || text[0] == '\0') return fallback;
    return atoi(text);
}

static void upsert_printer_status(char **fields, int field_count)
{
    if (!fields || field_count != 13) return;

    agent_printer_status_t next = {};
    copy_clean(next.state, sizeof(next.state), fields[1][0] ? fields[1] : "unknown");
    next.progress_percent = (int16_t)parse_int_field(fields[2], -1);
    next.eta_seconds = (int32_t)parse_int_field(fields[3], -1);
    next.layer = (int16_t)parse_int_field(fields[4], -1);
    next.layers = (int16_t)parse_int_field(fields[5], -1);
    copy_clean(next.nozzle_c, sizeof(next.nozzle_c), fields[6]);
    copy_clean(next.bed_c, sizeof(next.bed_c), fields[7]);
    copy_clean(next.chamber_c, sizeof(next.chamber_c), fields[8]);
    copy_clean(next.job, sizeof(next.job), fields[9]);
    copy_clean(next.material, sizeof(next.material), fields[10]);
    copy_clean(next.source, sizeof(next.source), fields[11][0] ? fields[11] : "Cloud");
    next.updated_ms = now_ms();
    next.valid = true;

    if (next.progress_percent > 100) next.progress_percent = 100;
    if (next.progress_percent < -1) next.progress_percent = -1;

    portENTER_CRITICAL(&s_printer_mux);
    s_printer_status = next;
    portEXIT_CRITICAL(&s_printer_mux);

    ESP_LOGI(TAG, "Printer update: state=%s progress=%d job=%s source=%s",
             next.state, next.progress_percent, next.job, next.source);
}

static void handle_multi_write(const uint8_t *data, int data_len)
{
    char payload[300];
    int len = data_len < (int)sizeof(payload) - 1 ? data_len : (int)sizeof(payload) - 1;
    memcpy(payload, data, len);
    payload[len] = '\0';

    char *fields[13] = {};
    int field_count = split_tab_fields(payload, fields, 13);
    if (field_count <= 0 || !fields[0]) return;

    if (fields[0][0] == 'D') {
        if (field_count != 2 || !fields[1][0]) return;
        delete_instance(fields[1]);
        ESP_LOGI(TAG, "Instance deleted: %s", fields[1]);
        return;
    }

    if (fields[0][0] == 'P') {
        upsert_printer_status(fields, field_count);
        return;
    }

    if (fields[0][0] != 'U' || field_count != 10) return;
    if (!fields[1][0] || !fields[2][0] || !fields[3][0] || !fields[4][0] || !fields[5][0]) return;

    uint8_t state = (uint8_t)(fields[2][0] - '0');
    upsert_instance(fields[1], state, fields[3], fields[4], fields[5], fields[6], fields[7], fields[8], fields[9]);
    ESP_LOGI(TAG, "Instance update: id=%s state=%u label=%s provider=%s branch=%s metrics=%s model=%s effort=%s",
             fields[1], state, fields[3], fields[5], fields[6], fields[7], fields[8], fields[9]);
}

static void load_bonds(void)
{
    s_bonded_count = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_BOND_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return;

    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) != ESP_OK) {
        nvs_close(h);
        return;
    }
    if (count > MAX_BONDED_DEVICES) count = MAX_BONDED_DEVICES;

    for (uint8_t i = 0; i < count; i++) {
        char key[20];
        snprintf(key, sizeof(key), "addr_%u", i);
        size_t len = 6;
        if (nvs_get_blob(h, key, s_bonded_devices[i].addr, &len) == ESP_OK) {
            snprintf(key, sizeof(key), "type_%u", i);
            nvs_get_u8(h, key, &s_bonded_devices[i].addr_type);
            snprintf(key, sizeof(key), "name_%u", i);
            size_t name_len = MAX_NAME_LEN;
            s_bonded_devices[i].name[0] = '\0';
            nvs_get_str(h, key, s_bonded_devices[i].name, &name_len);
            s_bonded_count++;
        }
    }
    nvs_close(h);
}

static void save_bonds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_BOND_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return;

    uint8_t count = (uint8_t)s_bonded_count;
    nvs_set_u8(h, "count", count);
    for (uint8_t i = 0; i < count; i++) {
        char key[20];
        snprintf(key, sizeof(key), "addr_%u", i);
        nvs_set_blob(h, key, s_bonded_devices[i].addr, 6);
        snprintf(key, sizeof(key), "type_%u", i);
        nvs_set_u8(h, key, s_bonded_devices[i].addr_type);
        snprintf(key, sizeof(key), "name_%u", i);
        nvs_set_str(h, key, s_bonded_devices[i].name);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void add_bond(const uint8_t *addr, uint8_t addr_type)
{
    for (int i = 0; i < s_bonded_count; i++) {
        if (memcmp(s_bonded_devices[i].addr, addr, 6) == 0) {
            // Update name if we have one now
            if (s_current_peer_name[0]) {
                strncpy(s_bonded_devices[i].name, s_current_peer_name, MAX_NAME_LEN);
                s_bonded_devices[i].name[MAX_NAME_LEN] = '\0';
                save_bonds();
            }
            return;
        }
    }
    if (s_bonded_count < MAX_BONDED_DEVICES) {
        memcpy(s_bonded_devices[s_bonded_count].addr, addr, 6);
        s_bonded_devices[s_bonded_count].addr_type = addr_type;
        strncpy(s_bonded_devices[s_bonded_count].name, s_current_peer_name, MAX_NAME_LEN);
        s_bonded_devices[s_bonded_count].name[MAX_NAME_LEN] = '\0';
        s_bonded_count++;
        save_bonds();
    }
}

static int find_cached_bond(const uint8_t *addr)
{
    for (int i = 0; i < s_bonded_count; i++) {
        if (memcmp(s_bonded_devices[i].addr, addr, 6) == 0) return i;
    }
    return -1;
}

static void remove_cached_bond(int index)
{
    if (index < 0 || index >= s_bonded_count) return;
    for (int i = index; i < s_bonded_count - 1; i++) {
        s_bonded_devices[i] = s_bonded_devices[i + 1];
    }
    s_bonded_count--;
}

static void sync_bond_cache_with_store(void)
{
    ble_addr_t peer_addrs[MAX_BONDED_DEVICES];
    int peer_count = 0;
    int rc = ble_store_util_bonded_peers(peer_addrs, &peer_count, MAX_BONDED_DEVICES);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to read NimBLE bond store: %d", rc);
        return;
    }

    bool changed = false;

    for (int i = 0; i < s_bonded_count;) {
        bool found = false;
        for (int j = 0; j < peer_count; j++) {
            if (memcmp(s_bonded_devices[i].addr, peer_addrs[j].val, 6) == 0) {
                s_bonded_devices[i].addr_type = peer_addrs[j].type;
                found = true;
                break;
            }
        }
        if (!found) {
            ESP_LOGW(TAG, "Dropping stale cached bond");
            remove_cached_bond(i);
            changed = true;
        } else {
            i++;
        }
    }

    for (int i = 0; i < peer_count && s_bonded_count < MAX_BONDED_DEVICES; i++) {
        if (find_cached_bond(peer_addrs[i].val) < 0) {
            memcpy(s_bonded_devices[s_bonded_count].addr, peer_addrs[i].val, 6);
            s_bonded_devices[s_bonded_count].addr_type = peer_addrs[i].type;
            s_bonded_devices[s_bonded_count].name[0] = '\0';
            s_bonded_count++;
            changed = true;
        }
    }

    if (changed) {
        save_bonds();
    }
    ESP_LOGI(TAG, "Loaded %d bonded device(s)", s_bonded_count);
}

// Pairing access control
static bool g_ble_pairing_mode = true;

bool agent_ble_is_pairing_mode(void) { return g_ble_pairing_mode; }

void agent_ble_enable_pairing_mode(void) { g_ble_pairing_mode = true; ESP_LOGI(TAG, "Pairing mode ON"); }

void agent_ble_disable_pairing_mode(void) { g_ble_pairing_mode = false; ESP_LOGI(TAG, "Pairing mode OFF"); }

static bool is_peer_bonded(const uint8_t *addr)
{
    return find_cached_bond(addr) >= 0;
}

static void delete_peer_bond(const ble_addr_t *peer_addr)
{
    if (!peer_addr) return;

    ble_store_util_delete_peer(peer_addr);
    int cached = find_cached_bond(peer_addr->val);
    if (cached >= 0) {
        remove_cached_bond(cached);
        save_bonds();
    }
}

// Pairing modal coordination
static uint16_t s_pairing_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_pairing_action = 0;
static bool s_pairing_pending = false;

static void pairing_modal_cb(bool accepted)
{
    if (!s_pairing_pending) return;

    struct ble_sm_io resp;
    memset(&resp, 0, sizeof(resp));
    resp.action = s_pairing_action;

    if (s_pairing_action == BLE_SM_IOACT_NUMCMP) {
        resp.numcmp_accept = accepted ? 1 : 0;
    }
    ble_sm_inject_io(s_pairing_conn_handle, &resp);

    if (!accepted) {
        ble_gap_terminate(s_pairing_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    s_pairing_pending = false;
    s_pairing_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

#define UUID_SVC  0x01, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define UUID_CHR1 0x02, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define UUID_CHR2 0x03, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define UUID_CHR3 0x04, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define UUID_CHR4 0x05, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define UUID_CHR5 0x06, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

static const ble_uuid128_t ble_svc_uuid = BLE_UUID128_INIT(UUID_SVC);
static const ble_uuid128_t ble_chr1_uuid = BLE_UUID128_INIT(UUID_CHR1);
static const ble_uuid128_t ble_chr2_uuid = BLE_UUID128_INIT(UUID_CHR2);
static const ble_uuid128_t ble_chr3_uuid = BLE_UUID128_INIT(UUID_CHR3);
static const ble_uuid128_t ble_chr4_uuid = BLE_UUID128_INIT(UUID_CHR4);
static const ble_uuid128_t ble_chr5_uuid = BLE_UUID128_INIT(UUID_CHR5);

static int ble_gatts_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_advertise(void);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const struct ble_gatt_chr_def ble_chars[] = {
    { .uuid = &ble_chr1_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY, .val_handle = &state_val_handle },
    { .uuid = &ble_chr2_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP, .val_handle = &stats_val_handle },
    { .uuid = &ble_chr3_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &action_val_handle },
    { .uuid = &ble_chr4_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP, .val_handle = &name_val_handle },
    { .uuid = &ble_chr5_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP, .val_handle = &multi_val_handle },
    { 0 },
};

static const struct ble_gatt_svc_def ble_gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &ble_svc_uuid.u, .characteristics = (struct ble_gatt_chr_def *)ble_chars },
    { 0 },
};
#pragma GCC diagnostic pop

static int ble_gap_event(struct ble_gap_event *event, void *arg);

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason: %d", reason);
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE synced");
}

static int ble_gatts_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        if (!desc.sec_state.encrypted) {
            ESP_LOGW(TAG, "Rejecting GATT access from unencrypted connection");
            return BLE_ATT_ERR_INSUFFICIENT_ENC;
        }
    }

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (attr_handle == state_val_handle) {
            os_mbuf_append(ctxt->om, &g_ble_state, 1);
        } else if (attr_handle == action_val_handle) {
            uint8_t val = 0;
            os_mbuf_append(ctxt->om, &val, 1);
        }
        return 0;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (attr_handle == state_val_handle) {
            uint8_t state = 0;
            if (OS_MBUF_PKTLEN(ctxt->om) >= 1 && os_mbuf_copydata(ctxt->om, 0, 1, &state) == 0) {
                g_ble_state = state;
                g_ble_connected = true;
                upsert_legacy_instance();
                ESP_LOGI(TAG, "State: %d", g_ble_state);
            }
        } else if (attr_handle == stats_val_handle) {
            int pkt_len = OS_MBUF_PKTLEN(ctxt->om);
            int len = pkt_len < (int)sizeof(g_ble_stats_text) - 1 ? pkt_len : (int)sizeof(g_ble_stats_text) - 1;
            if (os_mbuf_copydata(ctxt->om, 0, len, g_ble_stats_text) == 0) {
                g_ble_stats_text[len] = '\0';
                g_ble_stats_changed = true;
                upsert_legacy_instance();
                ESP_LOGI(TAG, "Stats: %s", g_ble_stats_text);
            }
        } else if (attr_handle == name_val_handle) {
            int pkt_len = OS_MBUF_PKTLEN(ctxt->om);
            int len = pkt_len < MAX_NAME_LEN ? pkt_len : MAX_NAME_LEN;
            if (os_mbuf_copydata(ctxt->om, 0, len, s_current_peer_name) == 0) {
                s_current_peer_name[len] = '\0';
                ESP_LOGI(TAG, "Peer name: %s", s_current_peer_name);
                // Update the bond entry for the currently connected device
                struct ble_gap_conn_desc d;
                if (ble_gap_conn_find(conn_handle, &d) == 0) {
                    for (int i = 0; i < s_bonded_count; i++) {
                        if (memcmp(s_bonded_devices[i].addr, d.peer_id_addr.val, 6) == 0) {
                            strncpy(s_bonded_devices[i].name, s_current_peer_name, MAX_NAME_LEN);
                            s_bonded_devices[i].name[MAX_NAME_LEN] = '\0';
                            save_bonds();
                            break;
                        }
                    }
                }
            }
        } else if (attr_handle == multi_val_handle) {
            uint8_t buf[300];
            int pkt_len = OS_MBUF_PKTLEN(ctxt->om);
            int len = pkt_len < (int)sizeof(buf) ? pkt_len : (int)sizeof(buf);
            if (os_mbuf_copydata(ctxt->om, 0, len, buf) == 0) {
                handle_multi_write(buf, len);
            }
        }
        return 0;
    default:
        return 0;
    }
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t handle = event->connect.conn_handle;
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(handle, &d) == 0) {
                s_connected_peer_addr = d.peer_id_addr;
                s_have_connected_peer_addr = true;
                bool bonded = is_peer_bonded(d.peer_id_addr.val);
                if (!bonded && !g_ble_pairing_mode && s_bonded_count > 0) {
                    ESP_LOGI(TAG, "Rejecting unpaired peer (no pairing mode)");
                    ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                if (bonded) {
                    g_ble_pairing_mode = false;
                }
            }
            conn_handle = handle;
            ESP_LOGI(TAG, "Connected, conn_handle=%d", conn_handle);
        } else {
            g_ble_connected = false;
            ESP_LOGI(TAG, "Connect failed");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_pairing_pending = false;
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_have_connected_peer_addr = false;
        g_ble_connected = false;
        ESP_LOGI(TAG, "Disconnected, reason=%d", event->disconnect.reason);
        ble_advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Encryption established");
            g_ble_connected = true;
            g_ble_pairing_mode = false;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                s_connected_peer_addr = desc.peer_id_addr;
                s_have_connected_peer_addr = true;
                add_bond(desc.peer_id_addr.val, desc.peer_id_addr.type);
            }
        } else {
            ESP_LOGW(TAG, "Encryption failed, status=%d", event->enc_change.status);
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                delete_peer_bond(&desc.peer_id_addr);
            } else if (s_have_connected_peer_addr) {
                delete_peer_bond(&s_connected_peer_addr);
            }
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ESP_LOGI(TAG, "Repeat pairing requested");
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            delete_peer_bond(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_gap_passkey_params *pk = &event->passkey.params;
        ESP_LOGI(TAG, "Passkey action=%d numcmp=%lu", pk->action, (unsigned long)pk->numcmp);
        if (pk->action == BLE_SM_IOACT_NUMCMP) {
            s_pairing_conn_handle = conn_handle;
            s_pairing_action = pk->action;
            s_pairing_pending = true;
            agent_viewer_show_pairing_modal(pk->numcmp, pairing_modal_cb);
        } else if (pk->action == BLE_SM_IOACT_DISP) {
            s_pairing_conn_handle = conn_handle;
            s_pairing_action = pk->action;
            s_pairing_pending = true;
            agent_viewer_show_pairing_modal(pk->numcmp, pairing_modal_cb);
        }
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Adv complete");
        ble_advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)"Agent-Viewer";
    fields.name_len = strlen("Agent-Viewer");
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "Advertising as Agent-Viewer");
}

void agent_ble_notify_action(uint8_t value)
{
    if (g_ble_connected) {
        uint8_t payload[1 + AGENT_INSTANCE_ID_LEN];
        int payload_len = 1;
        payload[0] = value;

        portENTER_CRITICAL(&s_instances_mux);
        if (s_focused_index >= 0 && s_focused_index < s_instance_count) {
            memcpy(&payload[1], s_instances[s_focused_index].id, AGENT_INSTANCE_ID_LEN);
            payload_len = 1 + AGENT_INSTANCE_ID_LEN;
        }
        portEXIT_CRITICAL(&s_instances_mux);

        os_mbuf *om = ble_hs_mbuf_from_flat(payload, payload_len);
        if (om) {
            ble_gattc_notify_custom(conn_handle, action_val_handle, om);
        }
    }
}

static void host_task(void *param)
{
    nimble_port_run();
}

void agent_ble_init(void)
{
    ESP_LOGI(TAG, "Initializing NimBLE");

    nimble_port_init();

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    load_bonds();
    sync_bond_cache_with_store();

    int rc = ble_gatts_count_cfg(ble_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(ble_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }

    nimble_port_freertos_init(host_task);
    ble_advertise();
}

int agent_ble_get_instances(agent_instance_info_t *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    portENTER_CRITICAL(&s_instances_mux);
    reselect_focused_instance_locked();
    int count = s_instance_count < max_count ? s_instance_count : max_count;
    int order[AGENT_MAX_INSTANCES];
    int highest_priority = highest_priority_locked();
    for (int i = 0; i < s_instance_count; i++) {
        order[i] = i;
    }
    for (int i = 0; i < s_instance_count - 1; i++) {
        for (int j = i + 1; j < s_instance_count; j++) {
            if (instance_before_locked(order[j], order[i], highest_priority)) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }
    for (int i = 0; i < count; i++) {
        out[i] = s_instances[order[i]];
    }
    portEXIT_CRITICAL(&s_instances_mux);

    return count;
}

bool agent_ble_get_focused_instance(agent_instance_info_t *out)
{
    if (!out) return false;

    bool found = false;
    portENTER_CRITICAL(&s_instances_mux);
    reselect_focused_instance_locked();
    if (s_focused_index >= 0 && s_focused_index < s_instance_count) {
        *out = s_instances[s_focused_index];
        found = true;
    }
    portEXIT_CRITICAL(&s_instances_mux);

    return found;
}

int agent_ble_get_instance_count(void)
{
    int count;
    portENTER_CRITICAL(&s_instances_mux);
    count = s_instance_count;
    portEXIT_CRITICAL(&s_instances_mux);
    return count;
}

bool agent_ble_get_printer_status(agent_printer_status_t *out)
{
    if (!out) return false;

    bool valid;
    portENTER_CRITICAL(&s_printer_mux);
    *out = s_printer_status;
    valid = s_printer_status.valid;
    portEXIT_CRITICAL(&s_printer_mux);
    return valid;
}

int agent_ble_get_bond_count(void)
{
    return s_bonded_count;
}

bool agent_ble_get_bond(int index, uint8_t addr[6])
{
    if (index < 0 || index >= s_bonded_count) return false;
    memcpy(addr, s_bonded_devices[index].addr, 6);
    return true;
}

const char *agent_ble_get_bond_name(int index)
{
    if (index < 0 || index >= s_bonded_count) return "";
    return s_bonded_devices[index].name;
}

void agent_ble_delete_bond(int index)
{
    if (index < 0 || index >= s_bonded_count) return;

    struct ble_gap_conn_desc desc;
    bool is_connected = (conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                         ble_gap_conn_find(conn_handle, &desc) == 0 &&
                         memcmp(desc.peer_id_addr.val, s_bonded_devices[index].addr, 6) == 0);

    ble_addr_t peer_addr;
    memcpy(peer_addr.val, s_bonded_devices[index].addr, 6);
    peer_addr.type = s_bonded_devices[index].addr_type;

    if (is_connected) {
        ble_gap_unpair(&peer_addr);
    }
    ble_store_util_delete_peer(&peer_addr);

    remove_cached_bond(index);
    save_bonds();
}

void agent_ble_delete_all_bonds(void)
{
    while (s_bonded_count > 0) {
        agent_ble_delete_bond(0);
    }
}
