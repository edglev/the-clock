idf_component_register(
    SRCS "agent_viewer.cpp"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl agent_ble agent_pmic agent_settings agent_printer agent_ams agent_ring waveshare__esp32_s3_touch_amoled_1_75
)
