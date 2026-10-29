#pragma once
// Module 4 bench test. Not part of the flight stack. Delete before Module 7.
// Starts tof_task (Core 0, bus B) and runs P0 to P5 from a Core 0 test task:
// init, 300 s static, accuracy sweep, invalid cases, XSHUT recovery, and 300 s of
// M3 Phase B on Core 1 alongside the ToF (interference and M3 regression check).
void m4_start();
