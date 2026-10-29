idf_component_register(
    SRCS "drv_vl53l1x.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hal_i2c
    PRIV_REQUIRES board_config hal_gpio esp_timer esp_rom freertos)
