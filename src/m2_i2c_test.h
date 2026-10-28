#pragma once
// Module 2 bench test. Not part of the flight stack. Delete before Module 7.
// Scans both buses, reads each sensor's ID, runs a recovery self-test, then starts a
// 10-minute soak (Bus A at 1 kHz on Core 1, Bus B at 50 Hz on Core 0) and a reporter.
void m2_start();
