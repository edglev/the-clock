#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool agent_orientation_init(void);
void agent_orientation_timer_update(void);
int agent_orientation_get_angle_deg(void);
bool agent_orientation_is_locked(void);
void agent_orientation_set_locked(bool locked);

#ifdef __cplusplus
}
#endif
