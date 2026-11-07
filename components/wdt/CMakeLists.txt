idf_component_register(
    SRCS "wdt.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES board_config hal_gptimer hal_mcpwm safety drv_mpu6500 esp_timer esp_hw_support freertos
    LDFRAGMENTS "linker.lf")
