#pragma once
#include <cstdint>

#include "drv_mpu6500.h"
#include "esp_err.h"
#include "hal_i2c.h"
#include "safety.h"

// Flight task (blueprint 4.2), Core 1, configMAX_PRIORITIES - 2 (the IPC task holds the top
// priority on each core). Woken by the MPU-6500 data-ready interrupt exactly as in M3:
// notification, 14-byte burst read, scaling, freshest setpoint, safety step, output write.
// No estimator, filters or PID yet (M7a). When ARMED all four outputs follow the throttle,
// scaled by board::kBenchDutyCapPermille. After init: no logging, no heap, no lock shared with
// Core 0. Everything Core 0 prints arrives through seqlock slots. Code placed in IRAM (linker.lf).
namespace flight {

constexpr int kEventLog = 16;

struct EventRecord {
    safety::Event ev;
    int64_t t_force_us = 0;      // esp_timer just after force_low_all() (cuts and disarms), else 0
    uint32_t force_cycles = 0;   // CPU cycles from the decision to the force-low being written
};

// Ring of the latest events: record of event n is rec[(n - 1) % kEventLog]; count = newest seq.
struct EventLog {
    uint32_t count = 0;
    EventRecord rec[kEventLog];
};

// One 10 s window, the M3 Phase B figures plus the flight-loop extras.
struct Window {
    uint32_t index = 0;
    uint32_t length_us = 0;
    uint32_t raw_pulses = 0;
    uint32_t wakes = 0;
    uint32_t missed = 0;           // sum of (notification count - 1)
    uint32_t timeouts = 0;         // waits that ended without a notification
    uint32_t reads = 0;
    uint32_t i2c_errors = 0;
    uint32_t iv_n = 0;
    uint32_t iv_min = UINT32_MAX;
    uint32_t iv_max = 0;
    uint64_t iv_sum = 0;
    uint64_t iv_sumsq = 0;
    uint32_t burst_min = UINT32_MAX;
    uint32_t burst_max = 0;
    uint64_t burst_sum = 0;
    uint32_t lat_min = UINT32_MAX;
    uint32_t lat_max = 0;
    uint64_t lat_sum = 0;
    uint32_t samples = 0;
    float accel_sq_sum = 0;        // |a|^2, g^2 (Core 0 prints the root of the mean; float: no soft double on Core 1)
    int64_t gyro_sum[3] = {0, 0, 0};
    uint64_t gyro_sumsq[3] = {0, 0, 0};
    uint32_t raw_iv_min = 0;
    uint32_t raw_iv_max = 0;
    uint32_t isr_cycles_max = 0;
    // Flight-loop extras.
    uint32_t iterations = 0;
    uint32_t work_cyc_min = UINT32_MAX;  // wake-up to output written, CPU cycles
    uint32_t work_cyc_max = 0;
    uint64_t work_cyc_sum = 0;
    uint32_t overruns = 0;              // iterations longer than board::kLoopOverrunUs
    uint32_t sp_reads = 0;
    uint32_t sp_giveups = 0;            // seqlock read gave up: last good setpoint used
    uint32_t sp_retries_max = 0;
    uint32_t sp_torn = 0;               // check word mismatch (must stay 0)
    uint32_t armed_iterations = 0;
};

struct Status {
    uint32_t publish_seq = 0;
    int64_t t_us = 0;
    safety::State state = safety::State::kBoot;
    bool latched = false;
    safety::Cut last_cut = safety::Cut::kNone;
    uint16_t blocking = 0;
    bool have_sp = false;
    int64_t sp_age_us = 0;
    uint8_t sp_transport = 0;
    uint16_t sp_seq = 0;
    uint16_t throttle = 0;
    uint8_t flags = 0;
    uint16_t duty_permille = 0;
    bool forced_low = true;          // register read-back
    bool imu_healthy = false;
    bool imu_missing = false;
    bool int_enabled = true;
    uint32_t since_pulse_us = 0;
    bool cal_done = false;
    uint32_t cal_progress = 0;
    uint32_t cal_restarts = 0;
    float bias[3] = {0, 0, 0};
    float g0 = 0;
    float cal_sd = 0;
    bool ref_ok = false;
    bool at_rest = false;
    bool accel_ok = false;
    bool tilt_ok = false;
    float accel[3] = {0, 0, 0};       // last sample, g (Core 0 computes the tilt angle)
    uint32_t arms = 0, cuts = 0, refusals = 0, disarms = 0;
    uint32_t overruns = 0;
    uint32_t sp_giveups = 0;
    uint32_t sp_torn = 0;
    uint32_t sp_retries_max = 0;
    uint32_t i2c_errors = 0;
    bool flash_ok = true;
};

struct StartReport {
    bool done = false;
    esp_err_t bus_err = ESP_OK;
    hal_i2c::LineCheck bus_lines;
    drv_mpu6500::InitStatus init;
    esp_err_t int_err = ESP_OK;
    bool ok = false;
    uint32_t stack_free = 0;
};

// Starts the task on Core 1 and waits (up to 3 s) for its init. `outputs_ok`: hal_mcpwm::init()
// succeeded; if false the machine never leaves BOOT. Call once, after hal_mcpwm::init().
StartReport start(bool outputs_ok);

bool status(Status* out);
bool window(Window* out, uint32_t* seq);  // seq increments with every completed window
bool events(EventLog* out);

// Bench fault injection: Core 1 disables (false) or restores (true) the data-ready interrupt.
void request_imu_int(bool enabled);

}  // namespace flight
