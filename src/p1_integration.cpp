#include "p1_integration.h"

#include "board_config.h"

#if FE_P1_INTEGRATION

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_i2c.h"
#include "m7c_timing.h"
#include "tof_task.h"

namespace {

const char* TAG = "p1";

constexpr int64_t kReportUs = 10LL * 1000 * 1000;
constexpr int kStartWaitPolls = 30;   // up to 300 ms for the ToF init (boot, reset, configuration)
constexpr uint32_t kStartPollMs = 10;

TofStatus s_prev{};                    // the counters at the previous TOF line
TofStatus s_now{};
int64_t s_next_report = 0;
int64_t s_prev_report = 0;
uint32_t s_window = 0;
uint32_t s_episodes_seen = 0;
bool s_in_episode = false;

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }

const char* state_name(TofState s) {
    switch (s) {
        case TofState::kStarting: return "starting";
        case TofState::kRunning:  return "running";
        case TofState::kFailed:   return "FAILED";
    }
    return "?";
}

// Range statuses seen in the window, as "name count" pairs (only the non-zero ones).
void status_hist(const TofStatus& a, const TofStatus& b, char* buf, size_t len) {
    size_t n = 0;
    buf[0] = '\0';
    for (int i = 0; i < 16 && n < len; ++i) {
        const uint32_t d = a.status_count[i] - b.status_count[i];
        if (d == 0) continue;
        const char* name = i < 15 ? drv_vl53l1x::range_status_name(static_cast<drv_vl53l1x::RangeStatus>(i)) : "other";
        n += std::snprintf(buf + n, len - n, "%s%s %u", n ? ", " : "", name, u(d));
    }
    if (n == 0) std::snprintf(buf, len, "none");
}

void report() {
    const double secs = (esp_timer_get_time() - s_prev_report) / 1e6;
    const uint32_t readings = s_now.readings - s_prev.readings;
    const uint32_t valid = s_now.valid - s_prev.valid;
    const uint32_t nodata = s_now.no_data - s_prev.no_data;
    const uint32_t ioerr = s_now.io_errors - s_prev.io_errors;
    char hist[200];
    status_hist(s_now, s_prev, hist, sizeof(hist));
    const uint32_t age_ms =
        s_now.last_reading_us ? (static_cast<uint32_t>(esp_timer_get_time()) - s_now.last_reading_us) / 1000 : 0;
    ESP_LOGI(TAG, "TOF w%02u %.1fs | rate %.2f Hz | readings %u valid %u (%.1f %%) no-data %u | bus B I/O errors %u"
                  " (total %u) | status: %s | latest %u mm %s, age %u ms | state %s, inits %u, no-data episodes %u",
             u(s_window), secs, secs > 0 ? readings / secs : 0.0, u(readings), u(valid),
             readings ? 100.0 * valid / readings : 0.0, u(nodata), u(ioerr), u(s_now.io_errors), hist,
             u(s_now.last_distance_mm), s_now.last_valid ? "valid" : "not valid", u(age_ms), state_name(s_now.state),
             u(s_now.inits), u(s_now.nodata_episodes));
}

}  // namespace

void p1_start() {
    tof_task_start();
    bool have = false;
    for (int i = 0; i < kStartWaitPolls; ++i) {
        vTaskDelay(pdMS_TO_TICKS(kStartPollMs));
        if (tof_status(&s_now) && s_now.state != TofState::kStarting) {
            have = true;
            break;
        }
    }
    const TaskHandle_t h = xTaskGetHandle("tof");
    const int core = h ? static_cast<int>(xTaskGetCoreID(h)) : -2;
    const unsigned prio = h ? static_cast<unsigned>(uxTaskPriorityGet(h)) : 0u;
    if (!have) {
        ESP_LOGE(TAG, "TOF: FAIL | no status from tof_task within %u ms | task core %d priority %u",
                 u(kStartWaitPolls * kStartPollMs), core, prio);
    } else {
        const bool ok = s_now.state == TofState::kRunning;
        ESP_LOGI(TAG, "TOF: %s | state %s | bus B %s (lines %s, %u clocks) | init %s (step %u, model 0x%04X, boot %u us)"
                      " | start %s | task core %d priority %u | readings feed only this reporter, never the safety logic",
                 ok ? "PASS" : "FAIL", state_name(s_now.state), esp_err_to_name(s_now.bus_err),
                 hal_i2c::line_action_name(s_now.bus_lines.action), u(s_now.bus_lines.clocks),
                 esp_err_to_name(s_now.init.err), static_cast<unsigned>(s_now.init.step), s_now.init.model_id,
                 u(s_now.init.boot_us), esp_err_to_name(s_now.start_err), core, prio);
    }
    s_prev = s_now;
    s_prev_report = esp_timer_get_time();
    s_next_report = s_prev_report + kReportUs;
    m7c_set_load(true);  // the integration run keeps the 150 us stand-in load on (key L still switches it)
}

void p1_poll() {
    if (!tof_status(&s_now)) return;
    // Readings stopped: the first no-data record after a completed read.
    if (s_now.nodata_episodes != s_episodes_seen) {
        s_episodes_seen = s_now.nodata_episodes;
        s_in_episode = true;
        ESP_LOGW(TAG, "TOF EVENT no-data: last reading t=%u us, first no-data record t=%u us, gap %.1f ms (deadline %u ms)"
                      " | bus B I/O errors so far %u",
                 u(s_now.last_reading_us), u(s_now.nodata_first_us),
                 (s_now.nodata_first_us - s_now.last_reading_us) / 1000.0,
                 u(board::kTofNoDataPeriods * board::kTofPeriodMs), u(s_now.io_errors));
    } else if (s_in_episode && s_now.nodata_first_us == 0 && s_now.last_reading_us != 0) {
        s_in_episode = false;
        ESP_LOGW(TAG, "TOF EVENT readings resumed: reading t=%u us (%s, %u mm), without a re-init (inits %u)",
                 u(s_now.last_reading_us), s_now.last_valid ? "valid" : "not valid", u(s_now.last_distance_mm),
                 u(s_now.inits));
    }
    const int64_t now = esp_timer_get_time();
    if (now >= s_next_report) {
        s_window++;
        report();
        s_prev = s_now;
        s_prev_report = now;
        // From now, not from the missed deadline: page serving can starve this Core 0 task for
        // seconds, and catching up printed several windows of 0.1 s back to back (I2, 9 November).
        s_next_report = now + kReportUs;
    }
}

#else

void p1_start() {}
void p1_poll() {}

#endif  // FE_P1_INTEGRATION
