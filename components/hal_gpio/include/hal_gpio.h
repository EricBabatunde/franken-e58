#pragma once
#include "esp_err.h"

namespace hal_gpio {

// Configure the four motor gate pins as outputs and drive them low.
// Must run before any other code touches these pins.
esp_err_t motor_pins_init_low();

// ToF XSHUT (open-drain). set(true) releases the line so the sensor runs;
// set(false) drives it low to hold the sensor in reset. init() starts in reset.
// level() reads the pad (0 or 1), configured or not; it only enables the pin's input.
esp_err_t tof_xshut_init();
void tof_xshut_set(bool run);
int tof_xshut_level();

// IMU data-ready (INT pin, push-pull, active high): input, rising-edge interrupt.
// Installs the GPIO ISR service with ESP_INTR_FLAG_IRAM, so the handler must be IRAM_ATTR.
// The service is allocated on the calling core: call this from the task pinned to the core
// that should take the interrupt. The interrupt is enabled on return.
using IsrHandler = void (*)(void*);
esp_err_t imu_int_install(IsrHandler handler, void* arg);

// Bench fault injection (M7a, missing data-ready). Call from the core that installed the
// interrupt: the enable acts on the calling core. Disable: gpio_intr_disable(), in IRAM, no lock.
// Enable: gpio_intr_enable(), which takes the GPIO driver's spinlock (gpio_context.gpio_spinlock,
// also taken by GPIO configuration calls on Core 0); never used while ARMED.
void imu_int_enable(bool enabled);

}  // namespace hal_gpio
