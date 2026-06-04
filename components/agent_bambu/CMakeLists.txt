idf_component_register(
    SRCS "agent_bambu.cpp"
    INCLUDE_DIRS "."
    REQUIRES agent_ble esp_wifi esp_netif esp_event nvs_flash mqtt cjson mbedtls esp_timer
)
