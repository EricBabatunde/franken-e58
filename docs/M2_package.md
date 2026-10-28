# Franken-E58 Phase 1, Module 2: Dual-Bus I²C Scan, Identification, Recovery and 10-Minute Soak

**Scope:** wire both sensors to their own I²C buses, add a `hal_i2c` component, scan both buses, read each sensor's ID, prove a bus-recovery routine, and soak both buses at 400 kHz for 10 minutes. No sensor configuration yet (that is M3 and M4). No motors, no Wi-Fi.

**Not compiled.** Same caveat as M1: Claude Code builds it first and reports every API mismatch. Anything marked *verify* is recalled from memory and must be checked against the datasheet or the ESP-IDF 5.5.3 docs.

**Prerequisites (do these first)**

1. `pio --version` works and `pio run` builds on the pinned platform (`espressif32 @ 6.13.0`, ESP-IDF 5.5.3).
2. The M1 follow-up (Task 1) is applied: split component names (`esp_driver_*`), DIO flash mode, no `memory_type` line.
3. The multimeter check on GPIO 4 to 7 (about 0 V) is done.
4. Set `FE_LED_PROBE` to 0 in `board_config.h` once the LED question is closed, so every boot is not delayed by 10 s.

---

## 1. Goals

1. Both sensors respond on their own bus: MPU-6050 at 0x68 on I2C0 (GPIO 1/2), VL53L1X at 0x29 on I2C1 (GPIO 11/12).
2. Each sensor's ID register reads back correctly.
3. A recovery routine (tear down, clock SCL up to 9 times, STOP, rebuild) restores a stuck or glitched bus without a reboot.
4. A 10-minute soak at 400 kHz shows zero errors on both buses.
5. The real I²C burst time on Bus A is measured and compared with the blueprint's 400 µs estimate (§4.3).

## 2. Hardware: wiring and checks

**Power off and unplug USB before wiring.** No battery in M2. Keep wires under about 15 cm and keep SDA and SCL apart.

| ESP32-S3 | Device pin | Notes |
|---|---|---|
| 3V3 | MPU-6500 VCC, VL53L1X VIN | Sensors draw well under 30 mA |
| GND | MPU-6500 GND, VL53L1X GND | One common ground rail |
| GPIO 1 | MPU-6500 SDA | 4.7 kΩ to 3V3 |
| GPIO 2 | MPU-6500 SCL | 4.7 kΩ to 3V3 |
| GPIO 9 | MPU-6500 INT | Wire now, used in M3 |
| MPU-6500 AD0 | GND | Forces address 0x68 |
| GPIO 11 | VL53L1X SDA | 4.7 kΩ to 3V3 |
| GPIO 12 | VL53L1X SCL | 4.7 kΩ to 3V3 |
| GPIO 8 | VL53L1X XSHUT | Driven open-drain: low resets, released runs |
| GPIO 13 | VL53L1X GPIO1 | Optional, wire now for M4 |

Add 100 nF and 10 µF across 3V3 and GND close to each sensor.

**Multimeter checks, USB unplugged:**

| Check | Expected |
|---|---|
| 3V3 to GND | Not a short (well above 1 kΩ) |
| GPIO 1 line and GPIO 2 line to 3V3 | About 4.7 kΩ, or about 2.35 kΩ if the MPU breakout has its own 4.7 kΩ pull-ups |
| GPIO 11 line and GPIO 12 line to 3V3 | About 3.2 kΩ (10 kΩ on the ToF board in parallel with 4.7 kΩ) |

**After plugging in USB:** 3V3 rail about 3.3 V, and each SDA and SCL line idles at about 3.3 V.

## 3. Software

### 3.1 New files (already in the overlay zip)

```
components/hal_i2c/
├── CMakeLists.txt
├── include/hal_i2c.h
└── hal_i2c.cpp
src/
├── m2_i2c_test.h
└── m2_i2c_test.cpp          (bench test, deleted before Module 7)
docs/M2_package.md           (this file)
```

### `components/hal_i2c/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "hal_i2c.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_rom log)
```

### `components/hal_i2c/include/hal_i2c.h`

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Thin wrapper over the ESP-IDF 5.x I2C master driver (driver/i2c_master.h).
// Not thread-safe: exactly one task owns each bus.
namespace hal_i2c {

enum class Bus : uint8_t { A = 0, B = 1 };  // A = I2C0 (IMU), B = I2C1 (ToF)

// Opaque device identifier. Stays valid across recover().
struct Device {
    uint8_t bus;
    uint8_t slot;
};

esp_err_t init(Bus bus, int sda_gpio, int scl_gpio, uint32_t scl_hz);
esp_err_t add_device(Bus bus, uint8_t addr7, Device* out);
esp_err_t probe(Bus bus, uint8_t addr7, int timeout_ms);

// reg8: one-byte register address (MPU-6050). reg16: two-byte big-endian address (VL53L1X).
esp_err_t read_reg8(Device d, uint8_t reg, uint8_t* buf, size_t len, int timeout_ms);
esp_err_t read_reg16(Device d, uint16_t reg, uint8_t* buf, size_t len, int timeout_ms);
esp_err_t write_reg8(Device d, uint8_t reg, uint8_t value, int timeout_ms);

// Bus recovery: removes devices and the bus, clocks SCL up to 9 times until SDA
// releases, issues a STOP, then rebuilds the bus and re-adds the same devices.
esp_err_t recover(Bus bus);

}  // namespace hal_i2c
```

### `components/hal_i2c/hal_i2c.cpp`

```cpp
#include "hal_i2c.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace hal_i2c {
namespace {

const char* TAG = "hal_i2c";
constexpr int kMaxDevicesPerBus = 2;

struct BusState {
    bool inited = false;
    int sda = -1;
    int scl = -1;
    uint32_t hz = 0;
    i2c_master_bus_handle_t handle = nullptr;
    uint8_t n_dev = 0;
    uint8_t addr[kMaxDevicesPerBus] = {};
    i2c_master_dev_handle_t dev[kMaxDevicesPerBus] = {};
};

BusState g_bus[2];

BusState& state(Bus b) { return g_bus[static_cast<int>(b)]; }

esp_err_t create_bus(BusState& b, int port) {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = static_cast<i2c_port_num_t>(port);
    cfg.sda_io_num = static_cast<gpio_num_t>(b.sda);
    cfg.scl_io_num = static_cast<gpio_num_t>(b.scl);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = false;  // external 4.7 kOhm pull-ups only
    return i2c_new_master_bus(&cfg, &b.handle);
}

esp_err_t create_device(BusState& b, uint8_t slot) {
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = b.addr[slot];
    dc.scl_speed_hz = b.hz;
    return i2c_master_bus_add_device(b.handle, &dc, &b.dev[slot]);
}

// Releases a slave that is holding SDA low mid-byte, then issues a STOP.
void bitbang_clear(int sda, int scl) {
    gpio_config_t c = {};
    c.intr_type = GPIO_INTR_DISABLE;
    c.pull_up_en = GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;

    c.pin_bit_mask = 1ULL << scl;
    c.mode = GPIO_MODE_OUTPUT_OD;
    gpio_config(&c);
    c.pin_bit_mask = 1ULL << sda;
    c.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    gpio_config(&c);

    const gpio_num_t sda_pin = static_cast<gpio_num_t>(sda);
    const gpio_num_t scl_pin = static_cast<gpio_num_t>(scl);
    gpio_set_level(scl_pin, 1);
    gpio_set_level(sda_pin, 1);
    esp_rom_delay_us(10);

    for (int i = 0; i < 9 && gpio_get_level(sda_pin) == 0; ++i) {
        gpio_set_level(scl_pin, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl_pin, 1);
        esp_rom_delay_us(5);
    }

    // STOP condition: SDA low to high while SCL is high.
    gpio_set_level(sda_pin, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda_pin, 1);
    esp_rom_delay_us(5);
}

}  // namespace

esp_err_t init(Bus bus, int sda_gpio, int scl_gpio, uint32_t scl_hz) {
    BusState& b = state(bus);
    if (b.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    b.sda = sda_gpio;
    b.scl = scl_gpio;
    b.hz = scl_hz;
    const esp_err_t err = create_bus(b, static_cast<int>(bus));
    b.inited = (err == ESP_OK);
    return err;
}

esp_err_t add_device(Bus bus, uint8_t addr7, Device* out) {
    BusState& b = state(bus);
    if (!b.inited || b.n_dev >= kMaxDevicesPerBus || out == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t slot = b.n_dev;
    b.addr[slot] = addr7;
    const esp_err_t err = create_device(b, slot);
    if (err != ESP_OK) {
        return err;
    }
    b.n_dev++;
    out->bus = static_cast<uint8_t>(bus);
    out->slot = slot;
    return ESP_OK;
}

esp_err_t probe(Bus bus, uint8_t addr7, int timeout_ms) {
    BusState& b = state(bus);
    if (!b.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(b.handle, addr7, timeout_ms);
}

esp_err_t read_reg8(Device d, uint8_t reg, uint8_t* buf, size_t len, int timeout_ms) {
    BusState& b = g_bus[d.bus];
    return i2c_master_transmit_receive(b.dev[d.slot], &reg, 1, buf, len, timeout_ms);
}

esp_err_t read_reg16(Device d, uint16_t reg, uint8_t* buf, size_t len, int timeout_ms) {
    BusState& b = g_bus[d.bus];
    const uint8_t a[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)};
    return i2c_master_transmit_receive(b.dev[d.slot], a, sizeof(a), buf, len, timeout_ms);
}

esp_err_t write_reg8(Device d, uint8_t reg, uint8_t value, int timeout_ms) {
    BusState& b = g_bus[d.bus];
    const uint8_t tx[2] = {reg, value};
    return i2c_master_transmit(b.dev[d.slot], tx, sizeof(tx), timeout_ms);
}

esp_err_t recover(Bus bus) {
    BusState& b = state(bus);
    if (!b.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "recovering bus %d (SDA=%d SCL=%d)", static_cast<int>(bus), b.sda, b.scl);

    for (uint8_t i = 0; i < b.n_dev; ++i) {
        if (b.dev[i] != nullptr) {
            i2c_master_bus_rm_device(b.dev[i]);
            b.dev[i] = nullptr;
        }
    }
    if (b.handle != nullptr) {
        i2c_del_master_bus(b.handle);
        b.handle = nullptr;
    }

    bitbang_clear(b.sda, b.scl);

    esp_err_t err = create_bus(b, static_cast<int>(bus));
    if (err != ESP_OK) {
        b.inited = false;
        return err;
    }
    for (uint8_t i = 0; i < b.n_dev; ++i) {
        err = create_device(b, i);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

}  // namespace hal_i2c
```

### `src/m2_i2c_test.h`

```cpp
#pragma once
// Module 2 bench test. Not part of the flight stack. Delete before Module 7.
// Scans both buses, reads each sensor's ID, runs a recovery self-test, then starts a
// 10-minute soak (Bus A at 1 kHz on Core 1, Bus B at 50 Hz on Core 0) and a reporter.
void m2_start();
```

### `src/m2_i2c_test.cpp`

```cpp
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
```

### 3.2 Modifications to existing files

**`components/board_config/include/board_config.h`**: add `#include <cstdint>` after `#pragma once`. Inside `namespace board`, add:

```cpp
// I2C (both buses)
constexpr uint32_t kI2cHz = 400000;  // Fast-mode, the MPU-6050 maximum
constexpr uint8_t kMpuAddr = 0x68;   // AD0 low
constexpr uint8_t kTofAddr = 0x29;   // VL53L1X default
```

After the `FE_LED_PROBE` define, add:

```cpp
// Set to 0 to skip the Module 2 bench test.
#define FE_M2_TEST 1
```

**`components/hal_gpio/include/hal_gpio.h`**: inside `namespace hal_gpio`, add:

```cpp
// ToF XSHUT (open-drain). set(true) releases the line so the sensor runs;
// set(false) drives it low to hold the sensor in reset. init() starts in reset.
esp_err_t tof_xshut_init();
void tof_xshut_set(bool run);
```

**`components/hal_gpio/hal_gpio.cpp`**: inside `namespace hal_gpio`, add:

```cpp
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
```

**`src/CMakeLists.txt`**: replace with

```cmake
idf_component_register(
    SRCS "main.cpp" "led_probe.cpp" "m2_i2c_test.cpp"
    INCLUDE_DIRS "."
    REQUIRES board_config hal_gpio hal_i2c
    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_timer esp_system esp_hw_support heap)
```

**`sdkconfig.defaults`**: append

```
# IRAM-safe peripheral control (the flight loop must survive cache stalls)
CONFIG_I2C_ISR_IRAM_SAFE=y
CONFIG_GPIO_CTRL_FUNC_IN_IRAM=y
```

**`src/main.cpp`**: add `#include "m2_i2c_test.h"` with the other includes. Immediately before the heartbeat `for (;;)` loop, add:

```cpp
#if FE_M2_TEST
    m2_start();
#endif
```

**`CLAUDE.md`**: change the Status section to "Current module: M2, see `docs/M2_package.md`."

## 4. Claude Code prompt (plan mode)

Unzip the overlay into the repo first: `unzip franken-e58-m2-overlay.zip -d ~/franken-e58`.

```
Module 2 (dual-bus I2C scan, identification, recovery, soak). Read CLAUDE.md, then docs/M2_package.md sections 1 to 3. The new files (components/hal_i2c, src/m2_i2c_test.*) already exist in this repo. Do NOT recreate them.

Task:
1. Apply every modification listed in section 3.2 of docs/M2_package.md (board_config.h, hal_gpio.h and .cpp, src/CMakeLists.txt, sdkconfig.defaults, src/main.cpp, CLAUDE.md status line). Change nothing else.
2. Run `pio run`. Fix compile errors with the smallest possible change that keeps behaviour identical. Only edit files under src/ and components/, plus sdkconfig.defaults if an error requires it. List every change.
3. Delete sdkconfig.esp32s3, rebuild, run tools/verify_sdkconfig.sh. Report every MISMATCH with the option name ESP-IDF 5.5.3 actually uses.
4. Check these against the ESP-IDF 5.5.3 sources under ~/.platformio/packages/framework-espidf and report any mismatch: the i2c_master.h function names and signatures I used (i2c_new_master_bus, i2c_master_bus_add_device, i2c_master_probe, i2c_master_transmit_receive, i2c_master_transmit, i2c_master_bus_rm_device, i2c_del_master_bus), the field names in i2c_master_bus_config_t and i2c_device_config_t, and the header for esp_rom_delay_us.
5. Never run upload or the serial monitor.

Show me the plan first and wait for approval, then act. Final report: (a) numbered list of every change with file and reason, (b) MISMATCH list, (c) API mismatches found in step 4, (d) anything in the M2 spec that looks wrong for ESP-IDF 5.5.3.
```

## 5. Bench procedure

1. Complete the prerequisites at the top. Unplug USB.
2. Wire per §2 and run the multimeter checks.
3. Unzip the overlay, run the Claude Code prompt, review its report, and make sure the build is clean.
4. Plug in USB, upload, and open the monitor (`pio device monitor`).
5. Expect, in order: both bus init lines, two scans (A finds 0x68, B finds 0x29), the WHO_AM_I and ToF ID lines, `recovery self-test: PASS`, then `starting 600 s soak`, then a stats line every 10 s.
6. **Do not touch the wiring for the first two minutes.** A flaky breadboard contact is a finding in its own right, so write down any error that appears and what you were doing.
7. **Wire-pull test:** at about minute 3, pull the Bus A SDA jumper for about one second and reinsert it. Expect `err` and `rec` to rise and the reads to resume without a reboot. Repeat for Bus B at about minute 5.
8. Let the soak finish and copy the `SOAK RESULT` line and the last stats line.

## 6. Acceptance criteria

| # | Criterion | Pass condition |
|---|---|---|
| 1 | Build | Clean, with no `verify_sdkconfig.sh` MISMATCH, including the two new options |
| 2 | Scan | Bus A finds 0x68. Bus B finds 0x29. No unexpected addresses |
| 3 | IDs | WHO_AM_I = 0x68 or 0x70. ToF model ID = 0xEACC (verify against the datasheet) |
| 4 | Recovery self-test | `PASS` on both buses |
| 5 | Soak | 600 s with `err=0` on both buses and `mism=0`. Expect about 600 000 reads on A and about 30 000 on B |
| 6 | Wire-pull | Errors appear, recovery triggers, reads resume with no reboot. Recorded for both buses |
| 7 | Timing recorded | Bus A burst `dur_us` min/avg/max and `period_us` min/max noted for the blueprint budget (§4.3) |

If the average burst time is well above 400 µs, or the maximum is above about 1000 µs, flag it. It would mean the blueprint timing budget needs a change before M3.

## 7. Report back to me

- The Claude Code report items (a) to (d).
- The scan results and ID lines.
- The recovery self-test line.
- The wire-pull results for both buses.
- The `SOAK RESULT` line and the last stats line.
- Anything odd with the wiring, or anything that errored while you weren't touching it.

## 8. ESP-IDF APIs used and where to read about them

| API | Used for | Guide section |
|---|---|---|
| `i2c_new_master_bus`, `i2c_master_bus_add_device` | Creating a bus and a device | Peripherals → I2C → I2C master |
| `i2c_master_probe` | Address scan | Same section |
| `i2c_master_transmit_receive`, `i2c_master_transmit` | Register read and write | Same section |
| `i2c_del_master_bus`, `i2c_master_bus_rm_device` | Teardown for recovery | Same section |
| `gpio_config` with `GPIO_MODE_INPUT_OUTPUT_OD` | Open-drain bit-banged recovery and XSHUT | Peripherals → GPIO |
| `esp_rom_delay_us` | Short busy-wait delays | System → Miscellaneous System APIs |
| `xTaskCreatePinnedToCore`, `vTaskDelayUntil` | Core pinning and fixed-period loop | System → FreeRTOS (IDF) |
| `esp_timer_get_time` | Microsecond timing | System → High Resolution Timer |

ESP32-S3 Technical Reference Manual: read the I2C Controller chapter for the SCL clock and timing registers, and the bus-hang section for why 9 clock pulses release a stuck slave.

## 9. If something goes wrong

- **Every address ACKs in the scan:** SDA is stuck low, or SDA and SCL are shorted. Check the wiring.
- **Nothing found on a bus:** check 3V3 at the sensor, then the pull-ups, then that SDA and SCL aren't swapped.
- **ToF not found:** measure XSHUT to VIN. If it is open, add a 10 kΩ pull-up from XSHUT to 3V3.
- **Random single errors:** suspect breadboard contacts first, then wire length, then the 4.7 kΩ pull-ups.
