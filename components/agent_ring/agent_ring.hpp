#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_RING_SIZE 456
#define AGENT_RING_WIDTH 14
#define AGENT_RING_ROTATION 270

lv_obj_t *agent_ring_create(lv_obj_t *parent, int min_value, int max_value, int value);
void agent_ring_set_visible(lv_obj_t *ring, bool visible);

#ifdef __cplusplus
}
#endif
