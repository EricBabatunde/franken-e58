idf_component_register(
    SRCS "hal_mcpwm.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config esp_driver_mcpwm
    PRIV_REQUIRES hal soc esp_hw_support freertos)
