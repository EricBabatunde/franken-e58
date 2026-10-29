#pragma once
#include <cstdint>

#include "board_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_i2c.h"

// MPU-6500 driver. Register addresses, bit fields and values come from
// docs/mpu6500_notes.md (PS = product specification, RM = register map).
// Not thread-safe: the task that owns the IMU bus owns this driver.
// The driver never logs. init() and reconfigure() report through InitStatus so the
// caller can print from Core 0 (CLAUDE.md Core 1 rules, M3 deviation D-M3-01).
namespace drv_mpu6500 {

// Where the init sequence stopped. kNone means success.
enum class Step : uint8_t {
    kNone = 0,
    kProbe,          // no ACK at the I2C address, or the device could not be added
    kWhoAmI,         // WHO_AM_I read failed or was not 0x70
    kReset,          // DEVICE_RESET write failed
    kResetCheck,     // PWR_MGMT_1 read failed or was not 0x01 after the reset
    kWrite,          // a configuration write failed
    kReadback,       // a configuration register read back a different value
    kBadArgument,    // DLPF_CFG out of range, or reconfigure() before init()
};

struct InitStatus {
    esp_err_t err = ESP_OK;
    Step step = Step::kNone;
    uint8_t reg = 0;         // register of the failing step
    uint8_t expected = 0;
    uint8_t actual = 0;
    uint8_t who_am_i = 0;
    uint8_t pwr_mgmt_1 = 0;  // value read back after DEVICE_RESET
    uint8_t dlpf_cfg = 0;
    uint8_t n_checked = 0;   // configuration registers written and read back
    uint32_t reset_us = 0;   // 32-bit esp_timer time of the DEVICE_RESET write
};

// One 14-byte burst from ACCEL_XOUT_H (0x3B): accel, temperature, gyro (RM p.7).
struct RawSample {
    uint32_t t_us;  // interrupt timestamp of the pulse that triggered the read
    int16_t accel[3];
    int16_t temp;
    int16_t gyro[3];
};

// Probes 0x68 on `bus`, adds the device and runs the full sequence at board::kImuDlpfCfg:
// WHO_AM_I, DEVICE_RESET, wait >= 100 ms, PWR_MGMT_1 check, configuration writes with
// read-back, INT_ENABLE last. Init only: may allocate (hal_i2c::add_device).
InitStatus init(hal_i2c::Bus bus);

// Repeats the full sequence with a different DLPF_CFG (0 to 7). No allocation.
InitStatus reconfigure(uint8_t dlpf_cfg);

// One 14-byte transfer into `out`. No allocation; uses a static buffer.
esp_err_t read_burst(uint32_t t_us, RawSample* out);

// First 32-bit timestamp at which gyro data is trusted: 35 ms after the last DEVICE_RESET
// (gyro start-up from sleep, PS p.7). Compare with wrap-safe signed subtraction.
uint32_t gyro_valid_from_us();

float gyro_dps(int16_t raw);  // board::kImuGyroRangeDps scaling (PS p.7)
float accel_g(int16_t raw);   // board::kImuAccelRangeG scaling (PS p.8)

// ---- Interrupt path ----
// Timestamps are esp_timer_get_time() truncated to 32 bits. They wrap after about
// 71.6 minutes, so always subtract them as uint32_t. No 64-bit atomics anywhere.

// Installs the data-ready ISR on GPIO board::kImuInt through hal_gpio. Call from `task`,
// pinned to the core that should take the interrupt; `task` receives the notifications.
esp_err_t int_install(TaskHandle_t task);

uint32_t raw_pulses();      // every data-ready pulse since int_install (wraps)
uint32_t last_notify_us();  // ISR timestamp of the most recent notifying pulse

#if FE_IMU_ISR_INSTRUMENT
// Bench-only instrumentation. Not present in flight builds.
struct IsrInstrument {
    uint32_t raw_iv_min_us;        // shortest interval between raw pulses
    uint32_t raw_iv_max_us;        // longest interval between raw pulses
    uint32_t handler_cycles_max;   // our handler only, not the IDF GPIO dispatcher
};
void isr_set_notify_every(uint32_t n);  // 0 = count only, never notify
void isr_instrument_reset();            // applied by the ISR on its next pulse
IsrInstrument isr_instrument();
#endif

}  // namespace drv_mpu6500
