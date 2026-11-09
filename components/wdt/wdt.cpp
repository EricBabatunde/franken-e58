#include "wdt.h"

#include <atomic>

#include "board_config.h"
#include "drv_mpu6500.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal_gptimer.h"
#include "hal_mcpwm.h"
#include "safety.h"

namespace wdt {
namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;

// Written by the flight task, read by the ISR (same core).
std::atomic<uint32_t> s_hb_us{0};
std::atomic<uint32_t> s_window_epoch{0};

// Written by the ISR only.
std::atomic<uint32_t> s_count{0};
uint32_t s_isr_epoch = 0;
std::atomic<uint32_t> s_w_count{0}, s_w_cyc_min{UINT32_MAX}, s_w_cyc_max{0}, s_w_cyc_sum{0};
std::atomic<uint32_t> s_w_late_max{0}, s_w_late_sum{0};
std::atomic<uint32_t> s_w_late_max_t{0};  // M7c: entry time of the worst alarm delay (32-bit esp_timer)
Trip s_trip;                               // fields written before s_trip_seq (release)
std::atomic<uint32_t> s_trip_seq{0};

void IRAM_ATTR isr(void*, uint32_t late_ticks) {
    const uint32_t c0 = esp_cpu_get_cycle_count();
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    s_count.store(s_count.load(kRelaxed) + 1, kRelaxed);

    if (safety::published_state() == safety::State::kArmed && hal_mcpwm::hard_latched() == 0) {
        const uint32_t hb = s_hb_us.load(kRelaxed);
        const uint32_t pulse = drv_mpu6500::last_notify_us();
        Reason r = Reason::kNone;
        uint32_t ref = 0;
        if (now - hb > board::kWdtHeartbeatStallUs) {
            r = Reason::kHeartbeat;
            ref = hb;
        } else if (now - pulse > board::kImuMissingCutUs) {
            r = Reason::kImuMissing;
            ref = pulse;
        }
        if (r != Reason::kNone) {
            const uint32_t c1 = esp_cpu_get_cycle_count();
            hal_mcpwm::hard_cut(static_cast<uint8_t>(r));
            const uint32_t c2 = esp_cpu_get_cycle_count();
            s_trip.reason = r;
            s_trip.t_decide_us = now;
            s_trip.ref_us = ref;
            s_trip.force_cycles = c2 - c1;
            const uint32_t seq = s_trip_seq.load(kRelaxed) + 1;
            s_trip.seq = seq;
            s_trip_seq.store(seq, std::memory_order_release);
        }
    }

    // Window statistics (single writer: this ISR). A new epoch from take_window() resets them.
    const uint32_t epoch = s_window_epoch.load(kRelaxed);
    if (epoch != s_isr_epoch) {
        s_isr_epoch = epoch;
        s_w_count.store(0, kRelaxed);
        s_w_cyc_min.store(UINT32_MAX, kRelaxed);
        s_w_cyc_max.store(0, kRelaxed);
        s_w_cyc_sum.store(0, kRelaxed);
        s_w_late_max.store(0, kRelaxed);
        s_w_late_sum.store(0, kRelaxed);
        s_w_late_max_t.store(0, kRelaxed);
    }
    const uint32_t cyc = esp_cpu_get_cycle_count() - c0;
    s_w_count.store(s_w_count.load(kRelaxed) + 1, kRelaxed);
    if (cyc < s_w_cyc_min.load(kRelaxed)) s_w_cyc_min.store(cyc, kRelaxed);
    if (cyc > s_w_cyc_max.load(kRelaxed)) s_w_cyc_max.store(cyc, kRelaxed);
    s_w_cyc_sum.store(s_w_cyc_sum.load(kRelaxed) + cyc, kRelaxed);
    if (late_ticks > s_w_late_max.load(kRelaxed)) {
        s_w_late_max.store(late_ticks, kRelaxed);
        s_w_late_max_t.store(now, kRelaxed);
    }
    s_w_late_sum.store(s_w_late_sum.load(kRelaxed) + late_ticks, kRelaxed);
}

}  // namespace

StartReport start() {
    s_hb_us.store(static_cast<uint32_t>(esp_timer_get_time()), kRelaxed);
    const hal_gptimer::Report g = hal_gptimer::start_periodic(board::kWdtResolutionHz, board::kWdtPeriodUs,
                                                              board::kWdtIntrLevel, &isr, nullptr);
    StartReport r;
    r.err = g.err;
    r.period_us = board::kWdtPeriodUs;
    r.resolution_hz = g.resolution_hz;
    r.alarm_ticks = g.alarm_ticks;
    r.intr_level = g.intr_level;
    r.core = static_cast<int>(xPortGetCoreID());
    return r;
}

void IRAM_ATTR heartbeat(uint32_t now_us) { s_hb_us.store(now_us, kRelaxed); }

uint32_t IRAM_ATTR isr_count() { return s_count.load(kRelaxed); }

Trip IRAM_ATTR last_trip() {
    // A trip is written once per latch (the ISR makes no new decision while the hard latch is set),
    // and the flight task reads it only after seeing the latch, so the record is stable here.
    (void)s_trip_seq.load(std::memory_order_acquire);
    return s_trip;
}

IsrWindow IRAM_ATTR take_window() {
    IsrWindow w;
    w.count = s_w_count.load(kRelaxed);
    w.cyc_min = s_w_cyc_min.load(kRelaxed);
    w.cyc_max = s_w_cyc_max.load(kRelaxed);
    w.cyc_sum = s_w_cyc_sum.load(kRelaxed);
    w.late_max_ticks = s_w_late_max.load(kRelaxed);
    w.late_sum_ticks = s_w_late_sum.load(kRelaxed);
    w.late_max_t_us = s_w_late_max_t.load(kRelaxed);
    s_window_epoch.store(s_window_epoch.load(kRelaxed) + 1, kRelaxed);
    return w;
}

}  // namespace wdt
