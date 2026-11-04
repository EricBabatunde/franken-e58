#pragma once

// Module 7a bench test (FE_M7A_TEST): motor-output layer, arming and failsafe state machine, on
// the M6 AP with the UDP and WebSocket control paths. NO MOTORS MAY BE CONNECTED: GPIO 4 to 7 are
// measured with a multimeter only. Core 0 prints the banner, the start-up checks, a status line
// every second, one line per event (state change, refusal, cut) and the M3-style IMU window lines.
// Console keys (UART0 only): 'm' toggles the data-ready interrupt (missing-IMU injection),
// 's' prints a summary. Nothing on the console can arm or drive the outputs.
void m7a_start();
