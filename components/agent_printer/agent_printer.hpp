#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void agent_printer_create(lv_obj_t *tile);
void agent_printer_timer_update(void);
void agent_printer_set_ring_visible(bool visible);

#ifdef __cplusplus
}
#endif
