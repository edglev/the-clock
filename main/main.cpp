#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "agent_pmic.hpp"
#include "agent_ble.hpp"
#include "agent_viewer.hpp"

static const char *TAG = "agent-viewer";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting Agent Viewer");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bsp_display_start();
    bsp_display_backlight_on();

    agent_pmic_init();
    agent_ble_init();
    agent_viewer_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
