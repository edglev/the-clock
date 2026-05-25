idf_component_register(
    SRCS "agent_ble.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt
    PRIV_REQUIRES agent_viewer
)
