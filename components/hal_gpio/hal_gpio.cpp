#include "hal_gpio.h"

#include <cstdint>

#include "board_config.h"
#include "driver/gpio.h"

namespace hal_gpio {

esp_err_t motor_pins_init_low() {
    uint64_t mask = 0;
    for (int pin : board::kMotorGpio) {
        mask |= (1ULL << pin);
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;     // never pull a gate high
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;  // belt and braces beside the 10 kΩ
    cfg.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    for (int pin : board::kMotorGpio) {
        err = gpio_set_level(static_cast<gpio_num_t>(pin), 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

}  // namespace hal_gpio
