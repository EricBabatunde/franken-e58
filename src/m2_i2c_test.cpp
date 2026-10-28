#include "m2_i2c_test.h"

#include <atomic>
#include <cstdint>

#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_gpio.h"
#include "hal_i2c.h"

namespace {

const char* TAG = "m2";
using hal_i2c::Bus;

// MPU-6050 registers (verify against the datasheet and register map)
constexpr uint8_t kMpuRegWhoAmI = 0x75;      // expect 0x68
constexpr uint8_t kMpuRegAccelXoutH = 0x3B;  // start of the 14-byte burst
constexpr uint8_t kMpuWhoAmI = 0x68;

// VL53L1X identification (verify against the datasheet and ST ULD API)
constexpr uint16_t kTofRegModelId = 0x010F;  // expect 0xEA then 0xCC
constexpr uint8_t kTofModelId0 = 0xEA;
constexpr uint8_t kTofModelId1 = 0xCC;

constexpr int kSoakSeconds = 600;
constexpr int kReportEverySeconds = 10;
constexpr int kTimeoutMs = 10;
constexpr uint32_t kRecoverAfter = 5;  // consecutive failures before a recovery

struct Stats {
    std::atomic<uint32_t> ok{0};
    std::atomic<uint32_t> err{0};
    std::atomic<uint32_t> recoveries{0};
    std::atomic<uint32_t> mismatch{0};
    std::atomic<uint32_t> dur_min{UINT32_MAX};
    std::atomic<uint32_t> dur_max{0};
    std::atomic<uint64_t> dur_sum{0};
    std::atomic<uint32_t> per_min{UINT32_MAX};
    std::atomic<uint32_t> per_max{0};
};

Stats g_a;
Stats g_b;
hal_i2c::Device g_mpu{};
hal_i2c::Device g_tof{};

constexpr auto kRelaxed = std::memory_order_relaxed;

void record(Stats& s, uint32_t dur, esp_err_t e) {
    (e == ESP_OK ? s.ok : s.err).fetch_add(1, kRelaxed);
    if (dur < s.dur_min.load(kRelaxed)) s.dur_min.store(dur, kRelaxed);
    if (dur > s.dur_max.load(kRelaxed)) s.dur_max.store(dur, kRelaxed);
    s.dur_sum.fetch_add(dur, kRelaxed);
}

void record_period(Stats& s, uint32_t p) {
    if (p < s.per_min.load(kRelaxed)) s.per_min.store(p, kRelaxed);
    if (p > s.per_max.load(kRelaxed)) s.per_max.store(p, kRelaxed);
}

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }

unsigned avg_us(const Stats& s) {
    const uint32_t n = s.ok.load(kRelaxed) + s.err.load(kRelaxed);
    return n ? static_cast<unsigned>(s.dur_sum.load(kRelaxed) / n) : 0u;
}

void scan(Bus bus, const char* name) {
    int found = 0;
    for (int a = 0x08; a <= 0x77; ++a) {
        if (hal_i2c::probe(bus, static_cast<uint8_t>(a), 20) == ESP_OK) {
            ESP_LOGI(TAG, "scan %s: device at 0x%02X", name, a);
            ++found;
        }
    }
    ESP_LOGI(TAG, "scan %s done: %d device(s)", name, found);
}

bool selftest_bus(Bus bus, const char* name, uint8_t addr) {
    const esp_err_t r = hal_i2c::recover(bus);
    const esp_err_t p = hal_i2c::probe(bus, addr, 20);
    ESP_LOGI(TAG, "recovery self-test bus %s: recover=%s probe=%s", name,
             esp_err_to_name(r), esp_err_to_name(p));
    return r == ESP_OK && p == ESP_OK;
}

// Bus A: 14-byte burst at 1 kHz, the same transfer the flight loop will use.
void soak_a_task(void*) {
    uint8_t buf[14];
    uint32_t consecutive = 0;
    int64_t prev = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        const int64_t t0 = esp_timer_get_time();
        if (prev != 0) record_period(g_a, static_cast<uint32_t>(t0 - prev));
        prev = t0;

        const esp_err_t e = hal_i2c::read_reg8(g_mpu, kMpuRegAccelXoutH, buf, sizeof(buf), kTimeoutMs);
        record(g_a, static_cast<uint32_t>(esp_timer_get_time() - t0), e);

        if (e != ESP_OK) {
            if (++consecutive >= kRecoverAfter) {
                hal_i2c::recover(Bus::A);
                g_a.recoveries.fetch_add(1, kRelaxed);
                consecutive = 0;
                prev = 0;
                wake = xTaskGetTickCount();
            }
        } else {
            consecutive = 0;
        }
        vTaskDelayUntil(&wake, 1);  // 1 tick = 1 ms at CONFIG_FREERTOS_HZ=1000
    }
}

// Bus B: 2-byte ID read at 50 Hz, the rate the ToF will run at.
void soak_b_task(void*) {
    uint8_t id[2];
    uint32_t consecutive = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t e = hal_i2c::read_reg16(g_tof, kTofRegModelId, id, sizeof(id), kTimeoutMs);
        record(g_b, static_cast<uint32_t>(esp_timer_get_time() - t0), e);

        if (e != ESP_OK) {
            if (++consecutive >= kRecoverAfter) {
                hal_i2c::recover(Bus::B);
                g_b.recoveries.fetch_add(1, kRelaxed);
                consecutive = 0;
                wake = xTaskGetTickCount();
            }
        } else {
            consecutive = 0;
            if (id[0] != kTofModelId0 || id[1] != kTofModelId1) {
                g_b.mismatch.fetch_add(1, kRelaxed);
            }
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
}

void print_stats(int t) {
    ESP_LOGI(TAG,
             "t=%3ds | A ok=%u err=%u rec=%u dur_us min/avg/max=%u/%u/%u period_us min/max=%u/%u"
             " | B ok=%u err=%u rec=%u mism=%u dur_us min/avg/max=%u/%u/%u",
             t, u(g_a.ok), u(g_a.err), u(g_a.recoveries), u(g_a.dur_min), avg_us(g_a),
             u(g_a.dur_max), u(g_a.per_min), u(g_a.per_max), u(g_b.ok), u(g_b.err),
             u(g_b.recoveries), u(g_b.mismatch), u(g_b.dur_min), avg_us(g_b), u(g_b.dur_max));
}

void report_task(void*) {
    for (int t = kReportEverySeconds; t <= kSoakSeconds; t += kReportEverySeconds) {
        vTaskDelay(pdMS_TO_TICKS(kReportEverySeconds * 1000));
        print_stats(t);
    }
    const bool pass = g_a.err.load() == 0 && g_b.err.load() == 0 && g_b.mismatch.load() == 0;
    ESP_LOGI(TAG, "SOAK RESULT: %s (A err=%u, B err=%u, ID mismatches=%u, recoveries A/B=%u/%u)",
             pass ? "PASS" : "FAIL", u(g_a.err), u(g_b.err), u(g_b.mismatch),
             u(g_a.recoveries), u(g_b.recoveries));
    ESP_LOGI(TAG, "Soak tasks keep running. Reset the board for a fresh run.");
    vTaskDelete(nullptr);
}

}  // namespace

void m2_start() {
    // 1. Buses.
    esp_err_t e = hal_i2c::init(Bus::A, board::kImuSda, board::kImuScl, board::kI2cHz);
    ESP_LOGI(TAG, "init bus A (I2C0, SDA=%d SCL=%d): %s", board::kImuSda, board::kImuScl, esp_err_to_name(e));
    e = hal_i2c::init(Bus::B, board::kTofSda, board::kTofScl, board::kI2cHz);
    ESP_LOGI(TAG, "init bus B (I2C1, SDA=%d SCL=%d): %s", board::kTofSda, board::kTofScl, esp_err_to_name(e));

    // 2. ToF out of reset: low for 10 ms, release, wait for boot.
    hal_gpio::tof_xshut_init();
    vTaskDelay(pdMS_TO_TICKS(10));
    hal_gpio::tof_xshut_set(true);
    vTaskDelay(pdMS_TO_TICKS(20));

    // 3. Scan.
    scan(Bus::A, "A");
    scan(Bus::B, "B");

    // 4. Devices and IDs.
    const bool mpu_present = hal_i2c::probe(Bus::A, board::kMpuAddr, 20) == ESP_OK;
    const bool tof_present = hal_i2c::probe(Bus::B, board::kTofAddr, 20) == ESP_OK;
    if (!mpu_present) {
        ESP_LOGE(TAG, "MPU-6050 not found at 0x%02X. Check 3V3, GND, SDA/SCL on GPIO %d/%d, AD0 to GND, pull-ups.",
                 board::kMpuAddr, board::kImuSda, board::kImuScl);
    }
    if (!tof_present) {
        ESP_LOGE(TAG, "VL53L1X not found at 0x%02X. Check wiring on GPIO %d/%d, and measure XSHUT (GPIO %d) to VIN: "
                      "if open, add a 10 kOhm pull-up from XSHUT to 3V3.",
                 board::kTofAddr, board::kTofSda, board::kTofScl, board::kTofXshut);
    }
    if (!mpu_present || !tof_present) {
        ESP_LOGE(TAG, "M2 stopped: fix wiring and reset. Soak not started.");
        return;
    }
    hal_i2c::add_device(Bus::A, board::kMpuAddr, &g_mpu);
    hal_i2c::add_device(Bus::B, board::kTofAddr, &g_tof);

    uint8_t who = 0;
    e = hal_i2c::read_reg8(g_mpu, kMpuRegWhoAmI, &who, 1, kTimeoutMs);
    ESP_LOGI(TAG, "MPU WHO_AM_I = 0x%02X (%s)", who,
             (e == ESP_OK && who == kMpuWhoAmI) ? "OK" : "UNEXPECTED");

    uint8_t id[2] = {};
    e = hal_i2c::read_reg16(g_tof, kTofRegModelId, id, sizeof(id), kTimeoutMs);
    ESP_LOGI(TAG, "ToF model ID = 0x%02X%02X (%s)", id[0], id[1],
             (e == ESP_OK && id[0] == kTofModelId0 && id[1] == kTofModelId1) ? "OK" : "UNEXPECTED");

    // 5. Recovery self-test.
    const bool rec_ok = selftest_bus(Bus::A, "A", board::kMpuAddr) &&
                        selftest_bus(Bus::B, "B", board::kTofAddr);
    ESP_LOGI(TAG, "recovery self-test: %s", rec_ok ? "PASS" : "FAIL");

    // 6. Soak.
    ESP_LOGI(TAG, "starting %d s soak at %u Hz", kSoakSeconds, u(board::kI2cHz));
    xTaskCreatePinnedToCore(soak_a_task, "soak_a", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
    xTaskCreatePinnedToCore(soak_b_task, "soak_b", 4096, nullptr, 6, nullptr, 0);
    xTaskCreatePinnedToCore(report_task, "m2_report", 4096, nullptr, 3, nullptr, 0);
}
