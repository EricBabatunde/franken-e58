#pragma once
#include <cstdint>

#include "esp_err.h"

// Periodic GPTimer alarm (M7b): the hardware behind the heartbeat watchdog. Thin wrapper over the
// ESP-IDF 5.5.3 GPTimer driver (driver/gptimer.h).
//
// The interrupt is allocated on the core that calls start_periodic() (gptimer_register_event_callbacks
// allocates it; verify against IDF docs: GPTimer, interrupt allocation). The IDF default ISR is in
// IRAM (CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y); it takes the timer group spinlock briefly to clear the
// interrupt and re-enable the alarm. No other GPTimer exists in this firmware, so that spinlock is
// taken only on the calling core.
namespace hal_gptimer {

// Called from the ISR on every alarm. Must be IRAM_ATTR. `late_ticks`: counter value when the ISR
// read it, i.e. ticks since the alarm fired (the counter reloads to 0 at the alarm).
using Callback = void (*)(void* arg, uint32_t late_ticks);

struct Report {
    esp_err_t err = ESP_OK;
    uint32_t resolution_hz = 0;
    uint32_t alarm_ticks = 0;
    int intr_level = 0;
};

// Creates the timer, registers `cb`, sets an auto-reload alarm every `period_us` and starts it.
// Init only (allocates the driver object and the interrupt). Call once.
Report start_periodic(uint32_t resolution_hz, uint32_t period_us, int intr_level, Callback cb, void* arg);

}  // namespace hal_gptimer
