#include <cstdint>

#include "board_config.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_gpio.h"
#include "led_probe.h"
#include "m2_i2c_test.h"

static const char* TAG = "main";

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_USB:      return "USB";
        default:               return "other";
    }
}

extern "C" void app_main(void) {
    // 1. Safe state first: motor gates low before anything else runs.
    const esp_err_t gpio_err = hal_gpio::motor_pins_init_low();

    // 2. Banner.
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "Franken-E58 M1 boot. IDF %s, %d core(s), rev %d",
             esp_get_idf_version(), chip.cores, chip.revision);
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));
    ESP_LOGI(TAG, "motor pins %d,%d,%d,%d low: %s",
             board::kMotorGpio[0], board::kMotorGpio[1],
             board::kMotorGpio[2], board::kMotorGpio[3],
             gpio_err == ESP_OK ? "OK" : esp_err_to_name(gpio_err));
    ESP_LOGI(TAG, "heap: internal free %u B, PSRAM free %u B",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // 3. One-off LED probe (bring-up only).
#if FE_LED_PROBE
    led_probe_run();
#endif

#if FE_M2_TEST
    m2_start();
#endif

    // 4. Heartbeat: 1 Hz, microsecond timestamp from the 64-bit esp_timer.
    for (;;) {
        ESP_LOGI(TAG, "HB t_us=%lld internal_free=%u min_free=%u",
                 static_cast<long long>(esp_timer_get_time()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
