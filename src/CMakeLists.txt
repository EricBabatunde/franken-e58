idf_component_register(
    SRCS "main.cpp" "led_probe.cpp" "m2_i2c_test.cpp" "m3_imu_test.cpp"
    INCLUDE_DIRS "."
    REQUIRES board_config hal_gpio hal_i2c drv_mpu6500
    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_timer esp_system esp_hw_support heap)
