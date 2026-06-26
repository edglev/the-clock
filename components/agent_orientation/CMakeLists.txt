idf_component_register(
    SRCS "agent_orientation.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c waveshare__esp32_s3_touch_amoled_1_75
)
