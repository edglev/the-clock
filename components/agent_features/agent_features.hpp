#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGENT_FEATURE_AGENTS = 0,
    AGENT_FEATURE_GITLAB,
    AGENT_FEATURE_PRINTER,
    AGENT_FEATURE_COUNT,
} agent_feature_t;

void agent_features_init(void);
bool agent_feature_is_enabled(agent_feature_t feature);
bool agent_feature_set_enabled(agent_feature_t feature, bool enabled);

#ifdef __cplusplus
}
#endif
