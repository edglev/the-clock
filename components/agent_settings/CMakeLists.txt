idf_component_register(
    SRCS "agent_settings.cpp"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl agent_ble agent_bambu agent_features agent_pmic agent_orientation esp_system waveshare__esp32_s3_touch_amoled_1_75
)
