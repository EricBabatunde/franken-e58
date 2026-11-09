# linker.lf: R5 experiment (b), M7c; maps nothing unless CONFIG_FE_R5_I2C_IRAM=y.
idf_component_register(
    SRCS "hal_i2c.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_rom log
    LDFRAGMENTS "linker.lf")
