idf_component_register(
    SRCS "main.cpp" "led_probe.cpp"
    INCLUDE_DIRS "."
    REQUIRES board_config hal_gpio
    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_timer esp_system esp_hw_support heap)
