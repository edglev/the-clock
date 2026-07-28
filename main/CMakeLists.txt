idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES agent_ble agent_features agent_gitlab agent_bambu agent_pmic agent_viewer nvs_flash
)
