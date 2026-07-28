#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void agent_merge_requests_create(lv_obj_t *tile);
void agent_merge_requests_timer_update(void);
void agent_merge_requests_set_ring_visible(bool visible);
void agent_merge_requests_mark_seen(void);
bool agent_merge_requests_has_unseen(void);
bool agent_merge_requests_alert_on(void);

#ifdef __cplusplus
}
#endif
