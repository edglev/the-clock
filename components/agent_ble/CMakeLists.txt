idf_component_register(
    SRCS "agent_ble.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash mbedtls
    PRIV_REQUIRES agent_viewer
)
