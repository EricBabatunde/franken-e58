idf_component_register(
    SRCS "drv_mpu6500.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config hal_i2c freertos
    PRIV_REQUIRES hal_gpio esp_timer esp_hw_support)
