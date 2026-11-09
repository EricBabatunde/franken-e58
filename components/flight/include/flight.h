#pragma once
#include <cstdint>

#include "drv_mpu6500.h"
#include "esp_err.h"
#include "hal_i2c.h"
#include "latency_hist.h"
#include "safety.h"
#include "wdt.h"

// Flight task (blueprint 4.2), Core 1, configMAX_PRIORITIES - 2 (the IPC task holds the top
// priority on each core). Woken by the MPU-6500 data-ready interrupt exactly as in M3:
// notification, 14-byte burst read, scaling, freshest setpoint, safety step, output write.
// No estimator, filters or PID yet (M7a). When ARMED all four outputs follow the throttle,
// scaled by board::kBenchDutyCapPermille. After init: no logging, no heap, no lock shared with
// Core 0. Everything Core 0 prints arrives through seqlock slots. Code placed in IRAM (linker.lf).
//
// M7b: the task starts the GPTimer heartbeat watchdog (wdt, interrupt on Core 1), stores its
// heartbeat every iteration, turns the watchdog hard latch into a safety cut, clears the latch
// only on entering ARMED, recovers the IMU while SAFE or DISARMED, retries the IMU init at boot
// (board::kImuBootRetry) and runs the console fault injections.
//
// M7c (bench builds, CONFIG_FE_LATENCY_HIST): latency histograms per condition, the synthetic
// compute load, and the R5 (d) read schedule (CONFIG_FE_R5_GYRO_ONLY). Every build: an
// accelerometer value older than board::kAccelStaleUs fails the rest, tilt and accelerometer checks.
namespace flight {

constexpr int kEventLog = 16;

struct EventRecord {
    safety::Event ev;
    int64_t t_force_us = 0;      // esp_timer just after force_low_all() (cuts and disarms), else 0
    uint32_t force_cycles = 0;   // CPU cycles from the decision to the force-low being written
    // Watchdog cuts (wd_heartbeat, wd_imu_missing): the ISR's own record (32-bit esp_timer times).
    uint32_t wd_decide_us = 0;   // ISR decision time (the force is written in the same ISR)
    uint32_t wd_ref_us = 0;      // last heartbeat or last data-ready pulse
    uint32_t wd_force_cycles = 0;
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
    // M7b watchdog ISR cost in this window.
    wdt::IsrWindow wd;
};

// M7b IMU recovery attempts (SAFE or DISARMED only).
struct RecoveryRecord {
    uint32_t seq = 0;
    int64_t t_us = 0;
    safety::State state = safety::State::kBoot;  // state when the attempt started (never ARMED)
    uint32_t attempt = 0;        // 1 to board::kRecoveryAttempts within the episode
    uint32_t missing_us = 0;     // data-ready missing for this long when the attempt started
    esp_err_t bus_reset_err = ESP_OK;
    drv_mpu6500::Step step = drv_mpu6500::Step::kNone;
    esp_err_t init_err = ESP_OK;
    uint8_t who_am_i = 0;
    uint32_t duration_us = 0;    // bus reset plus the full IMU init sequence
    bool ok = false;
};
constexpr int kRecoveryLog = 8;
struct RecoveryLog {
    uint32_t count = 0;          // record n is rec[(n - 1) % kRecoveryLog]
    RecoveryRecord rec[kRecoveryLog];
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
    // M7b.
    uint8_t hard_latched = 0;        // watchdog hard latch (0: clear)
    bool wd_alive = true;
    uint32_t wd_count = 0;           // watchdog alarms handled (wraps)
    uint32_t rec_attempts = 0;       // IMU recovery attempts since boot
    uint32_t rec_ok = 0;
    uint32_t rec_episode = 0;        // attempts in the current episode
    bool rec_exhausted = false;      // board::kRecoveryAttempts failed: Core 0 decides restart or BOOT
    // M7c.
    bool accel_stale = false;        // last full read older than board::kAccelStaleUs
    uint32_t accel_age_us = 0;
    uint32_t load_us = 0;            // synthetic load in effect (0: off)
};

// M7c latency histograms for one condition (bench builds, CONFIG_FE_LATENCY_HIST). Core 1 keeps
// the running set and publishes it every board::kTimingPublishUs, or at once when Core 0 asks
// (timing_publish). Core 0 starts a new condition with timing_reset(): Core 1 clears the set at its
// next iteration and tags it with the new epoch.
struct Timing {
    uint32_t epoch = 0;              // condition epoch these counts belong to
    uint32_t publish_ack = 0;        // the last timing_publish() request served
    int64_t t_first_us = 0;          // first iteration in this epoch
    int64_t t_last_us = 0;
    uint32_t iterations = 0;
    uint32_t wakes = 0;              // data wake-ups
    uint32_t missed = 0;             // notifications beyond one per wake-up
    uint32_t timeouts = 0;           // waits without data
    uint32_t i2c_errors = 0;
    uint32_t overruns = 0;           // iterations longer than board::kLoopOverrunUs
    uint32_t accel_stale = 0;        // iterations with a stale accelerometer
    uint32_t load_us = 0;            // synthetic load at the last iteration
    uint32_t load_changes = 0;       // load toggled within this epoch (the condition is mixed)
    int32_t period_dev_min = 0;      // signed wake-to-wake minus board::kNominalPeriodUs
    int32_t period_dev_max = 0;
    latency::Hist<200, 1> period;    // |wake-to-wake - board::kNominalPeriodUs|, consecutive data wakes
    latency::Hist<100, 1> wake;      // data-ready interrupt to task wake-up
    latency::Hist<150, 10> burst14;  // 14-byte burst read
    latency::Hist<150, 10> burst6;   // 6-byte gyro read (R5 d only)
    latency::Hist<100, 10> work;     // wake-up to the end of the iteration, every iteration
};


struct StartReport {
    bool done = false;
    esp_err_t bus_err = ESP_OK;
    hal_i2c::LineCheck bus_lines;
    drv_mpu6500::InitStatus init;     // the last attempt
    esp_err_t int_err = ESP_OK;
    bool ok = false;
    uint32_t stack_free = 0;
    // M7b boot retry: every attempt (1 unless board::kImuBootRetry).
    uint32_t attempts = 0;
    drv_mpu6500::InitStatus attempt[3];
    esp_err_t retry_bus_reset_err[3] = {ESP_OK, ESP_OK, ESP_OK};
    esp_err_t retry_soft_reset_err[3] = {ESP_OK, ESP_OK, ESP_OK};
    wdt::StartReport wdt;
};

// Starts the task on Core 1 and waits (up to 3 s) for its init. `outputs_ok`: hal_mcpwm::init()
// succeeded; if false the machine never leaves BOOT. Call once, after hal_mcpwm::init().
StartReport start(bool outputs_ok);

bool status(Status* out);
bool window(Window* out, uint32_t* seq);  // seq increments with every completed window
bool events(EventLog* out);

// Bench fault injection: Core 1 disables (false) or restores (true) the data-ready interrupt.
// While disabled, IMU recovery is suppressed (the injection is deliberate).
void request_imu_int(bool enabled);

// M7b bench fault injection (console keys only; never reachable from the network). The flight
// task acts on the request between the safety decision and the output write.
enum class Fault : uint8_t {
    kNone = 0,
    kStallShort,   // 'h': busy wait board::kFaultStallShortUs, interrupts on
    kStallMasked,  // 'H': busy wait board::kFaultStallMaskedUs, interrupts masked on Core 1
    kStallLong,    // 'T': busy wait board::kFaultStallLongUs, interrupts on (task watchdog)
    kAbort,        // 'X': abort()
    kImuSleep,     // 'z': put the IMU to sleep (data-ready stops; recovery test)
};
void request_fault(Fault f);

bool recovery_log(RecoveryLog* out);

// Core 0, after rec_exhausted and the restart limit (board::kRestartLimit): the machine goes to
// BOOT (gates low, final) unless ARMED.
void request_boot_state();

// M7c (bench builds, CONFIG_FE_LATENCY_HIST; no effect otherwise). Core 0 only.
bool timing(Timing* out);              // the latest published set
uint32_t timing_reset();               // new condition: returns its epoch
uint32_t timing_publish();             // publish at the next iteration: returns the request number
void request_load(uint32_t load_us);   // synthetic load after each IMU read (0: off)
bool timing_enabled();                 // CONFIG_FE_LATENCY_HIST

}  // namespace flight
