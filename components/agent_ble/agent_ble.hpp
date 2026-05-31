#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// BLE UUIDs (as reported by Bleak on Windows):
// Service: 00000000-0000-a359-42f0-4467de900001
// State (Write):  00000000-0000-a359-42f0-4467de900002  0=IDLE, 1=THINKING, 2=WAITING, 3=SUCCESS
// Stats (Write):  00000000-0000-a359-42f0-4467de900003  UTF-8 string, max ~24 chars
// Action (Notify): 00000000-0000-a359-42f0-4467de900004  1 byte notification (touch ack)
// Name (Write):   00000000-0000-a359-42f0-4467de900005  Peer device name, max 32 chars
// Multi (Write):  00000000-0000-a359-42f0-4467de900006
//   U\tid\tstate\tlabel\tstatus\tprovider\tbranch\tmetrics\tmodel\teffort
//   D\tid
//   P\tstate\tprogress\teta_s\tlayer\tlayers\tnozzle_c\tbed_c\tchamber_c\tjob\tmaterial\tsource\tupdated_ms
#define AGENT_SVC_UUID    0x01, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define AGENT_STATE_CHAR  0x02, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define AGENT_STATS_CHAR  0x03, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define AGENT_ACTION_CHAR 0x04, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define AGENT_NAME_CHAR   0x05, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define AGENT_MULTI_CHAR  0x06, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

#define AGENT_MAX_INSTANCES 16
#define AGENT_INSTANCE_ID_LEN 8
#define AGENT_INSTANCE_LABEL_LEN 32
#define AGENT_INSTANCE_STATUS_LEN 32
#define AGENT_INSTANCE_PROVIDER_LEN 12
#define AGENT_INSTANCE_BRANCH_LEN 24
#define AGENT_INSTANCE_METRICS_LEN 20
#define AGENT_INSTANCE_MODEL_LEN 24
#define AGENT_INSTANCE_EFFORT_LEN 12
#define AGENT_PRINTER_STATE_LEN 16
#define AGENT_PRINTER_JOB_LEN 48
#define AGENT_PRINTER_MATERIAL_LEN 24
#define AGENT_PRINTER_TEMP_LEN 8
#define AGENT_PRINTER_SOURCE_LEN 16

enum agent_state {
    AGENT_STATE_IDLE     = 0,
    AGENT_STATE_THINKING = 1,
    AGENT_STATE_WAITING  = 2,
    AGENT_STATE_SUCCESS  = 3,
};

typedef struct {
    char id[AGENT_INSTANCE_ID_LEN + 1];
    char label[AGENT_INSTANCE_LABEL_LEN + 1];
    char status[AGENT_INSTANCE_STATUS_LEN + 1];
    char provider[AGENT_INSTANCE_PROVIDER_LEN + 1];
    char branch[AGENT_INSTANCE_BRANCH_LEN + 1];
    char metrics[AGENT_INSTANCE_METRICS_LEN + 1];
    char model[AGENT_INSTANCE_MODEL_LEN + 1];
    char effort[AGENT_INSTANCE_EFFORT_LEN + 1];
    uint8_t state;
    uint32_t updated_ms;
    uint32_t priority_entered_ms;
} agent_instance_info_t;

typedef struct {
    char state[AGENT_PRINTER_STATE_LEN + 1];
    char job[AGENT_PRINTER_JOB_LEN + 1];
    char material[AGENT_PRINTER_MATERIAL_LEN + 1];
    char nozzle_c[AGENT_PRINTER_TEMP_LEN + 1];
    char bed_c[AGENT_PRINTER_TEMP_LEN + 1];
    char chamber_c[AGENT_PRINTER_TEMP_LEN + 1];
    char source[AGENT_PRINTER_SOURCE_LEN + 1];
    int16_t progress_percent;
    int32_t eta_seconds;
    int16_t layer;
    int16_t layers;
    uint32_t updated_ms;
    bool valid;
} agent_printer_status_t;

extern uint8_t g_ble_state;
extern char    g_ble_stats_text[32];
extern bool    g_ble_connected;
extern bool    g_ble_stats_changed;

void agent_ble_init(void);
void agent_ble_notify_action(uint8_t value);
int  agent_ble_get_instances(agent_instance_info_t *out, int max_count);
bool agent_ble_get_focused_instance(agent_instance_info_t *out);
int  agent_ble_get_instance_count(void);
bool agent_ble_get_printer_status(agent_printer_status_t *out);
int  agent_ble_get_bond_count(void);
bool agent_ble_get_bond(int index, uint8_t addr[6]);
const char *agent_ble_get_bond_name(int index);
void agent_ble_delete_bond(int index);
void agent_ble_delete_all_bonds(void);
bool agent_ble_is_pairing_mode(void);
void agent_ble_enable_pairing_mode(void);
void agent_ble_disable_pairing_mode(void);

#ifdef __cplusplus
}
#endif
