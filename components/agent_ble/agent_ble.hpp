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
#define AGENT_SVC_UUID    0x01, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define AGENT_STATE_CHAR  0x02, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define AGENT_STATS_CHAR  0x03, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define AGENT_ACTION_CHAR 0x04, 0x00, 0x90, 0xde, 0x67, 0x44, 0xf0, 0x42, 0x59, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

enum agent_state {
    AGENT_STATE_IDLE     = 0,
    AGENT_STATE_THINKING = 1,
    AGENT_STATE_WAITING  = 2,
    AGENT_STATE_SUCCESS  = 3,
};

extern uint8_t g_ble_state;
extern char    g_ble_stats_text[32];
extern bool    g_ble_connected;
extern bool    g_ble_stats_changed;

void agent_ble_init(void);
void agent_ble_notify_action(uint8_t value);

#ifdef __cplusplus
}
#endif
