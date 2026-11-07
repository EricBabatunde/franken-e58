#include "hal_gptimer.h"

#include "driver/gptimer.h"
#include "esp_attr.h"

namespace hal_gptimer {
namespace {

gptimer_handle_t s_timer = nullptr;
Callback s_cb = nullptr;
void* s_arg = nullptr;

// Runs inside the IDF default GPTimer ISR. With auto-reload the counter restarts from 0 at the
// alarm, so the captured count is the time since the alarm.
bool IRAM_ATTR on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t* ed, void*) {
    s_cb(s_arg, static_cast<uint32_t>(ed->count_value));
    return false;  // the callback never wakes a task
}

}  // namespace

Report start_periodic(uint32_t resolution_hz, uint32_t period_us, int intr_level, Callback cb, void* arg) {
    Report r;
    r.resolution_hz = resolution_hz;
    r.alarm_ticks = static_cast<uint32_t>(static_cast<uint64_t>(resolution_hz) * period_us / 1000000u);
    r.intr_level = intr_level;
    if (s_timer != nullptr || cb == nullptr) {
        r.err = ESP_ERR_INVALID_STATE;
        return r;
    }
    s_cb = cb;
    s_arg = arg;

    gptimer_config_t tc = {};
    tc.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    tc.direction = GPTIMER_COUNT_UP;
    tc.resolution_hz = resolution_hz;
    tc.intr_priority = intr_level;  // 1 to 3 (ESP_INTR_FLAG_LOWMED) for the GPTimer driver
    if ((r.err = gptimer_new_timer(&tc, &s_timer)) != ESP_OK) return r;

    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = on_alarm;
    // Allocates the interrupt on the calling core.
    if ((r.err = gptimer_register_event_callbacks(s_timer, &cbs, nullptr)) != ESP_OK) return r;

    gptimer_alarm_config_t ac = {};
    ac.alarm_count = r.alarm_ticks;
    ac.reload_count = 0;
    ac.flags.auto_reload_on_alarm = 1;
    if ((r.err = gptimer_set_alarm_action(s_timer, &ac)) != ESP_OK) return r;
    if ((r.err = gptimer_enable(s_timer)) != ESP_OK) return r;
    r.err = gptimer_start(s_timer);
    return r;
}

}  // namespace hal_gptimer
