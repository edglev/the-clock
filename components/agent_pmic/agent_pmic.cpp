#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "agent_pmic.hpp"

static const char *TAG = "agent_pmic";

static i2c_master_dev_handle_t pmic_dev = NULL;

static esp_err_t pmic_read_reg(uint8_t reg, uint8_t *val)
{
    if (!pmic_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(pmic_dev, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static esp_err_t pmic_write_reg(uint8_t reg, uint8_t val)
{
    if (!pmic_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(pmic_dev, buf, 2, pdMS_TO_TICKS(100));
}

bool agent_pmic_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = { 0 },
    };

    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &pmic_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PMIC device: %d", ret);
        return false;
    }

    uint8_t chip_id = 0;
    ret = pmic_read_reg(0x03, &chip_id);
    if (ret != ESP_OK || chip_id != 0x4A) {
        ESP_LOGE(TAG, "AXP2101 not found (chip_id=0x%02x, ret=%d)", chip_id, ret);
        pmic_dev = NULL;
        return false;
    }
    ESP_LOGI(TAG, "AXP2101 detected (chip_id=0x%02x)", chip_id);

    pmic_write_reg(0x30, 0xFF);
    ESP_LOGI(TAG, "PMIC initialized");
    return true;
}

int agent_pmic_get_battery_percent(void)
{
    uint8_t raw = 0;
    if (pmic_read_reg(0xA4, &raw) != ESP_OK) {
        return -1;
    }
    return (int)raw;
}

bool agent_pmic_is_charging(void)
{
    uint8_t status = 0;
    if (pmic_read_reg(0x01, &status) != ESP_OK) {
        return false;
    }
    return (status & 0x20) != 0;
}
