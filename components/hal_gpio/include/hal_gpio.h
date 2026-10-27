#pragma once
#include "esp_err.h"

namespace hal_gpio {

// Configure the four motor gate pins as outputs and drive them low.
// Must run before any other code touches these pins.
esp_err_t motor_pins_init_low();

}  // namespace hal_gpio
