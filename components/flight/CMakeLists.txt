# Core 1 flight task (M7a: IMU read, safety step, output write). linker.lf places it in IRAM.
idf_component_register(
    SRCS "flight.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config drv_mpu6500 hal_i2c safety wdt
    PRIV_REQUIRES hal_mcpwm seqlock comms esp_timer esp_hw_support freertos
    LDFRAGMENTS "linker.lf")
