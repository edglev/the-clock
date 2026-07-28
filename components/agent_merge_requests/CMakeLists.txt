idf_component_register(
    SRCS "agent_merge_requests.cpp"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl agent_ble agent_ring
)
