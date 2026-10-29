// VL53L1X driver for Franken-E58, ported from the Pololu VL53L1X library for Arduino
// (https://github.com/pololu/vl53l1x-arduino, commit 8f21d608af00b42dc8776cfdf5ce935b45d2387d,
// files VL53L1X.h and VL53L1X.cpp). Arduino dependencies replaced by hal_i2c, hal_gpio,
// esp_timer and FreeRTOS. See docs/THIRD_PARTY_LICENSES.md.
//
// Copyright (c) 2017, STMicroelectronics
// Copyright (c) 2018-2022, Pololu Corporation
// All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
// may be used to endorse or promote products derived from this software
// without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once
#include <cstdint>

#include "esp_err.h"
#include "hal_i2c.h"

// Not thread-safe: the task that owns bus B (tof_task) owns this driver. Never logs.
// No heap allocation after init() has added the I2C device once.
namespace drv_vl53l1x {

// Pololu VL53L1X::DistanceMode.
enum class DistanceMode : uint8_t { kShort, kMedium, kLong };

// Pololu VL53L1X::RangeStatus (values from the ST API, see UM2356 p.17 Table 4).
enum class RangeStatus : uint8_t {
    kRangeValid = 0,
    kSigmaFail = 1,
    kSignalFail = 2,
    kRangeValidMinRangeClipped = 3,
    kOutOfBoundsFail = 4,
    kHardwareFail = 5,
    kRangeValidNoWrapCheckFail = 6,
    kWrapTargetFail = 7,
    kXtalkSignalFail = 9,
    kSynchronizationInt = 10,
    kMinRangeFail = 13,
    kNone = 255,  // "no update": raw status not in the table
};
const char* range_status_name(RangeStatus s);  // Pololu VL53L1X::rangeStatusToString()

enum class InitStep : uint8_t {
    kNone = 0,
    kAddDevice,     // hal_i2c::add_device failed
    kBootTimeout,   // FIRMWARE__SYSTEM_STATUS never reported booted
    kModelId,       // model ID read failed or was not 0xEACC
    kI2c,           // an I2C transfer failed during configuration
    kBadSetting,    // timing budget out of range
};

struct InitStatus {
    esp_err_t err = ESP_OK;
    InitStep step = InitStep::kNone;
    uint16_t model_id = 0;
    uint32_t release_us = 0;   // 32-bit esp_timer time XSHUT was released
    uint32_t boot_us = 0;      // release to first booted status read
};

struct Reading {
    uint32_t t_us;             // 32-bit esp_timer time the result read started
    uint32_t read_us;          // duration of the 17-byte result read
    uint16_t range_mm;         // after the API gain correction; meaningful only if valid
    uint8_t raw_status;        // RESULT__RANGE_STATUS
    RangeStatus status;        // mapped status
    uint8_t stream_count;
    bool valid;                // status == kRangeValid and range_mm >= board::kTofMinValidMm
    float signal_mcps;         // peak signal rate, crosstalk corrected
    float ambient_mcps;
};

// XSHUT reset and release, boot wait, model ID check, then the Pololu init() sequence
// (software reset, DataInit, StaticInit), short distance mode and the configured timing
// budget. Does not start ranging. Safe to call again to recover the sensor.
InitStatus init(hal_i2c::Bus bus);

esp_err_t start_continuous(uint32_t period_ms);  // Pololu VL53L1X::startContinuous()
esp_err_t stop_continuous();                     // Pololu VL53L1X::stopContinuous()
esp_err_t data_ready(bool* ready);               // Pololu VL53L1X::dataReady()

// Reads one result, updates the DSS, maps the status and clears the interrupt
// (Pololu VL53L1X::read() without the blocking wait).
esp_err_t read(Reading* out);

esp_err_t set_distance_mode(DistanceMode mode);  // Pololu VL53L1X::setDistanceMode()
esp_err_t set_timing_budget_us(uint32_t budget_us);  // Pololu setMeasurementTimingBudget()
uint32_t timing_budget_us();                      // Pololu getMeasurementTimingBudget()

}  // namespace drv_vl53l1x
