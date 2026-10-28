idf_component_register(
    SRCS "hal_i2c.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_rom log)
