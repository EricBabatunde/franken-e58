#pragma once
// Module 3 bench test. Not part of the flight stack. Delete before Module 7.
// A Core 1 task owns bus A and the MPU-6500 driver and runs Phase A1 (10 s, DLPF_CFG 0,
// count raw pulses), Phase A2 (60 s, DLPF_CFG 0, read on every 8th pulse) and Phase B
// (600 s, compiled-in DLPF_CFG and decimation). A Core 0 task prints the results.
void m3_start();
