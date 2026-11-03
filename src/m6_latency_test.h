#pragma once

// Module 6 bench test (FE_M6_TEST): UDP and WebSocket latency servers on the M5 AP, with the
// M3 Phase B IMU loop on Core 1 reading the setpoint slots once per sample. Core 0 prints
// everything; Core 1 stays silent. The client side is tools/latency_test.py and
// web/latency.html; tools/analyse_m6.py splits the IMU windows by traffic. Deleted before
// Module 7.
void m6_start();
