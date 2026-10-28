idf_component_register(
    SRCS "hal_gpio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config
    PRIV_REQUIRES esp_driver_gpio)
