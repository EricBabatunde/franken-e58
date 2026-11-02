#pragma once

// Module 5 bench test (FE_M5_TEST): LittleFS mount, Wi-Fi AP, static pages, and the M3 Phase B
// IMU loop on Core 1 under Wi-Fi (M5-IMU-1 idle AP, M5-IMU-2 under HTTP load). Core 0 prints
// everything; Core 1 stays silent. Deleted before Module 7.
void m5_start();
