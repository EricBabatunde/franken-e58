#pragma once
#include <cstdint>

// Module 3 bench test. Not part of the flight stack. Delete before Module 7.
// A Core 1 task owns bus A and the MPU-6500 driver and runs Phase A1 (10 s, DLPF_CFG 0,
// count raw pulses), Phase A2 (60 s, DLPF_CFG 0, read on every 8th pulse) and Phase B
// (600 s, compiled-in DLPF_CFG and decimation). A Core 0 task prints the results.
void m3_start();

// Phase B only, for the M4 interference test (P5). Same tasks and output as Phase B.
struct M3PhaseBResult {
    double rate_hz;
    double iv_sd_us;       // notify-interval standard deviation
    double jitter_us;      // max deviation of the interval from its mean
    double burst_avg_us;
    uint32_t wake_max_us;  // ISR-to-task wake-up maximum
    uint32_t wakes;
    uint32_t reads;
    uint32_t missed;
    uint32_t timeouts;
    uint32_t i2c_errors;
};
void m3_phase_b_start(uint32_t seconds);
bool m3_phase_b_result(M3PhaseBResult* out);  // true once the Phase B summary has been printed
