#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "agent_pmic.hpp"
#include "agent_ble.hpp"
#include "agent_bambu.hpp"
#include "agent_gitlab.hpp"
#include "agent_features.hpp"
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

    ESP_LOGI(TAG, "Starting display");
    bsp_display_start();
    if (bsp_display_lock(1000) == ESP_OK) {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        bsp_display_unlock();
    }
    bsp_display_backlight_on();
    vTaskDelay(pdMS_TO_TICKS(100));

    agent_features_init();
    ESP_LOGI(TAG, "Starting BLE");
    agent_ble_init();
    ESP_LOGI(TAG, "Starting GitLab monitor");
    agent_gitlab_init();
    ESP_LOGI(TAG, "Starting Bambu cloud fallback");
    agent_bambu_init();
    ESP_LOGI(TAG, "Starting UI");
    agent_viewer_init();
    ESP_LOGI(TAG, "Starting PMIC");
    agent_pmic_init();
    ESP_LOGI(TAG, "Startup complete");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
