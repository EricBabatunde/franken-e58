#pragma once
#include <cstdint>

#include "esp_err.h"

// Motor outputs on board::kMotorGpio (blueprint 3.2): one MCPWM timer at board::kPwmHz,
// board::kPwmPeriodTicks per period, four generators, duty in permille. Init order: every
// generator is forced low before its actions are set and before the timer is enabled, so no
// pulse can leave a pin during start-up.
//
// set_all_permille(), force_low_all() and release_all() are IRAM-safe, take no lock and may be
// called from an ISR. The force acts on the generators directly (continuous force, immediate
// update), independent of the compare registers.
namespace hal_mcpwm {

struct InitReport {
    esp_err_t err = ESP_OK;
    bool forced_before_start = false;   // every generator read back as forced low before the timer ran
    bool ids_verified = false;          // operator and generator IDs confirmed through the register
    int oper_id[4] = {-1, -1, -1, -1};
    int gen_id[4] = {-1, -1, -1, -1};
    uint32_t resolution_hz = 0;
    uint32_t period_ticks = 0;
};

// Call once from Core 0 or Core 1 at start-up (allocates driver objects). Outputs end forced low.
InitReport init();

void set_all_permille(uint16_t permille);  // clamped to 0..1000; compare updated at the counter reset
void force_low_all();                      // all four generators forced low, immediately
void release_all();                        // generators follow the compare value again
bool all_forced_low();                     // register read-back: every generator forced low
uint16_t duty_permille();                  // last value set

}  // namespace hal_mcpwm
