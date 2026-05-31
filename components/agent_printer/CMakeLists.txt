idf_component_register(
    SRCS
        "agent_printer.cpp"
        "assets/bambuicon.c"
        "assets/mdi_40.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl agent_ble agent_pmic agent_ring
)
