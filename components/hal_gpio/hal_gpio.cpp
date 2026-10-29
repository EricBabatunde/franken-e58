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

esp_err_t tof_xshut_init() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << board::kTofXshut;
    cfg.mode = GPIO_MODE_OUTPUT_OD;  // level 1 = high impedance, level 0 = driven low
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(static_cast<gpio_num_t>(board::kTofXshut), 0);
}

void tof_xshut_set(bool run) {
    gpio_set_level(static_cast<gpio_num_t>(board::kTofXshut), run ? 1 : 0);
}

esp_err_t imu_int_install(IsrHandler handler, void* arg) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << board::kImuInt;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;      // INT is driven push-pull (INT_PIN_CFG.OPEN = 0)
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_POSEDGE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_isr_handler_add(static_cast<gpio_num_t>(board::kImuInt), handler, arg);
}

}  // namespace hal_gpio
