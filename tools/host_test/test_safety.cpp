// Host unit test for components/safety (M7a): the arming and failsafe state machine and the
// rest detector. Same sources as the firmware, same thresholds (board_config.h is plain C++).
// Build and run (docs/M7a_package.md, D-M7a-05):
//   g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/safety/include -Icomponents/board_config/include
//       components/safety/safety.cpp components/safety/rest.cpp tools/host_test/test_safety.cpp
//       -o .pio/host/test_safety && .pio/host/test_safety
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "board_config.h"
#include "safety.h"

using namespace safety;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

Config machine_config() {
    return Config{board::kLinkSilenceCutUs, board::kThrottleMinPermille, board::kThrottleMinHoldUs,
                  board::kBenchDutyCapPermille};
}

// A 1 kHz loop with a 50 Hz client. All inputs healthy unless a test changes them.
struct Sim {
    Machine m{machine_config()};
    Inputs in;
    Outputs out;
    std::vector<Event> events;
    int64_t now = 1000000;      // 1 s after boot
    int64_t last_send = -1000000;
    bool sending = true;
    uint16_t throttle = 0;
    uint8_t flags = 0;
    uint8_t transport = 0;

    Sim() {
        in.imu_healthy = in.gyro_cal_done = in.at_rest = in.accel_ok = in.tilt_ok = true;
        m.begin(now, &out);
        collect();
    }
    void collect() {
        for (int i = 0; i < out.n_events; ++i) events.push_back(out.events[i]);
    }
    void frame() {
        in.have_setpoint = true;
        in.sp_rx_us = now;
        in.throttle = throttle;
        in.flags = flags;
        in.sp_transport = transport;
        last_send = now;
    }
    // Advances `us` microseconds in steps of `dt` (1 ms by default), sending at 50 Hz.
    void run(int64_t us, int64_t dt = 1000) {
        for (int64_t t = 0; t < us; t += dt) {
            now += dt;
            if (sending && now - last_send >= 20000) frame();
            in.now_us = now;
            m.step(in, &out);
            collect();
            invariants();
        }
    }
    void invariants() {
        check(out.outputs_allowed == (out.state == State::kArmed), "outputs allowed only in ARMED");
        if (out.state != State::kArmed) {
            check(out.force_low && out.duty_permille == 0, "forced low with zero duty outside ARMED");
        }
        check(out.duty_permille <= board::kBenchDutyCapPermille, "duty never above the bench cap");
    }
    // Arm flag low with throttle 0 for 700 ms, then the arm edge, then 100 ms.
    void arm_sequence(uint16_t thr_after = 0) {
        throttle = 0;
        flags = 0;
        run(700000);
        flags = kFlagArm;
        run(100000);
        throttle = thr_after;
        run(100000);
    }
    const Event* last(EventKind k) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->kind == k) return &*it;
        }
        return nullptr;
    }
    int count(EventKind k) const {
        int n = 0;
        for (const Event& e : events) n += e.kind == k;
        return n;
    }
};

// Brings a Sim from SAFE to DISARMED (calibration done, IMU healthy).
Sim disarmed() {
    Sim s;
    s.run(5000);
    return s;
}

void test_boot_transitions() {
    Sim s;
    check(s.events.size() == 1 && s.events[0].from == State::kBoot && s.events[0].to == State::kSafe,
          "begin(): BOOT -> SAFE event");
    // SAFE until calibration completes and the IMU is healthy.
    s.in.gyro_cal_done = false;
    s.run(50000);
    check(s.out.state == State::kSafe, "stays SAFE while the gyro calibration runs");
    s.in.gyro_cal_done = true;
    s.in.imu_healthy = false;
    s.run(50000);
    check(s.out.state == State::kSafe, "stays SAFE while the IMU is unhealthy");
    s.in.imu_healthy = true;
    s.run(2000);
    check(s.out.state == State::kDisarmed, "SAFE -> DISARMED once calibrated and healthy");
    const Event* e = s.last(EventKind::kState);
    check(e && e->from == State::kSafe && e->to == State::kDisarmed, "SAFE -> DISARMED event recorded");
    // An arm attempt in SAFE is refused as not ready.
    Sim t;
    t.in.gyro_cal_done = false;
    t.arm_sequence();
    const Event* r = t.last(EventKind::kRefusal);
    check(t.out.state == State::kSafe && r && r->refusal == Refusal::kNotReady, "arm attempt in SAFE: refused, not_ready");
}

void test_arm_and_output() {
    Sim s = disarmed();
    s.arm_sequence(500);
    check(s.out.state == State::kArmed, "arms after throttle low 700 ms and a fresh edge");
    check(s.out.outputs_allowed && !s.out.force_low, "ARMED with throttle: outputs released");
    check(s.out.duty_permille == 500 * board::kBenchDutyCapPermille / 1000, "duty = throttle x cap / 1000");
    check(!s.out.latched, "not latched after arming");
    s.throttle = 0;
    s.run(40000);
    check(s.out.state == State::kArmed && s.out.force_low && s.out.duty_permille == 0,
          "ARMED with throttle 0: duty 0, held by the force");
    s.throttle = 1200;
    s.run(40000);
    check(s.out.duty_permille == board::kBenchDutyCapPermille, "throttle above 1000 clamps to the cap");
}

// Every arm condition failing alone, from a table.
void test_each_condition_alone() {
    struct Row {
        const char* name;
        std::function<void(Sim&)> spoil;
        Refusal want;
    };
    const std::vector<Row> rows = {
        {"IMU unhealthy", [](Sim& s) { s.in.imu_healthy = false; }, Refusal::kImuUnhealthy},
        {"IMU data-ready missing", [](Sim& s) { s.in.imu_missing = true; }, Refusal::kImuUnhealthy},
        {"gyro calibration lost", [](Sim& s) { s.in.gyro_cal_done = false; }, Refusal::kGyroCal},
        {"accelerometer bad", [](Sim& s) { s.in.accel_ok = false; s.in.tilt_ok = false; }, Refusal::kAccelBad},
        {"tilted", [](Sim& s) { s.in.tilt_ok = false; }, Refusal::kTilted},
        {"moving", [](Sim& s) { s.in.at_rest = false; }, Refusal::kMoving},
        {"kill flag", [](Sim& s) { s.flags |= kFlagKill; }, Refusal::kKill},
    };
    for (const Row& r : rows) {
        Sim s = disarmed();
        s.throttle = 0;
        s.flags = 0;
        s.run(700000);
        r.spoil(s);
        s.flags |= kFlagArm;
        s.run(100000);
        const Event* e = s.last(EventKind::kRefusal);
        check(s.out.state == State::kDisarmed, std::string(r.name) + ": stays DISARMED");
        check(e && e->refusal == r.want, std::string(r.name) + ": refused with its own reason");
        check(e && e->mask == bit(r.want), std::string(r.name) + ": the only reason in the mask");
        check(s.count(EventKind::kRefusal) == 1, std::string(r.name) + ": one refusal per attempt");
    }
    // Throttle not at minimum: never low, or low for less than the hold time.
    {
        Sim s = disarmed();
        s.throttle = 50;
        s.run(700000);
        s.flags = kFlagArm;
        s.run(50000);
        const Event* e = s.last(EventKind::kRefusal);
        check(e && e->refusal == Refusal::kThrottleNotMin && e->mask == bit(Refusal::kThrottleNotMin),
              "throttle above minimum: refused, throttle_not_min only");
    }
    {
        Sim s = disarmed();
        s.throttle = 500;
        s.run(500000);
        s.throttle = 0;
        s.run(400000);  // 400 ms at minimum, less than the 500 ms hold
        s.flags = kFlagArm;
        s.run(20000);
        const Event* e = s.last(EventKind::kRefusal);
        check(s.out.state == State::kDisarmed && e && e->refusal == Refusal::kThrottleNotMin,
              "throttle at minimum for 400 ms: refused");
    }
    {
        Sim s = disarmed();
        s.throttle = board::kThrottleMinPermille;  // at the minimum band edge
        s.run(700000);
        s.flags = kFlagArm;
        s.run(20000);
        check(s.out.state == State::kArmed, "throttle exactly at the minimum band edge arms");
    }
    // No link: no frame can carry an attempt; the blocking mask shows it.
    {
        Sim s = disarmed();
        s.sending = false;
        s.run(500000);
        check((s.out.blocking & bit(Refusal::kNoLink)) != 0, "no link: blocking mask holds no_link");
    }
}

void test_combinations() {
    // Every pair of the spoilable conditions: the mask holds both, the reason is the earlier one.
    struct Cond {
        Refusal r;
        std::function<void(Sim&)> spoil;
    };
    const std::vector<Cond> conds = {
        {Refusal::kKill, [](Sim& s) { s.flags |= kFlagKill; }},
        {Refusal::kImuUnhealthy, [](Sim& s) { s.in.imu_healthy = false; }},
        {Refusal::kGyroCal, [](Sim& s) { s.in.gyro_cal_done = false; }},
        {Refusal::kTilted, [](Sim& s) { s.in.tilt_ok = false; }},
        {Refusal::kMoving, [](Sim& s) { s.in.at_rest = false; }},
        {Refusal::kThrottleNotMin, [](Sim& s) { s.throttle = 300; }},
    };
    for (size_t a = 0; a < conds.size(); ++a) {
        for (size_t b = a + 1; b < conds.size(); ++b) {
            Sim s = disarmed();
            s.throttle = 0;
            s.flags = 0;
            s.run(700000);
            conds[a].spoil(s);
            conds[b].spoil(s);
            s.flags |= kFlagArm;
            s.run(60000);
            const Event* e = s.last(EventKind::kRefusal);
            const uint16_t want = bit(conds[a].r) | bit(conds[b].r);
            const std::string nm = std::string(name(conds[a].r)) + " + " + name(conds[b].r);
            check(s.out.state == State::kDisarmed, nm + ": not armed");
            check(e && (e->mask & want) == want, nm + ": both reasons in the mask");
            check(e && e->refusal == conds[a].r, nm + ": first reason reported");
        }
    }
    // All conditions failing at once.
    Sim s = disarmed();
    s.in.imu_healthy = s.in.gyro_cal_done = s.in.at_rest = s.in.tilt_ok = false;
    s.throttle = 400;
    s.flags = kFlagArm | kFlagKill;
    s.run(50000);
    s.flags = kFlagKill;
    s.run(50000);
    s.flags = kFlagArm | kFlagKill;
    s.run(50000);
    check(s.out.state == State::kDisarmed, "everything failing: not armed");
}

void test_edge_rules() {
    // Arm flag high from the first frame after boot: refused (held across boot).
    {
        Sim s;  // the very first frame already carries the arm flag
        s.flags = kFlagArm;
        s.run(1500000);
        const Event* e = s.last(EventKind::kRefusal);
        check(s.out.state == State::kDisarmed && e && e->refusal == Refusal::kNoArmEdge,
              "arm held from boot: refused, arm_held_no_edge");
        check(s.count(EventKind::kRefusal) == 1, "arm held: one refusal, not one per frame");
        s.flags = 0;
        s.run(700000);
        s.flags = kFlagArm;
        s.run(50000);
        check(s.out.state == State::kArmed, "then low and high again: arms");
    }
    // Held across a reconnect while DISARMED.
    {
        Sim s = disarmed();
        s.flags = kFlagArm;
        s.run(100000);
        s.sending = false;
        s.run(300000);
        s.sending = true;
        s.run(800000);
        check(s.out.state == State::kDisarmed, "arm held across a reconnect: not armed");
        const Event* e = s.last(EventKind::kRefusal);
        check(e && e->refusal == Refusal::kNoArmEdge, "arm held across a reconnect: arm_held_no_edge");
    }
    // A reconnect between the low and the high: the low before the gap does not count.
    {
        Sim s = disarmed();
        s.flags = 0;
        s.run(700000);
        s.sending = false;
        s.run(200000);
        s.flags = kFlagArm;
        s.sending = true;
        s.run(700000);
        check(s.out.state == State::kDisarmed, "low before a gap, high after it: not armed");
    }
    // Switching transport counts as a reconnect.
    {
        Sim s = disarmed();
        s.flags = 0;
        s.run(700000);
        s.flags = kFlagArm;
        s.transport = 1;
        s.run(100000);
        check(s.out.state == State::kDisarmed, "arm edge on a new transport: not armed");
    }
    // A refused attempt consumes the edge.
    {
        Sim s = disarmed();
        s.flags = 0;
        s.run(700000);
        s.in.at_rest = false;
        s.flags = kFlagArm;
        s.run(50000);
        s.in.at_rest = true;
        s.run(1000000);
        check(s.out.state == State::kDisarmed, "refused attempt: arm held afterwards does not arm");
    }
    // Disarm by clearing the flag: not a cut, not latched.
    {
        Sim s = disarmed();
        s.arm_sequence(300);
        s.flags = 0;
        s.run(40000);
        const Event* e = s.last(EventKind::kDisarm);
        check(s.out.state == State::kDisarmed && e && !s.out.latched, "arm flag cleared: disarm, not latched");
        check(s.count(EventKind::kCut) == 0, "arm flag cleared: no cut event");
    }
}

void test_link_loss_threshold() {
    Sim s = disarmed();
    s.arm_sequence(100);
    // Stop sending and step in 1 us increments around the threshold.
    s.sending = false;
    const int64_t rx = s.in.sp_rx_us;
    s.run(rx + board::kLinkSilenceCutUs - 1 - s.now, 1);
    check(s.out.state == State::kArmed, "age 150 ms - 1 us: still ARMED");
    s.run(1, 1);
    check(s.now - rx == board::kLinkSilenceCutUs && s.out.state == State::kArmed, "age exactly 150 ms: still ARMED");
    s.run(1, 1);
    const Event* e = s.last(EventKind::kCut);
    check(s.out.state == State::kDisarmed && e && e->cut == Cut::kLinkSilence, "age 150 ms + 1 us: cut, link_silence");
    check(e && e->t_us - e->sp_rx_us == board::kLinkSilenceCutUs + 1, "cut decision age recorded (150.001 ms)");
    check(s.out.latched && s.out.force_low && s.out.duty_permille == 0, "cut: latched, forced low, duty 0");
}

void test_kill_and_imu() {
    {
        Sim s = disarmed();
        s.arm_sequence(400);
        s.flags = kFlagArm | kFlagKill;
        s.run(20000);
        const Event* e = s.last(EventKind::kCut);
        check(s.out.state == State::kDisarmed && e && e->cut == Cut::kKill, "kill: cut, kill");
        check(e && e->t_us == e->sp_rx_us, "kill: decided in the iteration that sees the frame");
    }
    {
        Sim s = disarmed();
        s.arm_sequence(400);
        s.in.imu_missing = true;
        const int64_t last_pulse = s.now - 2001;
        s.in.imu_last_us = last_pulse;
        s.run(1000);
        const Event* e = s.last(EventKind::kCut);
        check(s.out.state == State::kDisarmed && e && e->cut == Cut::kImuMissing, "missing data-ready: cut, imu_missing");
        check(e && e->imu_last_us == last_pulse && e->t_us - last_pulse == 3001, "missing data-ready: last pulse time recorded");
    }
}

void test_latch_and_rearm() {
    Sim s = disarmed();
    s.arm_sequence(200);
    s.sending = false;
    s.run(300000);
    check(s.out.state == State::kDisarmed && s.out.latched, "silence: cut and latched");
    // The client comes back with arm still high and throttle low: never re-arms by itself.
    s.sending = true;
    s.throttle = 0;
    s.run(5000000);
    check(s.out.state == State::kDisarmed && s.out.latched, "no automatic re-arm over 5 s with arm held");
    // Throttle high plus a fresh edge: refused.
    s.throttle = 300;
    s.flags = 0;
    s.run(100000);
    s.flags = kFlagArm;
    s.run(100000);
    check(s.out.state == State::kDisarmed && s.out.latched, "re-arm with throttle high: refused, still latched");
    // Throttle low plus a fresh edge: re-arms and clears the latch.
    s.throttle = 0;
    s.flags = 0;
    s.run(700000);
    s.flags = kFlagArm;
    s.run(40000);
    check(s.out.state == State::kArmed && !s.out.latched, "re-arm with throttle low and a fresh edge");
    // A cut while ARMED by a kill, then re-arm needs the kill cleared.
    s.flags = kFlagArm | kFlagKill;
    s.run(40000);
    s.flags = kFlagKill;
    s.run(700000);
    s.flags = kFlagArm | kFlagKill;
    s.run(40000);
    check(s.out.state == State::kDisarmed, "after a kill: arming refused while the kill flag is set");
}

// Random disturbances interleaved with genuine arm sequences: outputs are never allowed outside
// ARMED (checked every iteration by Sim::invariants), and ARMED never survives a missing IMU or a
// silent link.
void test_random() {
    std::mt19937 rng(12345);
    Sim s = disarmed();
    long armed_steps = 0, steps = 0;
    for (int episode = 0; episode < 3000; ++episode) {
        if (rng() % 2 == 0) {
            s.in.imu_missing = false;
            s.in.imu_healthy = s.in.at_rest = s.in.tilt_ok = s.in.accel_ok = s.in.gyro_cal_done = true;
            s.sending = true;
            s.arm_sequence(static_cast<uint16_t>(rng() % 1100));
        }
        const int n = 1 + static_cast<int>(rng() % 400);
        for (int i = 0; i < n; ++i) {
            const uint32_t r = rng();
            if (r % 97 == 0) s.sending = !s.sending;
            if (r % 53 == 0) s.flags = static_cast<uint8_t>(rng() & 0x07);
            if (r % 41 == 0) s.throttle = static_cast<uint16_t>(rng() % 1400);
            if (r % 211 == 0) s.in.imu_missing = !s.in.imu_missing;
            if (r % 307 == 0) s.in.imu_healthy = !s.in.imu_healthy;
            if (r % 401 == 0) s.in.at_rest = !s.in.at_rest;
            if (r % 503 == 0) s.in.tilt_ok = !s.in.tilt_ok;
            if (r % 997 == 0) s.transport ^= 1;
            s.run(500 + static_cast<int64_t>(rng() % 3) * 500);
            steps++;
            armed_steps += s.out.state == State::kArmed;
            if (s.out.state == State::kArmed) {
                check(!s.in.imu_missing, "random: never ARMED with data-ready missing");
                check(s.now - s.in.sp_rx_us <= board::kLinkSilenceCutUs, "random: never ARMED with a silent link");
            }
        }
    }
    check(armed_steps > 0 && armed_steps < steps, "random: both ARMED and not ARMED were exercised");
    std::printf("random run: %ld of %ld disturbed iterations ARMED, %d cuts, %d refusals, %zu events\n",
                armed_steps, steps, s.count(EventKind::kCut), s.count(EventKind::kRefusal), s.events.size());
}

void test_flash_guard() {
    check(flash_writes_allowed(State::kBoot) && flash_writes_allowed(State::kSafe) &&
              flash_writes_allowed(State::kDisarmed) && !flash_writes_allowed(State::kArmed),
          "flash writes refused only in ARMED");
    publish_state(State::kArmed);
    check(!flash_writes_allowed(), "published ARMED: flash writes refused");
    publish_state(State::kDisarmed);
    check(flash_writes_allowed(), "published DISARMED: flash writes allowed");
}

// ---- Rest detector.

RestConfig rest_config() {
    return RestConfig{board::kGyroCalSamples, board::kGyroCalSdMaxDps, board::kGyroCalRangeMaxDps,
                      board::kGyroCalBiasMaxDps, board::kAccelCalNormSdMaxG, board::kAccelRefMinG,
                      board::kAccelRefMaxG, board::kMotionBlockSamples, board::kMotionGyroDps,
                      board::kMotionAccelFrac, board::kRestHoldUs, board::kAccelValidMinFrac,
                      board::kAccelValidMaxFrac,
                      static_cast<float>(std::cos(board::kTiltLimitDeg * 3.14159265358979 / 180.0))};
}

struct Bench {
    RestDetector d{rest_config()};
    std::mt19937 rng{7};
    std::normal_distribution<float> noise{0.0f, 0.1f};   // measured gyro noise ~0.1 dps sd
    std::normal_distribution<float> anoise{0.0f, 0.002f};
    int64_t now = 0;
    float bias[3] = {1.4f, -1.1f, -2.25f};                // M6 bench bias
    float g0 = 1.068f;                                    // R6
    void feed(int n, float rate_dps = 0, float tilt_deg = 0, float gscale = 1.0f) {
        const float t = tilt_deg * 3.14159265f / 180.0f;
        for (int i = 0; i < n; ++i) {
            now += 1000;
            const float g[3] = {bias[0] + rate_dps + noise(rng), bias[1] + noise(rng), bias[2] + noise(rng)};
            const float a[3] = {g0 * gscale * std::sin(t) + anoise(rng), anoise(rng),
                                g0 * gscale * std::cos(t) + anoise(rng)};
            d.sample(now, g, a);
        }
    }
};

void test_rest_detector() {
    {
        Bench b;
        b.feed(1999);
        check(!b.d.cal_done(), "calibration needs the full 2000-sample window");
        b.feed(1);
        check(b.d.cal_done() && b.d.cal_restarts() == 0, "calibration completes after 2 s at rest");
        check(std::fabs(b.d.bias()[2] - b.bias[2]) < 0.02f, "bias estimate within 0.02 dps");
        check(std::fabs(b.d.g0() - 1.068f) < 0.005f && b.d.ref_ok(), "g0 = 1.068 g accepted (R6)");
        check(b.d.cal_sd_max() > 0.05f && b.d.cal_sd_max() < 0.2f, "calibration sd near the 0.1 dps noise");
        b.feed(10);
        check(b.d.at_rest(b.now) && b.d.accel_ok() && b.d.tilt_ok(), "after calibration: at rest, accel ok, level");
        b.feed(150, 20.0f);  // rotating at 20 dps
        check(!b.d.at_rest(b.now), "rotation: not at rest");
        b.feed(400);
        check(!b.d.at_rest(b.now), "400 ms after the motion: still not at rest");
        b.feed(250);
        check(b.d.at_rest(b.now), "more than 500 ms after the motion: at rest");
        b.feed(10, 0, 25.0f);
        check(b.d.accel_ok() && !b.d.tilt_ok(), "25 degrees: tilted, accelerometer still valid");
        b.feed(10, 0, 15.0f);
        check(b.d.tilt_ok(), "15 degrees: within the 20 degree limit");
        b.feed(10, 0, 0, 0.8f);
        check(!b.d.accel_ok() && !b.d.tilt_ok(), "|a| = 0.8 g0: accelerometer not trusted");
        b.feed(10, 0, 180.0f);
        check(!b.d.tilt_ok(), "upside down: tilted");
    }
    {
        Bench b;
        b.feed(1000);
        b.feed(20, 30.0f);  // a nudge during calibration
        b.feed(1990);
        check(!b.d.cal_done() && b.d.cal_restarts() >= 1, "motion during calibration restarts the window");
        b.feed(11);  // the sample that showed the motion is not part of the new window
        check(b.d.cal_done(), "then a clean 2000-sample window completes it");
    }
    {
        Bench b;
        b.bias[0] = 12.0f;  // beyond the sanity bound
        b.feed(6000);
        check(!b.d.cal_done() && b.d.cal_restarts() >= 2, "bias beyond 10 dps: calibration never completes");
    }
    {
        Bench b;
        b.g0 = 0.7f;
        b.feed(2000);
        check(b.d.cal_done() && !b.d.ref_ok() && !b.d.accel_ok(), "g0 = 0.7 g: accelerometer bad");
    }
}

}  // namespace

int main() {
    test_boot_transitions();
    test_arm_and_output();
    test_each_condition_alone();
    test_combinations();
    test_edge_rules();
    test_link_loss_threshold();
    test_kill_and_imu();
    test_latch_and_rearm();
    test_random();
    test_flash_guard();
    test_rest_detector();
    std::printf("safety host test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
