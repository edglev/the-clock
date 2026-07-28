#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void agent_gitlab_init(void);
bool agent_gitlab_is_configured(void);

#ifdef __cplusplus
}
#endif
