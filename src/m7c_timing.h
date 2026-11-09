#pragma once

// Module 7c timing (FE_M7C_TEST, Core 0 side): conditions, latency percentiles and the synthetic
// load, on top of the M7b bench (src/m7b_bench.cpp). The histograms are kept on Core 1 by the flight
// task (CONFIG_FE_LATENCY_HIST, bench builds only). Spec: docs/M7c_package.md.
//
// Console keys (UART0 only, never reachable from the network; none can arm or drive an output):
//   0 idle AP, 1 UDP 50 Hz, 2 WebSocket 50 Hz, 3 page serving, 4 soak: start a condition (labels the
//     log; the matching traffic tool is started by hand). Starting one ends the current one.
//   n  end the current condition and print its final percentiles.
//   L  synthetic load on/off (board::kSyntheticLoadUs after each IMU read).

void m7c_boot_report();              // once, after the M7b CONFIG line
bool m7c_key(int ch);                // true if the key was an M7c key (handled)
void m7c_poll();                     // bench loop: the 60 s percentile lines
const char* m7c_keys_help();         // "" when the M7c bench is not built
