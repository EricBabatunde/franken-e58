#pragma once
#include <cstdint>

// Module 3 bench test. Not part of the flight stack. Delete before Module 7.
// A Core 1 task owns bus A and the MPU-6500 driver and runs Phase A1 (10 s, DLPF_CFG 0,
// count raw pulses), Phase A2 (60 s, DLPF_CFG 0, read on every 8th pulse) and Phase B
// (600 s, compiled-in DLPF_CFG and decimation). A Core 0 task prints the results.
void m3_start();

// Phase B only, for the M4 interference test (P5) and the M5 runs. Same tasks and output as
// Phase B. The first call creates the tasks. A further call, made only after
// m3_phase_b_result() has returned true, starts another run on the same tasks.
struct M3PhaseBResult {
    double rate_hz;
    double iv_sd_us;       // notify-interval standard deviation
    double jitter_us;      // max deviation of the interval from its mean
    double burst_avg_us;
    uint32_t burst_max_us;
    uint32_t wake_min_us;  // ISR-to-task wake-up minimum, average and maximum
    double wake_avg_us;
    uint32_t wake_max_us;
    uint32_t wakes;
    uint32_t reads;
    uint32_t missed;
    uint32_t timeouts;
    uint32_t i2c_errors;
};
void m3_phase_b_start(uint32_t seconds);
bool m3_phase_b_result(M3PhaseBResult* out);  // true once the Phase B summary has been printed

// M6: a function called on Core 1 after every successful Phase B read, with the number of the
// 10 s window the sample belongs to (the same number as the "B wNN" line). It runs inside the
// flight-style loop, so it must not log, allocate or take a lock shared with Core 0. Set it
// before m3_phase_b_start(); null (the default) disables it.
using M3SampleHook = void (*)(uint32_t window);
void m3_set_sample_hook(M3SampleHook hook);
