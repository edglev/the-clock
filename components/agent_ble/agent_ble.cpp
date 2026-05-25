#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_sm.h"
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
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

#define UUID_SVC  0x01, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define UUID_CHR1 0x02, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define UUID_CHR2 0x03, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define UUID_CHR3 0x04, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

static const ble_uuid128_t ble_svc_uuid = BLE_UUID128_INIT(UUID_SVC);
static const ble_uuid128_t ble_chr1_uuid = BLE_UUID128_INIT(UUID_CHR1);
static const ble_uuid128_t ble_chr2_uuid = BLE_UUID128_INIT(UUID_CHR2);
static const ble_uuid128_t ble_chr3_uuid = BLE_UUID128_INIT(UUID_CHR3);

static int ble_gatts_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_advertise(void);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const struct ble_gatt_chr_def ble_chars[] = {
    { .uuid = &ble_chr1_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, .val_handle = &state_val_handle },
    { .uuid = &ble_chr2_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_WRITE, .val_handle = &stats_val_handle },
    { .uuid = &ble_chr3_uuid.u, .access_cb = ble_gatts_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &action_val_handle },
    { 0 },
};

static const struct ble_gatt_svc_def ble_gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &ble_svc_uuid.u, .characteristics = (struct ble_gatt_chr_def *)ble_chars },
    { 0 },
};
#pragma GCC diagnostic pop

static int ble_gap_event(struct ble_gap_event *event, void *arg);

extern "C" void ble_store_config_init(void);

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
            conn_handle = event->connect.conn_handle;
            g_ble_connected = true;
            ESP_LOGI(TAG, "Connected");
        } else {
            g_ble_connected = false;
            ESP_LOGI(TAG, "Connect failed");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_ble_connected = false;
        ESP_LOGI(TAG, "Disconnected, reason=%d", event->disconnect.reason);
        ble_advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change, status=%d", event->enc_change.status);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGI(TAG, "Repeat pairing requested");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io resp;
        memset(&resp, 0, sizeof(resp));
        resp.action = event->passkey.params.action;
        ble_sm_inject_io(conn_handle, &resp);
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

    nimble_port_init();

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
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

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    ble_advertise();
}
