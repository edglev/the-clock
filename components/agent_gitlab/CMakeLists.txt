idf_component_register(
    SRCS "agent_gitlab.cpp"
    INCLUDE_DIRS "."
    REQUIRES agent_ble agent_features esp_http_client esp_wifi nvs_flash cjson mbedtls
)
