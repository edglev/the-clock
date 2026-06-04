#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGENT_BAMBU_NOT_CONFIGURED = 0,
    AGENT_BAMBU_STANDBY,
    AGENT_BAMBU_WIFI_CONNECTING,
    AGENT_BAMBU_WIFI_CONNECTED,
    AGENT_BAMBU_CLOUD_CONNECTING,
    AGENT_BAMBU_CLOUD_CONNECTED,
    AGENT_BAMBU_ERROR,
} agent_bambu_state_t;

typedef struct {
    bool configured;
    bool wifi_connected;
    bool cloud_connected;
    bool fallback_active;
    agent_bambu_state_t state;
    char ssid[33];
    char ip[16];
    char printer[32];
    char detail[64];
    uint8_t wifi_count;
} agent_bambu_status_t;

#define AGENT_BAMBU_MAX_WIFI_NETWORKS 4

typedef struct {
    char ssid[33];
} agent_bambu_wifi_network_t;

void agent_bambu_init(void);
void agent_bambu_get_status(agent_bambu_status_t *out);
int agent_bambu_get_wifi_networks(agent_bambu_wifi_network_t *out, int max_count);
bool agent_bambu_delete_wifi_network(int index);

#ifdef __cplusplus
}
#endif
