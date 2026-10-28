#pragma once
#include <cstdint>
// Single source of truth for pin numbers and project-wide limits.
// No other file may contain a literal GPIO number.

namespace board {

// Motors (MCPWM in a later module): M1 FL, M2 FR, M3 RL, M4 RR
constexpr int kMotorGpio[4] = {4, 5, 6, 7};

// Bus A (I2C0): MPU-6050
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
constexpr uint32_t kI2cHz = 400000;  // Fast-mode, the MPU-6050 maximum
constexpr uint8_t kMpuAddr = 0x68;   // AD0 low
constexpr uint8_t kTofAddr = 0x29;   // VL53L1X default

}  // namespace board

// Set to 0 once the LED GPIO is identified.
#define FE_LED_PROBE 0

// Set to 0 to skip the Module 2 bench test.
#define FE_M2_TEST 1
