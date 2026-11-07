#pragma once
#include <cstdint>

#include "esp_err.h"

// Motor outputs on board::kMotorGpio (blueprint 3.2): one MCPWM timer at board::kPwmHz,
// board::kPwmPeriodTicks per period, four generators, duty in permille. Init order: every
// generator is forced low before its actions are set and before the timer is enabled, so no
// pulse can leave a pin during start-up.
//
// set_all_permille(), force_low_all(), release_all(), hard_cut(), clear_hard_cut() and hard_latched()
// are IRAM-safe, take no lock and may be called from an ISR on the same core. The force acts on the
// generators directly (continuous force, immediate update), independent of the compare registers.
//
// Hard latch (M7b, output_gate.h): hard_cut() is called ONLY by the GPTimer watchdog ISR (wdt::isr)
// and clear_hard_cut() ONLY by the flight task when the safety state machine enters ARMED. While
// the latch is set, release_all() refuses (returns false) and set_all_permille() writes 0.
//
// Panic: a --wrap of esp_panic_handler (hal_mcpwm.cpp) writes both force registers to "all forced
// low" before the IDF panic handler prints and reboots.
namespace hal_mcpwm {

struct InitReport {
    esp_err_t err = ESP_OK;
    bool forced_before_start = false;   // every generator read back as forced low before the timer ran
    bool ids_verified = false;          // operator and generator IDs confirmed through the register
    int oper_id[4] = {-1, -1, -1, -1};
    int gen_id[4] = {-1, -1, -1, -1};
    uint32_t resolution_hz = 0;
    uint32_t period_ticks = 0;
    bool panic_force_ready = false;     // panic wrapper armed with verified constants
    uint32_t panic_force_val[2] = {0, 0};
};

// Call once from Core 0 or Core 1 at start-up (allocates driver objects). Outputs end forced low.
InitReport init();

uint16_t set_all_permille(uint16_t permille);  // clamped to 0..1000 (0 while latched); returns the value set
void force_low_all();                      // all four generators forced low, immediately
bool release_all();                        // generators follow the compare value again; false while latched
void hard_cut(uint8_t reason);             // watchdog ISR only: force low and latch (reason 1 to 255)
void clear_hard_cut();                     // flight task only, on entering ARMED
uint8_t hard_latched();                    // 0: clear; otherwise the watchdog reason
bool panic_force_ready();
bool all_forced_low();                     // register read-back: every generator forced low
uint16_t duty_permille();                  // last value set

}  // namespace hal_mcpwm
