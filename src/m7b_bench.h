#pragma once

// Module 7b bench test (FE_M7B_TEST): GPTimer heartbeat watchdog and hard latch, panic force-low,
// watchdog configuration, fault injection keys (h, H, T, X, z, plus m and s), IMU recovery while
// SAFE or DISARMED, boot retry, task layout, reset reasons. Spec: docs/M7b_package.md.
// NO MOTORS, MOSFET STAGE OR SHIELD: GPIO 4 to 7 carry 10 kOhm pull-downs and a multimeter only.
void m7b_start();
