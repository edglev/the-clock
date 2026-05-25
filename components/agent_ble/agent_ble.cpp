#include <string.h>
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

static const char *TAG = "agent_ble";

uint8_t g_ble_state = 0;
char    g_ble_stats_text[32] = "Agent ready";
bool    g_ble_connected = false;
bool    g_ble_stats_changed = false;

static uint16_t state_val_handle;
static uint16_t stats_val_handle;
static uint16_t action_val_handle;
static uint16_t name_val_handle;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

#define MAX_BONDED_DEVICES 3
#define MAX_NAME_LEN 32
#define NVS_BOND_NAMESPACE "ble_bonds"

static char s_current_peer_name[MAX_NAME_LEN + 1] = "";

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char name[MAX_NAME_LEN + 1];
} bonded_device_t;

static bonded_device_t s_bonded_devices[MAX_BONDED_DEVICES];
static int s_bonded_count = 0;

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
    ESP_LOGI(TAG, "Loaded %d bonded device(s) from NVS", s_bonded_count);
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

// Pairing access control
static bool g_ble_pairing_mode = true;

bool agent_ble_is_pairing_mode(void) { return g_ble_pairing_mode; }

void agent_ble_enable_pairing_mode(void) { g_ble_pairing_mode = true; ESP_LOGI(TAG, "Pairing mode ON"); }

void agent_ble_disable_pairing_mode(void) { g_ble_pairing_mode = false; ESP_LOGI(TAG, "Pairing mode OFF"); }

static bool is_peer_bonded(const uint8_t *addr)
{
    for (int i = 0; i < s_bonded_count; i++) {
        if (memcmp(s_bonded_devices[i].addr, addr, 6) == 0) return true;
    }
    return false;
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

static const ble_uuid128_t ble_svc_uuid = BLE_UUID128_INIT(UUID_SVC);
static const ble_uuid128_t ble_chr1_uuid = BLE_UUID128_INIT(UUID_CHR1);
static const ble_uuid128_t ble_chr2_uuid = BLE_UUID128_INIT(UUID_CHR2);
static const ble_uuid128_t ble_chr3_uuid = BLE_UUID128_INIT(UUID_CHR3);
static const ble_uuid128_t ble_chr4_uuid = BLE_UUID128_INIT(UUID_CHR4);

static int ble_gatts_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_advertise(void);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const struct ble_gatt_chr_def ble_chars[] = {
    { .uuid = &ble_chr1_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, .val_handle = &state_val_handle },
    { .uuid = &ble_chr2_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE, .val_handle = &stats_val_handle },
    { .uuid = &ble_chr3_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &action_val_handle },
    { .uuid = &ble_chr4_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE, .val_handle = &name_val_handle },
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
            if (ctxt->om->om_len >= 1) {
                g_ble_state = ctxt->om->om_data[0];
                g_ble_connected = true;
                ESP_LOGI(TAG, "State: %d", g_ble_state);
            }
        } else if (attr_handle == stats_val_handle) {
            int len = ctxt->om->om_len < (int)sizeof(g_ble_stats_text) - 1 ? ctxt->om->om_len : (int)sizeof(g_ble_stats_text) - 1;
            memcpy(g_ble_stats_text, ctxt->om->om_data, len);
            g_ble_stats_text[len] = '\0';
            g_ble_stats_changed = true;
            ESP_LOGI(TAG, "Stats: %s", g_ble_stats_text);
        } else if (attr_handle == name_val_handle) {
            int len = ctxt->om->om_len < MAX_NAME_LEN ? ctxt->om->om_len : MAX_NAME_LEN;
            memcpy(s_current_peer_name, ctxt->om->om_data, len);
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
        g_ble_connected = false;
        ESP_LOGI(TAG, "Disconnected, reason=%d", event->disconnect.reason);
        ble_advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Encryption established");
            g_ble_connected = true;
            g_ble_pairing_mode = false;
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(conn_handle, &desc) == 0) {
                add_bond(desc.peer_id_addr.val, desc.peer_id_addr.type);
            }
        } else {
            ESP_LOGW(TAG, "Encryption failed, status=%d", event->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGI(TAG, "Repeat pairing requested");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
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
        os_mbuf *om = ble_hs_mbuf_from_flat(&value, 1);
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

    load_bonds();

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
    struct ble_store_key_sec key_sec;
    struct ble_store_key_cccd key_cccd;

    if (is_connected) {
        ble_gap_unpair(&peer_addr);
    }
    memset(&key_sec, 0, sizeof(key_sec));
    key_sec.peer_addr = peer_addr;
    ble_store_delete_peer_sec(&key_sec);
    memset(&key_cccd, 0, sizeof(key_cccd));
    key_cccd.peer_addr = peer_addr;
    ble_store_delete_cccd(&key_cccd);

    for (int i = index; i < s_bonded_count - 1; i++) {
        s_bonded_devices[i] = s_bonded_devices[i + 1];
    }
    s_bonded_count--;
    save_bonds();
}

void agent_ble_delete_all_bonds(void)
{
    while (s_bonded_count > 0) {
        agent_ble_delete_bond(0);
    }
}
