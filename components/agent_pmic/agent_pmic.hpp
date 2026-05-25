#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AXP2101_I2C_ADDR 0x34

bool agent_pmic_init(void);
int  agent_pmic_get_battery_percent(void);
bool agent_pmic_is_charging(void);

#ifdef __cplusplus
}
#endif
