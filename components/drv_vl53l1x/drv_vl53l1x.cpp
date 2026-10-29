// VL53L1X driver for Franken-E58, ported from the Pololu VL53L1X library for Arduino
// (https://github.com/pololu/vl53l1x-arduino, commit 8f21d608af00b42dc8776cfdf5ce935b45d2387d,
// files VL53L1X.h and VL53L1X.cpp). Arduino dependencies replaced by hal_i2c, hal_gpio,
// esp_timer and FreeRTOS. See docs/THIRD_PARTY_LICENSES.md.
//
// Pololu's notes: "Most of the functionality of this library is based on the VL53L1X API
// provided by ST (STSW-IMG007), and some of the explanatory comments are quoted or
// paraphrased from the API source code, API user manual (UM2356), and VL53L1X datasheet."
// Register addresses and values below are not documented in DS12385 or UM2356 (the
// datasheet says register details are not exposed, DS p.16); they come from this library.
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

#include "drv_vl53l1x.h"

#include "board_config.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_gpio.h"

namespace drv_vl53l1x {
namespace {

// Register addresses (Pololu VL53L1X.h, enum regAddr, "from API vl53l1x_register_map.h").
constexpr uint16_t SOFT_RESET = 0x0000;
constexpr uint16_t OSC_MEASURED__FAST_OSC__FREQUENCY = 0x0006;
constexpr uint16_t VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND = 0x0008;
constexpr uint16_t VHV_CONFIG__INIT = 0x000B;
constexpr uint16_t ALGO__PART_TO_PART_RANGE_OFFSET_MM = 0x001E;
constexpr uint16_t MM_CONFIG__OUTER_OFFSET_MM = 0x0022;
constexpr uint16_t DSS_CONFIG__TARGET_TOTAL_RATE_MCPS = 0x0024;
constexpr uint16_t PAD_I2C_HV__EXTSUP_CONFIG = 0x002E;
constexpr uint16_t GPIO__TIO_HV_STATUS = 0x0031;
constexpr uint16_t SIGMA_ESTIMATOR__EFFECTIVE_PULSE_WIDTH_NS = 0x0036;
constexpr uint16_t SIGMA_ESTIMATOR__EFFECTIVE_AMBIENT_WIDTH_NS = 0x0037;
constexpr uint16_t ALGO__CROSSTALK_COMPENSATION_VALID_HEIGHT_MM = 0x0039;
constexpr uint16_t ALGO__RANGE_IGNORE_VALID_HEIGHT_MM = 0x003E;
constexpr uint16_t ALGO__RANGE_MIN_CLIP = 0x003F;
constexpr uint16_t ALGO__CONSISTENCY_CHECK__TOLERANCE = 0x0040;
constexpr uint16_t CAL_CONFIG__VCSEL_START = 0x0047;
constexpr uint16_t PHASECAL_CONFIG__TIMEOUT_MACROP = 0x004B;
constexpr uint16_t PHASECAL_CONFIG__OVERRIDE = 0x004D;
constexpr uint16_t DSS_CONFIG__ROI_MODE_CONTROL = 0x004F;
constexpr uint16_t SYSTEM__THRESH_RATE_HIGH = 0x0050;
constexpr uint16_t SYSTEM__THRESH_RATE_LOW = 0x0052;
constexpr uint16_t DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT = 0x0054;
constexpr uint16_t DSS_CONFIG__APERTURE_ATTENUATION = 0x0057;
constexpr uint16_t MM_CONFIG__TIMEOUT_MACROP_A = 0x005A;
constexpr uint16_t MM_CONFIG__TIMEOUT_MACROP_B = 0x005C;
constexpr uint16_t RANGE_CONFIG__TIMEOUT_MACROP_A = 0x005E;
constexpr uint16_t RANGE_CONFIG__VCSEL_PERIOD_A = 0x0060;
constexpr uint16_t RANGE_CONFIG__TIMEOUT_MACROP_B = 0x0061;
constexpr uint16_t RANGE_CONFIG__VCSEL_PERIOD_B = 0x0063;
constexpr uint16_t RANGE_CONFIG__SIGMA_THRESH = 0x0064;
constexpr uint16_t RANGE_CONFIG__MIN_COUNT_RATE_RTN_LIMIT_MCPS = 0x0066;
constexpr uint16_t RANGE_CONFIG__VALID_PHASE_HIGH = 0x0069;
constexpr uint16_t SYSTEM__INTERMEASUREMENT_PERIOD = 0x006C;
constexpr uint16_t SYSTEM__GROUPED_PARAMETER_HOLD_0 = 0x0071;
constexpr uint16_t SYSTEM__SEED_CONFIG = 0x0077;
constexpr uint16_t SD_CONFIG__WOI_SD0 = 0x0078;
constexpr uint16_t SD_CONFIG__WOI_SD1 = 0x0079;
constexpr uint16_t SD_CONFIG__INITIAL_PHASE_SD0 = 0x007A;
constexpr uint16_t SD_CONFIG__INITIAL_PHASE_SD1 = 0x007B;
constexpr uint16_t SYSTEM__GROUPED_PARAMETER_HOLD_1 = 0x007C;
constexpr uint16_t SD_CONFIG__QUANTIFIER = 0x007E;
constexpr uint16_t SYSTEM__SEQUENCE_CONFIG = 0x0081;
constexpr uint16_t SYSTEM__GROUPED_PARAMETER_HOLD = 0x0082;
constexpr uint16_t SYSTEM__INTERRUPT_CLEAR = 0x0086;
constexpr uint16_t SYSTEM__MODE_START = 0x0087;
constexpr uint16_t RESULT__RANGE_STATUS = 0x0089;
constexpr uint16_t PHASECAL_RESULT__VCSEL_START = 0x00D8;
constexpr uint16_t RESULT__OSC_CALIBRATE_VAL = 0x00DE;
constexpr uint16_t FIRMWARE__SYSTEM_STATUS = 0x00E5;
constexpr uint16_t IDENTIFICATION__MODEL_ID = 0x010F;  // 0xEA, then module type 0xCC at 0x0110 (DS p.16)

constexpr uint16_t kModelId = 0xEACC;          // DS p.16 Table 8
constexpr uint32_t kTimingGuard = 4528;        // Pololu VL53L1X.h: TimingGuard (LOWPOWER_AUTONOMOUS)
constexpr uint16_t kTargetRate = 0x0A00;       // Pololu VL53L1X.h: TargetRate
constexpr size_t kResultLen = 17;              // RESULT__RANGE_STATUS (0x0089) to 0x0099
constexpr int kTimeoutMs = 10;
// verify against datasheet: no minimum XSHUT low time found in DS12385; 10 ms as in M2.
constexpr uint32_t kXshutLowMs = 10;

// Driver state, owned by the tof_task. Pololu VL53L1X class members.
hal_i2c::Device s_dev{};
bool s_have_dev = false;
esp_err_t s_io_err = ESP_OK;       // first I2C error since it was last cleared (Pololu last_status)
uint16_t s_fast_osc_frequency = 0;
uint16_t s_osc_calibrate_val = 0;
bool s_calibrated = false;
uint8_t s_saved_vhv_init = 0;
uint8_t s_saved_vhv_timeout = 0;
uint8_t s_result[kResultLen];

// Pololu VL53L1X::ResultBuffer.
struct ResultBuffer {
    uint8_t range_status;
    uint8_t stream_count;
    uint16_t dss_actual_effective_spads_sd0;
    uint16_t ambient_count_rate_mcps_sd0;
    uint16_t final_crosstalk_corrected_range_mm_sd0;
    uint16_t peak_signal_count_rate_crosstalk_corrected_mcps_sd0;
};
ResultBuffer s_results{};

uint32_t now_us() { return static_cast<uint32_t>(esp_timer_get_time()); }

void note(esp_err_t e) {
    if (e != ESP_OK && s_io_err == ESP_OK) {
        s_io_err = e;
    }
}

// Register access. Pololu VL53L1X::writeReg/writeReg16Bit/writeReg32Bit/readReg/
// readReg16Bit: 16-bit register address, values MSB first (DS p.16 Table 9).
void wr8(uint16_t reg, uint8_t value) {
    note(hal_i2c::write_reg16(s_dev, reg, value, kTimeoutMs));
}

void wr16(uint16_t reg, uint16_t value) {
    const uint8_t b[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    note(hal_i2c::write_reg16_block(s_dev, reg, b, sizeof(b), kTimeoutMs));
}

void wr32(uint16_t reg, uint32_t value) {
    const uint8_t b[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                          static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    note(hal_i2c::write_reg16_block(s_dev, reg, b, sizeof(b), kTimeoutMs));
}

uint8_t rd8(uint16_t reg) {
    uint8_t v = 0;
    note(hal_i2c::read_reg16(s_dev, reg, &v, 1, kTimeoutMs));
    return v;
}

uint16_t rd16(uint16_t reg) {
    uint8_t b[2] = {0, 0};
    note(hal_i2c::read_reg16(s_dev, reg, b, sizeof(b), kTimeoutMs));
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

// Pololu VL53L1X::init(), "VL53L1_poll_for_boot_completion()": FIRMWARE__SYSTEM_STATUS
// bit 0 set and the read itself succeeded (the sensor NACKs while booting).
bool wait_booted(uint32_t* boot_us) {
    const uint32_t start = now_us();
    for (;;) {
        uint8_t v = 0;
        const esp_err_t e = hal_i2c::read_reg16(s_dev, FIRMWARE__SYSTEM_STATUS, &v, 1, kTimeoutMs);
        if (e == ESP_OK && (v & 0x01) != 0) {
            if (boot_us != nullptr) {
                *boot_us = now_us() - start;
            }
            return true;
        }
        if (now_us() - start > board::kTofBootTimeoutMs * 1000) {
            return false;
        }
        vTaskDelay(1);
    }
}

// Pololu VL53L1X::decodeTimeout(), "based on VL53L1_decode_timeout()".
uint32_t decode_timeout(uint16_t reg_val) {
    return (static_cast<uint32_t>(reg_val & 0xFF) << (reg_val >> 8)) + 1;
}

// Pololu VL53L1X::encodeTimeout(), "based on VL53L1_encode_timeout()".
uint16_t encode_timeout(uint32_t timeout_mclks) {
    // encoded format: "(LSByte * 2^MSByte) + 1"
    uint32_t ls_byte = 0;
    uint16_t ms_byte = 0;
    if (timeout_mclks > 0) {
        ls_byte = timeout_mclks - 1;
        while ((ls_byte & 0xFFFFFF00) > 0) {
            ls_byte >>= 1;
            ms_byte++;
        }
        return static_cast<uint16_t>((ms_byte << 8) | (ls_byte & 0xFF));
    }
    return 0;
}

// Pololu VL53L1X::timeoutMclksToMicroseconds(), "based on VL53L1_calc_timeout_us()".
uint32_t timeout_mclks_to_us(uint32_t timeout_mclks, uint32_t macro_period_us) {
    return static_cast<uint32_t>((static_cast<uint64_t>(timeout_mclks) * macro_period_us + 0x800) >> 12);
}

// Pololu VL53L1X::timeoutMicrosecondsToMclks(), "based on VL53L1_calc_timeout_mclks()".
uint32_t timeout_us_to_mclks(uint32_t timeout_us, uint32_t macro_period_us) {
    return ((timeout_us << 12) + (macro_period_us >> 1)) / macro_period_us;
}

// Pololu VL53L1X::calcMacroPeriod(), "based on VL53L1_calc_macro_period_us()".
// Assumes s_fast_osc_frequency has been read and is non-zero.
uint32_t calc_macro_period(uint8_t vcsel_period) {
    // from VL53L1_calc_pll_period_us(): fast osc frequency in 4.12 format, PLL period in 0.24
    const uint32_t pll_period_us = (static_cast<uint32_t>(0x01) << 30) / s_fast_osc_frequency;
    // from VL53L1_decode_vcsel_period()
    const uint8_t vcsel_period_pclks = static_cast<uint8_t>((vcsel_period + 1) << 1);
    // VL53L1_MACRO_PERIOD_VCSEL_PERIODS = 2304
    uint32_t macro_period_us = 2304u * pll_period_us;
    macro_period_us >>= 6;
    macro_period_us *= vcsel_period_pclks;
    macro_period_us >>= 6;
    return macro_period_us;
}

// Pololu VL53L1X::setupManualCalibration(), "based on
// VL53L1_low_power_auto_setup_manual_calibration()".
void setup_manual_calibration() {
    // "save original vhv configs"
    s_saved_vhv_init = rd8(VHV_CONFIG__INIT);
    s_saved_vhv_timeout = rd8(VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND);
    // "disable VHV init"
    wr8(VHV_CONFIG__INIT, s_saved_vhv_init & 0x7F);
    // "set loop bound to tuning param" (LOWPOWERAUTO_VHV_LOOP_BOUND_DEFAULT)
    wr8(VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND, static_cast<uint8_t>((s_saved_vhv_timeout & 0x03) + (3 << 2)));
    // "override phasecal"
    wr8(PHASECAL_CONFIG__OVERRIDE, 0x01);
    wr8(CAL_CONFIG__VCSEL_START, rd8(PHASECAL_RESULT__VCSEL_START));
}

// Pololu VL53L1X::readResults(): 17 bytes from RESULT__RANGE_STATUS.
esp_err_t read_results() {
    const esp_err_t e = hal_i2c::read_reg16(s_dev, RESULT__RANGE_STATUS, s_result, kResultLen, kTimeoutMs);
    if (e != ESP_OK) {
        return e;
    }
    const uint8_t* b = s_result;
    s_results.range_status = b[0];
    // b[1] report_status: not used
    s_results.stream_count = b[2];
    s_results.dss_actual_effective_spads_sd0 = static_cast<uint16_t>((b[3] << 8) | b[4]);
    // b[5..6] peak_signal_count_rate_mcps_sd0: not used
    s_results.ambient_count_rate_mcps_sd0 = static_cast<uint16_t>((b[7] << 8) | b[8]);
    // b[9..10] sigma_sd0, b[11..12] phase_sd0: not used
    s_results.final_crosstalk_corrected_range_mm_sd0 = static_cast<uint16_t>((b[13] << 8) | b[14]);
    s_results.peak_signal_count_rate_crosstalk_corrected_mcps_sd0 = static_cast<uint16_t>((b[15] << 8) | b[16]);
    return ESP_OK;
}

// Pololu VL53L1X::updateDSS(), "based on VL53L1_low_power_auto_update_DSS()".
void update_dss() {
    const uint16_t spad_count = s_results.dss_actual_effective_spads_sd0;
    if (spad_count != 0) {
        // "Calc total rate per spad"
        uint32_t total_rate_per_spad =
            static_cast<uint32_t>(s_results.peak_signal_count_rate_crosstalk_corrected_mcps_sd0) +
            s_results.ambient_count_rate_mcps_sd0;
        // "clip to 16 bits"
        if (total_rate_per_spad > 0xFFFF) {
            total_rate_per_spad = 0xFFFF;
        }
        // "shift up to take advantage of 32 bits"
        total_rate_per_spad <<= 16;
        total_rate_per_spad /= spad_count;
        if (total_rate_per_spad != 0) {
            // "get the target rate and shift up by 16"
            uint32_t required_spads = (static_cast<uint32_t>(kTargetRate) << 16) / total_rate_per_spad;
            // "clip to 16 bit"
            if (required_spads > 0xFFFF) {
                required_spads = 0xFFFF;
            }
            // "override DSS config"
            wr16(DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, static_cast<uint16_t>(required_spads));
            return;
        }
    }
    // "We want to gracefully set a spad target, not just exit with an error"
    // "set target to mid point"
    wr16(DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, 0x8000);
}

// Pololu VL53L1X::getRangingData(), "based on VL53L1_GetRangingMeasurementData()".
// Raw RESULT__RANGE_STATUS to RangeStatus, "mostly based on ConvertStatusLite()".
RangeStatus map_status(uint8_t raw, uint8_t stream_count) {
    switch (raw) {
        case 17:  // MULTCLIPFAIL
        case 2:   // VCSELWATCHDOGTESTFAILURE
        case 1:   // VCSELCONTINUITYTESTFAILURE
        case 3:   // NOVHVVALUEFOUND
            return RangeStatus::kHardwareFail;
        case 13:  // USERROICLIP
            return RangeStatus::kMinRangeFail;
        case 18:  // GPHSTREAMCOUNT0READY
            return RangeStatus::kSynchronizationInt;
        case 5:   // RANGEPHASECHECK
            return RangeStatus::kOutOfBoundsFail;
        case 4:   // MSRCNOTARGET
            return RangeStatus::kSignalFail;
        case 6:   // SIGMATHRESHOLDCHECK
            return RangeStatus::kSigmaFail;
        case 7:   // PHASECONSISTENCY
            return RangeStatus::kWrapTargetFail;
        case 12:  // RANGEIGNORETHRESHOLD
            return RangeStatus::kXtalkSignalFail;
        case 8:   // MINCLIP
            return RangeStatus::kRangeValidMinRangeClipped;
        case 9:   // RANGECOMPLETE: from VL53L1_copy_sys_and_core_results_to_range_results()
            return stream_count == 0 ? RangeStatus::kRangeValidNoWrapCheckFail : RangeStatus::kRangeValid;
        default:
            return RangeStatus::kNone;
    }
}

}  // namespace

const char* range_status_name(RangeStatus s) {
    switch (s) {
        case RangeStatus::kRangeValid:                 return "range valid";
        case RangeStatus::kSigmaFail:                  return "sigma fail";
        case RangeStatus::kSignalFail:                 return "signal fail";
        case RangeStatus::kRangeValidMinRangeClipped:  return "range valid, min range clipped";
        case RangeStatus::kOutOfBoundsFail:            return "out of bounds fail";
        case RangeStatus::kHardwareFail:               return "hardware fail";
        case RangeStatus::kRangeValidNoWrapCheckFail:  return "range valid, no wrap check fail";
        case RangeStatus::kWrapTargetFail:             return "wrap target fail";
        case RangeStatus::kXtalkSignalFail:            return "xtalk signal fail";
        case RangeStatus::kSynchronizationInt:         return "synchronization int";
        case RangeStatus::kMinRangeFail:               return "min range fail";
        case RangeStatus::kNone:                       return "no update";
    }
    return "unknown status";
}

InitStatus init(hal_i2c::Bus bus) {
    InitStatus st{};
    if (!s_have_dev) {
        const esp_err_t e = hal_i2c::add_device(bus, board::kTofAddr, &s_dev);
        if (e != ESP_OK) {
            st.err = e;
            st.step = InitStep::kAddDevice;
            return st;
        }
        s_have_dev = true;
    }
    // A hardware reset clears the sensor, so the driver's calibration state starts again.
    // (Pololu keeps these across init() calls; its constructor sets them once.)
    s_calibrated = false;
    s_saved_vhv_init = 0;
    s_saved_vhv_timeout = 0;

    // XSHUT reset: hardware standby, then release (DS p.11, power-up option 1).
    hal_gpio::tof_xshut_init();
    vTaskDelay(pdMS_TO_TICKS(kXshutLowMs));
    hal_gpio::tof_xshut_set(true);
    st.release_us = now_us();
    if (!wait_booted(&st.boot_us)) {
        st.err = ESP_ERR_TIMEOUT;
        st.step = InitStep::kBootTimeout;
        return st;
    }

    // Pololu VL53L1X::init(): "check model ID and module type registers".
    s_io_err = ESP_OK;
    st.model_id = rd16(IDENTIFICATION__MODEL_ID);
    if (s_io_err != ESP_OK || st.model_id != kModelId) {
        st.err = (s_io_err != ESP_OK) ? s_io_err : ESP_ERR_INVALID_RESPONSE;
        st.step = InitStep::kModelId;
        return st;
    }

    // VL53L1_software_reset()
    wr8(SOFT_RESET, 0x00);
    esp_rom_delay_us(100);
    wr8(SOFT_RESET, 0x01);
    // "give it some time to boot; otherwise the sensor NACKs" (Pololu: delay(1))
    vTaskDelay(pdMS_TO_TICKS(1) + 1);
    if (!wait_booted(nullptr)) {
        st.err = ESP_ERR_TIMEOUT;
        st.step = InitStep::kBootTimeout;
        return st;
    }

    // VL53L1_DataInit(): "sensor uses 1V8 mode for I/O by default; switch to 2V8 mode if necessary"
    s_io_err = ESP_OK;
    if (board::kTofIo2v8) {
        wr8(PAD_I2C_HV__EXTSUP_CONFIG, rd8(PAD_I2C_HV__EXTSUP_CONFIG) | 0x01);
    }
    // "store oscillator info for later use"
    s_fast_osc_frequency = rd16(OSC_MEASURED__FAST_OSC__FREQUENCY);
    s_osc_calibrate_val = rd16(RESULT__OSC_CALIBRATE_VAL);
    if (s_io_err == ESP_OK && s_fast_osc_frequency == 0) {
        s_io_err = ESP_ERR_INVALID_RESPONSE;  // calc_macro_period() divides by it
    }

    // VL53L1_StaticInit(): VL53L1_preset_mode_standard_ranging() and the
    // LOWPOWER_AUTONOMOUS preset, as written by Pololu init().
    // static config
    wr16(DSS_CONFIG__TARGET_TOTAL_RATE_MCPS, kTargetRate);
    wr8(GPIO__TIO_HV_STATUS, 0x02);
    wr8(SIGMA_ESTIMATOR__EFFECTIVE_PULSE_WIDTH_NS, 8);     // tuning parm default
    wr8(SIGMA_ESTIMATOR__EFFECTIVE_AMBIENT_WIDTH_NS, 16);  // tuning parm default
    wr8(ALGO__CROSSTALK_COMPENSATION_VALID_HEIGHT_MM, 0x01);
    wr8(ALGO__RANGE_IGNORE_VALID_HEIGHT_MM, 0xFF);
    wr8(ALGO__RANGE_MIN_CLIP, 0);                          // tuning parm default
    wr8(ALGO__CONSISTENCY_CHECK__TOLERANCE, 2);            // tuning parm default
    // general config
    wr16(SYSTEM__THRESH_RATE_HIGH, 0x0000);
    wr16(SYSTEM__THRESH_RATE_LOW, 0x0000);
    wr8(DSS_CONFIG__APERTURE_ATTENUATION, 0x38);
    // timing config
    wr16(RANGE_CONFIG__SIGMA_THRESH, 360);                 // tuning parm default
    wr16(RANGE_CONFIG__MIN_COUNT_RATE_RTN_LIMIT_MCPS, 192);  // tuning parm default
    // dynamic config
    wr8(SYSTEM__GROUPED_PARAMETER_HOLD_0, 0x01);
    wr8(SYSTEM__GROUPED_PARAMETER_HOLD_1, 0x01);
    wr8(SD_CONFIG__QUANTIFIER, 2);                         // tuning parm default
    // from VL53L1_preset_mode_timed_ranging_*: "set GPH back to 0"
    wr8(SYSTEM__GROUPED_PARAMETER_HOLD, 0x00);
    wr8(SYSTEM__SEED_CONFIG, 1);                           // tuning parm default
    // from VL53L1_config_low_power_auto_mode
    wr8(SYSTEM__SEQUENCE_CONFIG, 0x8B);                    // VHV, PHASECAL, DSS1, RANGE
    wr16(DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, 200 << 8);
    wr8(DSS_CONFIG__ROI_MODE_CONTROL, 2);                  // REQUESTED_EFFFECTIVE_SPADS
    if (s_io_err != ESP_OK) {
        st.err = s_io_err;
        st.step = InitStep::kI2c;
        return st;
    }

    // Pololu init() defaults: long range, 50 ms timing budget.
    esp_err_t e = set_distance_mode(DistanceMode::kLong);
    if (e == ESP_OK) {
        e = set_timing_budget_us(50000);
    }
    // "the API triggers this change in VL53L1_init_and_start_range() once a measurement is
    // started; assumes MM1 and MM2 are disabled"
    if (e == ESP_OK) {
        s_io_err = ESP_OK;
        wr16(ALGO__PART_TO_PART_RANGE_OFFSET_MM, static_cast<uint16_t>(rd16(MM_CONFIG__OUTER_OFFSET_MM) * 4));
        e = s_io_err;
    }

    // Franken-E58 settings: short mode and the configured budget (board_config).
    if (e == ESP_OK) {
        e = set_distance_mode(DistanceMode::kShort);
    }
    if (e == ESP_OK) {
        e = set_timing_budget_us(board::kTofTimingBudgetUs);
    }
    if (e != ESP_OK) {
        st.err = e;
        st.step = (e == ESP_ERR_INVALID_ARG) ? InitStep::kBadSetting : InitStep::kI2c;
    }
    return st;
}

esp_err_t set_distance_mode(DistanceMode mode) {
    // Pololu VL53L1X::setDistanceMode(), "based on VL53L1_SetDistanceMode()".
    // "save existing timing budget"
    s_io_err = ESP_OK;
    const uint32_t budget_us = timing_budget_us();
    switch (mode) {
        case DistanceMode::kShort:
            // from VL53L1_preset_mode_standard_ranging_short_range()
            wr8(RANGE_CONFIG__VCSEL_PERIOD_A, 0x07);
            wr8(RANGE_CONFIG__VCSEL_PERIOD_B, 0x05);
            wr8(RANGE_CONFIG__VALID_PHASE_HIGH, 0x38);
            wr8(SD_CONFIG__WOI_SD0, 0x07);
            wr8(SD_CONFIG__WOI_SD1, 0x05);
            wr8(SD_CONFIG__INITIAL_PHASE_SD0, 6);  // tuning parm default
            wr8(SD_CONFIG__INITIAL_PHASE_SD1, 6);  // tuning parm default
            break;
        case DistanceMode::kMedium:
            // from VL53L1_preset_mode_standard_ranging()
            wr8(RANGE_CONFIG__VCSEL_PERIOD_A, 0x0B);
            wr8(RANGE_CONFIG__VCSEL_PERIOD_B, 0x09);
            wr8(RANGE_CONFIG__VALID_PHASE_HIGH, 0x78);
            wr8(SD_CONFIG__WOI_SD0, 0x0B);
            wr8(SD_CONFIG__WOI_SD1, 0x09);
            wr8(SD_CONFIG__INITIAL_PHASE_SD0, 10);  // tuning parm default
            wr8(SD_CONFIG__INITIAL_PHASE_SD1, 10);  // tuning parm default
            break;
        case DistanceMode::kLong:
            // from VL53L1_preset_mode_standard_ranging_long_range()
            wr8(RANGE_CONFIG__VCSEL_PERIOD_A, 0x0F);
            wr8(RANGE_CONFIG__VCSEL_PERIOD_B, 0x0D);
            wr8(RANGE_CONFIG__VALID_PHASE_HIGH, 0xB8);
            wr8(SD_CONFIG__WOI_SD0, 0x0F);
            wr8(SD_CONFIG__WOI_SD1, 0x0D);
            wr8(SD_CONFIG__INITIAL_PHASE_SD0, 14);  // tuning parm default
            wr8(SD_CONFIG__INITIAL_PHASE_SD1, 14);  // tuning parm default
            break;
    }
    if (s_io_err != ESP_OK) {
        return s_io_err;
    }
    // "reapply timing budget"
    return set_timing_budget_us(budget_us);
}

esp_err_t set_timing_budget_us(uint32_t budget_us) {
    // Pololu VL53L1X::setMeasurementTimingBudget(), "based on
    // VL53L1_SetMeasurementTimingBudgetMicroSeconds()"; assumes LOWPOWER_AUTONOMOUS.
    if (budget_us <= kTimingGuard) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t range_config_timeout_us = budget_us - kTimingGuard;
    if (range_config_timeout_us > 1100000) {  // FDA_MAX_TIMING_BUDGET_US * 2
        return ESP_ERR_INVALID_ARG;
    }
    range_config_timeout_us /= 2;

    // VL53L1_calc_timeout_register_values()
    s_io_err = ESP_OK;
    // "Update Macro Period for Range A VCSEL Period"
    uint32_t macro_period_us = calc_macro_period(rd8(RANGE_CONFIG__VCSEL_PERIOD_A));
    // "Update Phase timeout - uses Timing A" (TIMED_PHASECAL_CONFIG_TIMEOUT_US_DEFAULT = 1000)
    uint32_t phasecal_timeout_mclks = timeout_us_to_mclks(1000, macro_period_us);
    if (phasecal_timeout_mclks > 0xFF) {
        phasecal_timeout_mclks = 0xFF;
    }
    wr8(PHASECAL_CONFIG__TIMEOUT_MACROP, static_cast<uint8_t>(phasecal_timeout_mclks));
    // "Update MM Timing A timeout" (LOWPOWERAUTO_MM_CONFIG_TIMEOUT_US_DEFAULT = 1)
    wr16(MM_CONFIG__TIMEOUT_MACROP_A, encode_timeout(timeout_us_to_mclks(1, macro_period_us)));
    // "Update Range Timing A timeout"
    wr16(RANGE_CONFIG__TIMEOUT_MACROP_A,
         encode_timeout(timeout_us_to_mclks(range_config_timeout_us, macro_period_us)));
    // "Update Macro Period for Range B VCSEL Period"
    macro_period_us = calc_macro_period(rd8(RANGE_CONFIG__VCSEL_PERIOD_B));
    // "Update MM Timing B timeout"
    wr16(MM_CONFIG__TIMEOUT_MACROP_B, encode_timeout(timeout_us_to_mclks(1, macro_period_us)));
    // "Update Range Timing B timeout"
    wr16(RANGE_CONFIG__TIMEOUT_MACROP_B,
         encode_timeout(timeout_us_to_mclks(range_config_timeout_us, macro_period_us)));
    return s_io_err;
}

uint32_t timing_budget_us() {
    // Pololu VL53L1X::getMeasurementTimingBudget(); assumes LOWPOWER_AUTONOMOUS with the
    // VHV, PHASECAL, DSS1 and RANGE sequence steps enabled.
    // "Update Macro Period for Range A VCSEL Period"
    const uint32_t macro_period_us = calc_macro_period(rd8(RANGE_CONFIG__VCSEL_PERIOD_A));
    // "Get Range Timing A timeout"
    const uint32_t range_config_timeout_us = timeout_mclks_to_us(
        decode_timeout(rd16(RANGE_CONFIG__TIMEOUT_MACROP_A)), macro_period_us);
    return 2 * range_config_timeout_us + kTimingGuard;
}

esp_err_t start_continuous(uint32_t period_ms) {
    // Pololu VL53L1X::startContinuous(), from VL53L1_set_inter_measurement_period_ms()
    s_io_err = ESP_OK;
    wr32(SYSTEM__INTERMEASUREMENT_PERIOD, period_ms * s_osc_calibrate_val);
    wr8(SYSTEM__INTERRUPT_CLEAR, 0x01);  // sys_interrupt_clear_range
    wr8(SYSTEM__MODE_START, 0x40);       // mode_range__timed
    return s_io_err;
}

esp_err_t stop_continuous() {
    // Pololu VL53L1X::stopContinuous(), "based on VL53L1_stop_range()"
    s_io_err = ESP_OK;
    wr8(SYSTEM__MODE_START, 0x80);  // mode_range__abort
    // VL53L1_low_power_auto_data_stop_range()
    s_calibrated = false;
    // "restore vhv configs"
    if (s_saved_vhv_init != 0) {
        wr8(VHV_CONFIG__INIT, s_saved_vhv_init);
    }
    if (s_saved_vhv_timeout != 0) {
        wr8(VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND, s_saved_vhv_timeout);
    }
    // "remove phasecal override"
    wr8(PHASECAL_CONFIG__OVERRIDE, 0x00);
    return s_io_err;
}

esp_err_t data_ready(bool* ready) {
    // Pololu VL53L1X::dataReady(): "assumes interrupt is active low (GPIO_HV_MUX__CTRL bit 4 is 1)"
    uint8_t v = 0;
    const esp_err_t e = hal_i2c::read_reg16(s_dev, GPIO__TIO_HV_STATUS, &v, 1, kTimeoutMs);
    *ready = (e == ESP_OK) && ((v & 0x01) == 0);
    return e;
}

esp_err_t read(Reading* out) {
    // Pololu VL53L1X::read(), non-blocking path.
    const uint32_t t0 = now_us();
    esp_err_t e = read_results();
    out->t_us = t0;
    out->read_us = now_us() - t0;
    if (e != ESP_OK) {
        return e;
    }
    s_io_err = ESP_OK;
    if (!s_calibrated) {
        setup_manual_calibration();
        s_calibrated = true;
    }
    update_dss();

    // getRangingData(): "apply correction gain" (VL53L1_TUNINGPARM_LITE_RANGING_GAIN_FACTOR_DEFAULT
    // = 2011, about 98 %, with 1024 added for rounding)
    out->range_mm = static_cast<uint16_t>(
        (static_cast<uint32_t>(s_results.final_crosstalk_corrected_range_mm_sd0) * 2011 + 0x0400) / 0x0800);
    out->raw_status = s_results.range_status;
    out->stream_count = s_results.stream_count;
    out->status = map_status(s_results.range_status, s_results.stream_count);
    // from SetSimpleData(): 9.7 fixed point (Pololu countRateFixedToFloat())
    out->signal_mcps = static_cast<float>(s_results.peak_signal_count_rate_crosstalk_corrected_mcps_sd0) / (1 << 7);
    out->ambient_mcps = static_cast<float>(s_results.ambient_count_rate_mcps_sd0) / (1 << 7);
    // Franken-E58 (M4 D-M4-02, D-M4-03): only "range valid" at or above the 40 mm
    // guaranteed minimum (DS p.21) counts as a distance.
    out->valid = out->status == RangeStatus::kRangeValid && out->range_mm >= board::kTofMinValidMm;

    wr8(SYSTEM__INTERRUPT_CLEAR, 0x01);  // sys_interrupt_clear_range
    return s_io_err;
}

}  // namespace drv_vl53l1x
