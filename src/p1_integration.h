#pragma once

// Phase 1 integration run (FE_P1_INTEGRATION, Core 0 side; docs/P1_integration_package.md): the
// M7c bench plus the ToF task. Nothing from the ToF feeds the flight task or the safety logic: the
// readings go only to this reporter.
//
// p1_start(): after flight::start(). Starts tof_task (Core 0, bus B), waits up to 300 ms for it to
//   leave the starting state, prints the TOF boot line (init result, bus lines, core, priority) and
//   switches the synthetic load on. The bench prints the task table and the interrupt dump after it,
//   so both include the ToF task and the bus B interrupt.
// p1_poll(): bench loop. A TOF line every 10 s, and an event line when the readings stop (last
//   reading to first no-data record) or resume.
void p1_start();
void p1_poll();
