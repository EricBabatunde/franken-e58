#pragma once
#include <atomic>
#include <cstdint>

// Arming and failsafe state machine, blueprint section 4.8 (M7a). Pure C++: no IDF calls, so
// the same source runs on the laptop in tools/host_test/test_safety.cpp. On the device the code
// is placed in IRAM by linker.lf. No heap, no logging; events are returned to the caller.
//
//   BOOT -> SAFE -> DISARMED -> ARMED -> (cut) -> DISARMED (latched)
//
// BOOT: outputs forced low, nothing initialised yet. SAFE: outputs forced low and the IMU
// running; waiting for the gyro calibration and a healthy IMU. DISARMED: ready to arm.
// ARMED: outputs follow the throttle. A cut (link silence, KILL, missing IMU data-ready)
// returns to DISARMED with the latch set; clearing the arm flag returns to DISARMED without it.
// Either way re-arming needs the throttle held low and a fresh arm edge. No automatic re-arm.
namespace safety {

enum class State : uint8_t { kBoot, kSafe, kDisarmed, kArmed };

enum class Cut : uint8_t { kNone, kLinkSilence, kKill, kImuMissing };

// Reasons an arm attempt is refused, in the order they are checked. Bit (1 << reason) in a mask.
enum class Refusal : uint8_t {
    kNone = 0,
    kNotReady,        // still in BOOT or SAFE
    kNoLink,          // no setpoint ever received, or the newest is stale
    kKill,            // KILL flag set in the frame
    kNoArmEdge,       // arm flag already high when the link came up (boot, reconnect, cut)
    kImuUnhealthy,    // data-ready missing or I2C errors
    kGyroCal,         // gyro calibration not complete
    kAccelBad,        // accelerometer norm outside the trusted band (R6: relative to g0)
    kTilted,          // tilt beyond the limit
    kMoving,          // motion seen within the rest hold time
    kThrottleNotMin,  // throttle not at minimum for the hold time
    kVbatLow,         // hook only: VBAT sensing is not implemented (O4), never refuses
    kCount
};

enum class EventKind : uint8_t {
    kState,    // from -> to (BOOT->SAFE, SAFE->DISARMED, DISARMED->ARMED)
    kRefusal,  // an arm attempt refused: refusal is the first failing reason, mask all of them
    kCut,      // ARMED -> DISARMED, latched, with cut reason
    kDisarm,   // ARMED -> DISARMED because the arm flag was cleared (not a cut, not latched)
};

struct Event {
    uint32_t seq = 0;          // 1, 2, 3, ... over the life of the machine
    EventKind kind = EventKind::kState;
    State from = State::kBoot;
    State to = State::kBoot;
    Cut cut = Cut::kNone;
    Refusal refusal = Refusal::kNone;
    uint16_t mask = 0;         // refusals: every failing reason
    int64_t t_us = 0;          // decision time (Inputs::now_us)
    int64_t sp_rx_us = 0;      // receipt time of the newest setpoint (0: none)
    int64_t imu_last_us = 0;   // last data-ready timestamp (missing-IMU cut)
    uint16_t throttle = 0;
    uint8_t flags = 0;
};

// Per-iteration inputs. Times are microseconds on one monotonic clock (esp_timer on the device).
struct Inputs {
    int64_t now_us = 0;
    bool have_setpoint = false;  // at least one control frame since boot
    int64_t sp_rx_us = 0;        // receipt time of the freshest setpoint
    uint8_t sp_transport = 0;    // a change of transport counts as a reconnect
    uint16_t throttle = 0;       // permille, 0 to 1000 (clamped by the machine)
    uint8_t flags = 0;           // blueprint 4.6: bit0 arm, bit1 kill, bit2 angle mode
    bool imu_missing = false;    // no data-ready for longer than the cut threshold
    int64_t imu_last_us = 0;     // last data-ready timestamp, for the event record
    bool imu_healthy = false;
    bool gyro_cal_done = false;
    bool at_rest = false;
    bool accel_ok = false;
    bool tilt_ok = false;
};

struct Outputs {
    State state = State::kBoot;
    bool outputs_allowed = false;   // true only in ARMED
    bool force_low = true;          // generators forced low (always unless ARMED with duty > 0)
    uint16_t duty_permille = 0;     // per output, after the bench cap
    bool latched = false;           // DISARMED after a cut, until the next successful arm
    uint16_t blocking = 0;          // reasons arming would be refused now (status display)
    int n_events = 0;
    Event events[2];                // at most two per step
};

struct Config {
    int64_t link_silence_us;        // fresh below, cut above
    uint16_t throttle_min_permille;
    int64_t throttle_min_hold_us;
    uint32_t duty_cap_permille;
};

constexpr uint8_t kFlagArm = 0x01;
constexpr uint8_t kFlagKill = 0x02;

constexpr uint16_t bit(Refusal r) { return static_cast<uint16_t>(1u << static_cast<unsigned>(r)); }

// VBAT above minimum: NOT IMPLEMENTED (blueprint 6.1 O4). This hook always passes. It must be
// replaced by a real check before motors run under load (Phase 3 step 3).
inline bool vbat_ok() { return true; }

class Machine {
public:
    explicit Machine(const Config& cfg) : cfg_(cfg) {}

    // BOOT -> SAFE once the outputs are initialised and forced low and the IMU is running.
    void begin(int64_t now_us, Outputs* out);

    // One iteration. Fills `out` (state, outputs, events). Call at the loop rate.
    void step(const Inputs& in, Outputs* out);

    State state() const { return state_; }
    bool latched() const { return latched_; }
    Cut last_cut() const { return last_cut_; }

private:
    uint16_t blocking(const Inputs& in, bool fresh) const;
    void emit(Outputs* out, const Event& e);
    void set_outputs(const Inputs& in, Outputs* out) const;

    Config cfg_;
    State state_ = State::kBoot;
    bool latched_ = false;
    Cut last_cut_ = Cut::kNone;
    uint32_t seq_ = 0;
    // Link tracking.
    bool was_fresh_ = false;
    uint8_t last_transport_ = 0;
    int64_t last_rx_us_ = 0;          // receipt time of the last frame processed
    bool arm_prev_ = true;            // arm flag of the previous frame; true = held (no edge yet)
    bool first_after_reset_ = true;   // next fresh frame is the first after boot or a reconnect
    int64_t throttle_low_since_ = -1; // -1: not at minimum
};

// Gyro calibration at rest, motion and tilt from raw scaled samples (deg/s, g). Fed every sample.
struct RestConfig {
    uint32_t cal_samples;
    float cal_sd_max_dps;
    float cal_range_max_dps;
    float cal_bias_max_dps;
    float cal_norm_sd_max_g;
    float ref_min_g;
    float ref_max_g;
    uint32_t block_samples;
    float motion_gyro_dps;
    float motion_accel_frac;
    int64_t rest_hold_us;
    float valid_min_frac;
    float valid_max_frac;
    float cos_tilt_limit;     // cos(tilt limit), computed by the caller at init
};

class RestDetector {
public:
    explicit RestDetector(const RestConfig& cfg) : cfg_(cfg) {}

    void sample(int64_t now_us, const float gyro_dps[3], const float accel_g[3]);

    bool cal_done() const { return cal_done_; }
    uint32_t cal_restarts() const { return cal_restarts_; }
    uint32_t cal_progress() const { return n_; }   // samples in the current window
    bool ref_ok() const { return ref_ok_; }        // g0 inside the plausible band
    bool at_rest(int64_t now_us) const;
    bool accel_ok() const { return accel_ok_; }
    bool tilt_ok() const { return tilt_ok_; }
    const float* bias() const { return bias_; }
    float g0() const { return g0_; }
    float cal_sd_max() const { return cal_sd_max_; }  // largest axis sd of the accepted window

private:
    void restart();

    RestConfig cfg_;
    // Calibration window (Welford per axis, plus range and accel norm squared).
    uint32_t n_ = 0;
    float mean_[3] = {0, 0, 0};
    float m2_[3] = {0, 0, 0};
    float min_[3] = {0, 0, 0};
    float max_[3] = {0, 0, 0};
    float a2_mean_ = 0;
    float a2_m2_ = 0;
    bool cal_done_ = false;
    uint32_t cal_restarts_ = 0;
    float bias_[3] = {0, 0, 0};
    float g0_ = 0;
    float g0_sq_ = 0;
    bool ref_ok_ = false;
    float cal_sd_max_ = 0;
    // Motion blocks after calibration.
    uint32_t block_n_ = 0;
    float block_gyro_dev_ = 0;
    bool block_accel_out_ = false;
    int64_t rest_since_us_ = 0;
    bool accel_ok_ = false;
    bool tilt_ok_ = false;
};

// Flash-write guard: no flash writes while ARMED (blueprint 4.2 rule 6). Every future write path
// asks flash_writes_allowed() first. The flight task publishes its state every iteration.
constexpr bool flash_writes_allowed(State s) { return s != State::kArmed; }
void publish_state(State s);
bool flash_writes_allowed();

const char* name(State s);
const char* name(Cut c);
const char* name(Refusal r);

}  // namespace safety
