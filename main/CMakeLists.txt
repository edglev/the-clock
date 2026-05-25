idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES agent_ble agent_pmic agent_viewer nvs_flash
)
