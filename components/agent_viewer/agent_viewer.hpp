#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*agent_viewer_pairing_cb_t)(bool accepted);

void agent_viewer_init(void);
void agent_viewer_show_pairing_modal(uint32_t passkey, agent_viewer_pairing_cb_t cb);
void agent_viewer_hide_pairing_modal(void);

#ifdef __cplusplus
}
#endif
