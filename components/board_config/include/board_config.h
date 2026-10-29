#pragma once
#include <cstdint>
// Single source of truth for pin numbers and project-wide limits.
// No other file may contain a literal GPIO number.

namespace board {

// Motors (MCPWM in a later module): M1 FL, M2 FR, M3 RL, M4 RR
constexpr int kMotorGpio[4] = {4, 5, 6, 7};

// Bus A (I2C0): MPU-6500
constexpr int kImuSda = 1;
constexpr int kImuScl = 2;
constexpr int kImuInt = 9;

// Bus B (I2C1): VL53L1X
constexpr int kTofSda   = 11;
constexpr int kTofScl   = 12;
constexpr int kTofXshut = 8;
constexpr int kTofInt   = 13;  // optional

// Battery sense (ADC1_CH9)
constexpr int kVbatAdc = 10;

// Possible onboard LED pins, reserved until identified by the M1 probe
constexpr int kLedCandidates[4] = {48, 38, 47, 21};

// I2C (both buses)
constexpr uint32_t kI2cHz = 400000;  // Fast-mode, the MPU-6500 maximum
constexpr uint8_t kMpuAddr = 0x68;   // AD0 low
constexpr uint8_t kTofAddr = 0x29;   // VL53L1X default

// MPU-6500 configuration. Values and page references: docs/mpu6500_notes.md
// (PS = product specification, RM = register map).
constexpr int kImuDlpfCfg = 1;         // CONFIG.DLPF_CFG: 184 Hz, 2.9 ms, 1 kHz (RM p.14)
constexpr int kImuIsrDecimation = 1;   // act on every Nth data-ready pulse
constexpr int kImuGyroRangeDps = 1000; // GYRO_FS_SEL = 2, 32.8 LSB/(deg/s) (RM p.14, PS p.7)
constexpr int kImuAccelRangeG = 8;     // ACCEL_FS_SEL = 2, 4096 LSB/g (RM p.15, PS p.8)
constexpr int kImuAccelDlpfCfg = 3;    // ACCEL_CONFIG2.A_DLPF_CFG: 41 Hz, 11.8 ms (RM p.15)
constexpr int kImuSmplrtDiv = 0;       // 1 kHz; only effective for DLPF_CFG 1 to 6 (RM p.12)

// DLPF_CFG 1 to 6 give 1 kHz data-ready, so act on every pulse. DLPF_CFG 0 gives 8 kHz,
// so act on every 8th (blueprint section 4.4 item 2, Option A). DLPF_CFG 7 is not a flight setting.
static_assert((kImuDlpfCfg >= 1 && kImuDlpfCfg <= 6 && kImuIsrDecimation == 1) ||
                  (kImuDlpfCfg == 0 && kImuIsrDecimation == 8),
              "kImuIsrDecimation must be 1 for DLPF_CFG 1 to 6, and 8 only with DLPF_CFG 0");

}  // namespace board

// Set to 0 once the LED GPIO is identified.
#define FE_LED_PROBE 0

// Set to 0 to skip the Module 2 bench test.
#define FE_M2_TEST 0

// Set to 0 to skip the Module 3 bench test.
#define FE_M3_TEST 1

#if FE_M2_TEST && FE_M3_TEST
#error "FE_M2_TEST and FE_M3_TEST both drive bus A: enable only one"
#endif

// Bench-only IMU ISR instrumentation (raw-interval min/max, handler cycle count, runtime
// notify-every-N). Off in flight builds, where the ISR is timestamp, count, notify only.
#define FE_IMU_ISR_INSTRUMENT FE_M3_TEST
