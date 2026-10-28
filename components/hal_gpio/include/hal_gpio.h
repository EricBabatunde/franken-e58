#pragma once
#include "esp_err.h"

namespace hal_gpio {

// Configure the four motor gate pins as outputs and drive them low.
// Must run before any other code touches these pins.
esp_err_t motor_pins_init_low();

// ToF XSHUT (open-drain). set(true) releases the line so the sensor runs;
// set(false) drives it low to hold the sensor in reset. init() starts in reset.
esp_err_t tof_xshut_init();
void tof_xshut_set(bool run);

}  // namespace hal_gpio
