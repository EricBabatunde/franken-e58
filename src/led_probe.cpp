#include "led_probe.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "led_probe";

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// One WS2812 frame via the RMT peripheral (addressable RGB LED).
// Timings at 10 MHz (datasheet centre values): bit0 = 0.4 us high / 0.9 us low,
// bit1 = 0.8 us high / 0.5 us low.  // verify against datasheet
static bool ws2812_set(gpio_num_t pin, uint8_t r, uint8_t g, uint8_t b) {
    rmt_channel_handle_t ch = nullptr;
    rmt_encoder_handle_t enc = nullptr;

    rmt_tx_channel_config_t cc = {};
    cc.gpio_num = pin;
    cc.clk_src = RMT_CLK_SRC_DEFAULT;
    cc.resolution_hz = 10 * 1000 * 1000;
    cc.mem_block_symbols = 64;
    cc.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&cc, &ch) != ESP_OK) {
        return false;
    }

    rmt_bytes_encoder_config_t ec = {};
    ec.bit0.level0 = 1; ec.bit0.duration0 = 4;
    ec.bit0.level1 = 0; ec.bit0.duration1 = 9;
    ec.bit1.level0 = 1; ec.bit1.duration0 = 8;
    ec.bit1.level1 = 0; ec.bit1.duration1 = 5;
    ec.flags.msb_first = 1;
    if (rmt_new_bytes_encoder(&ec, &enc) != ESP_OK) {
        rmt_del_channel(ch);
        return false;
    }

    bool ok = (rmt_enable(ch) == ESP_OK);
    if (ok) {
        const uint8_t grb[3] = {g, r, b};
        rmt_transmit_config_t tc = {};
        ok = (rmt_transmit(ch, enc, grb, sizeof(grb), &tc) == ESP_OK) &&
             (rmt_tx_wait_all_done(ch, 100) == ESP_OK);
        rmt_disable(ch);
    }
    rmt_del_encoder(enc);
    rmt_del_channel(ch);
    return ok;
}

void led_probe_run() {
    for (int pin_num : board::kLedCandidates) {
        const gpio_num_t pin = static_cast<gpio_num_t>(pin_num);

        ESP_LOGI(TAG, "PROBE: WS2812 on GPIO %d (red, green, blue, white, off)", pin_num);
        const uint8_t colours[5][3] = {
            {32, 0, 0}, {0, 32, 0}, {0, 0, 32}, {32, 32, 32}, {0, 0, 0}};
        for (const auto& c : colours) {
            if (!ws2812_set(pin, c[0], c[1], c[2])) {
                ESP_LOGW(TAG, "WS2812 transmit failed on GPIO %d", pin_num);
                break;
            }
            delay_ms(600);
        }
        gpio_reset_pin(pin);
        delay_ms(500);
    }
    ESP_LOGI(TAG, "PROBE DONE: report which GPIO lit the LED (or none)");
}
