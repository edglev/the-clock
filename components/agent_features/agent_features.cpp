#include "nvs.h"
#include "agent_features.hpp"

#define NVS_NAMESPACE "features"

static bool s_enabled[AGENT_FEATURE_COUNT] = {true, true, true};
static const char *s_keys[AGENT_FEATURE_COUNT] = {"agents", "gitlab", "printer"};

void agent_features_init(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    for (int i = 0; i < AGENT_FEATURE_COUNT; i++) {
        uint8_t value = 1;
        if (nvs_get_u8(handle, s_keys[i], &value) == ESP_OK) {
            s_enabled[i] = value != 0;
        }
    }
    nvs_close(handle);
}

bool agent_feature_is_enabled(agent_feature_t feature)
{
    return feature >= 0 && feature < AGENT_FEATURE_COUNT && s_enabled[feature];
}

bool agent_feature_set_enabled(agent_feature_t feature, bool enabled)
{
    if (feature < 0 || feature >= AGENT_FEATURE_COUNT) return false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(handle, s_keys[feature], enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    return true;
}
