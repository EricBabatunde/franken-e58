#pragma once
#include <cstdint>

#include "esp_err.h"

// GPTimer heartbeat watchdog (M7b, blueprint 4.2 and 4.8, decisions D1 and D2).
//
// A GPTimer alarm every board::kWdtPeriodUs, interrupt allocated on Core 1 (start() is called by
// the flight task) at level board::kWdtIntrLevel. The ISR reads the flight task heartbeat time and
// the last IMU data-ready timestamp and, ONLY while the published safety state is ARMED, cuts when
//   - the heartbeat is older than board::kWdtHeartbeatStallUs (5 ms), or
//   - data-ready has been missing for longer than board::kImuMissingCutUs (3 ms).
// A cut is hal_mcpwm::hard_cut(): every generator forced low through the IRAM force path, then the
// hard latch set. While the latch is set the ISR makes no further decision. In SAFE and DISARMED
// the ISR only counts. No logging, no allocation, no lock shared with Core 0.
namespace wdt {

enum class Reason : uint8_t { kNone = 0, kHeartbeat = 1, kImuMissing = 2 };  // = safety::kHard*

struct StartReport {
    esp_err_t err = ESP_OK;
    uint32_t period_us = 0;
    uint32_t resolution_hz = 0;
    uint32_t alarm_ticks = 0;
    int intr_level = 0;
    int core = -1;
};

// The latest cut decided by the ISR. Times are esp_timer microseconds truncated to 32 bits.
struct Trip {
    uint32_t seq = 0;            // increments with every cut (0: none yet)
    Reason reason = Reason::kNone;
    uint32_t t_decide_us = 0;    // ISR time of the decision
    uint32_t ref_us = 0;         // last heartbeat (kHeartbeat) or last data-ready pulse (kImuMissing)
    uint32_t force_cycles = 0;   // CPU cycles from the decision to the force written (hard_cut)
};

// Per-window ISR cost, reset by take_window().
struct IsrWindow {
    uint32_t count = 0;          // alarms handled
    uint32_t cyc_min = 0;        // our callback body only, CPU cycles
    uint32_t cyc_max = 0;
    uint32_t cyc_sum = 0;
    uint32_t late_max_ticks = 0; // alarm to callback entry, timer ticks (board::kWdtResolutionHz)
    uint32_t late_sum_ticks = 0;
    uint32_t late_max_t_us = 0;  // M7c: 32-bit esp_timer time at the callback entry of the worst delay
};

StartReport start();               // flight task only (Core 1), once, after the IMU interrupt is installed
void heartbeat(uint32_t now_us);   // flight task, every iteration, including wait timeouts
uint32_t isr_count();              // total alarms handled (wraps)
Trip last_trip();                  // read after hal_mcpwm::hard_latched() is seen (same core as the ISR)
IsrWindow take_window();           // flight task: the current window, then a reset on the next alarm

}  // namespace wdt
