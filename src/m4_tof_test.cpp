#include "m4_tof_test.h"

#include "board_config.h"

#if FE_M4_TEST

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "drv_vl53l1x.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "m3_imu_test.h"
#include "tof_task.h"

namespace {

const char* TAG = "m4";
using drv_vl53l1x::RangeStatus;

constexpr int kCountdownSeconds = 15;
constexpr uint32_t kP1Seconds = 300;
constexpr uint32_t kP1WindowSeconds = 30;
constexpr uint32_t kP2StepSeconds = 30;
constexpr uint16_t kP2NominalMm[5] = {100, 200, 400, 800, 1200};
constexpr uint32_t kP3Seconds = 20;
constexpr int kP4Cycles = 3;
constexpr uint32_t kP4TimeoutMs = 3000;
constexpr uint32_t kP5Seconds = 300;
constexpr uint32_t kPollMs = 5;                    // test task poll of the ToF slot
constexpr double kRateHz = 1000.0 / board::kTofPeriodMs;  // 40 Hz
constexpr double kRateToleranceHz = 2.0;           // acceptance 3
constexpr double kValidMinPercent = 99.0;          // acceptance 4
// Datasheet figures (DS12385), for comparison only (D-M4-05): short-mode ranging error
// +/-20 mm dark, +/-25 mm at 200 kcps/SPAD (p.22, Table 19), at a 100 ms budget (p.20);
// repeatability +/-1 % to +/-0.15 % (p.21).
constexpr double kDsRangingErrorMm = 20.0;
constexpr double kTapeErrorMm = 5.0;
constexpr double kDsRepeatabilityFraction = 0.01;
// M3 baseline (Phase B, Wi-Fi off) and the acceptance 8 limits.
constexpr double kM3IvSdUs = 0.44, kM3JitterUs = 3.7, kM3BurstAvgUs = 450.4;
constexpr uint32_t kM3WakeMaxUs = 13;
constexpr double kP5IvSdMaxUs = 2.0, kP5JitterMaxUs = 20.0;
constexpr uint32_t kP5WakeMaxUs = 50;

constexpr int kHistNone = 14;    // RangeStatus::kNone and anything above 13
constexpr int kHistNoData = 15;  // no-data records
constexpr int kHistSize = 16;

struct Acc {
    int64_t t_start = 0;
    uint32_t readings = 0;     // readings received, valid or not (no-data records excluded)
    uint32_t valid = 0;
    uint32_t no_data = 0;
    uint32_t seq_gaps = 0;     // records this task missed (should stay 0)
    uint32_t stale = 0;        // invalid record carrying a distance, or a repeated timestamp
    double sum = 0.0;
    double sumsq = 0.0;
    uint16_t min = UINT16_MAX;
    uint16_t max = 0;
    uint32_t hist[kHistSize] = {};
    uint32_t read_min = UINT32_MAX;
    uint32_t read_max = 0;
    uint64_t read_sum = 0;
};

uint32_t s_last_seq = 0;
uint32_t s_last_t = 0;
bool s_have_last_t = false;

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }

void add(Acc& a, const TofSample& s) {
    if (s.no_data) {
        a.no_data++;
        a.hist[kHistNoData]++;
        return;
    }
    a.readings++;
    a.hist[s.status <= 13 ? s.status : kHistNone]++;
    if (!s.valid && s.distance_mm != 0) {
        a.stale++;
    }
    if (s_have_last_t && s.t_us == s_last_t) {
        a.stale++;
    }
    s_last_t = s.t_us;
    s_have_last_t = true;
    if (s.read_us < a.read_min) a.read_min = s.read_us;
    if (s.read_us > a.read_max) a.read_max = s.read_us;
    a.read_sum += s.read_us;
    if (s.valid) {
        a.valid++;
        a.sum += s.distance_mm;
        a.sumsq += static_cast<double>(s.distance_mm) * s.distance_mm;
        if (s.distance_mm < a.min) a.min = s.distance_mm;
        if (s.distance_mm > a.max) a.max = s.distance_mm;
    }
}

// Drops anything published so far, so the next collection starts clean.
void sync_seq() {
    TofSample s{};
    if (tof_read(&s)) {
        s_last_seq = s.seq;
    }
}

// Polls the ToF slot for `ms` milliseconds, adding every new record to each accumulator.
void collect(uint32_t ms, Acc* a, Acc* b = nullptr) {
    const int64_t end = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
    while (esp_timer_get_time() < end) {
        TofSample s{};
        if (tof_read(&s) && s.seq != s_last_seq) {
            const uint32_t gap = s.seq - s_last_seq - 1;
            a->seq_gaps += gap;
            add(*a, s);
            if (b != nullptr) {
                b->seq_gaps += gap;
                add(*b, s);
            }
            s_last_seq = s.seq;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

Acc start_acc() {
    Acc a{};
    a.t_start = esp_timer_get_time();
    return a;
}

double elapsed_s(const Acc& a) { return (esp_timer_get_time() - a.t_start) / 1e6; }
double rate(const Acc& a) { return a.readings / elapsed_s(a); }
double mean(const Acc& a) { return a.valid ? a.sum / a.valid : 0.0; }
double sdev(const Acc& a) {
    if (a.valid < 2) return 0.0;
    const double m = mean(a);
    const double v = a.sumsq / a.valid - m * m;
    return v > 0.0 ? std::sqrt(v) : 0.0;
}
double valid_pct(const Acc& a) { return a.readings ? 100.0 * a.valid / a.readings : 0.0; }
unsigned min_mm(const Acc& a) { return a.valid ? a.min : 0u; }
double read_avg(const Acc& a) { return a.readings ? static_cast<double>(a.read_sum) / a.readings : 0.0; }

// "status:count" for every non-zero histogram bin.
const char* hist_str(const Acc& a, char* buf, size_t len) {
    size_t n = 0;
    buf[0] = '\0';
    for (int i = 0; i < kHistSize && n < len; ++i) {
        if (a.hist[i] == 0) continue;
        const char* name = (i == kHistNoData) ? "no data"
                           : (i == kHistNone) ? drv_vl53l1x::range_status_name(RangeStatus::kNone)
                                              : drv_vl53l1x::range_status_name(static_cast<RangeStatus>(i));
        n += snprintf(buf + n, len - n, "%s%s %u", n ? ", " : "", name, u(a.hist[i]));
    }
    if (n == 0) snprintf(buf, len, "none");
    return buf;
}

void countdown(const char* what) {
    for (int s = kCountdownSeconds; s > 0; --s) {
        ESP_LOGI(TAG, ">>> %s: %d s", what, s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, ">>> %s: measuring now, hold still", what);
}

[[noreturn]] void finish(const char* why) {
    ESP_LOGI(TAG, "M4 bench test %s. Reset the board for a fresh run.", why);
    vTaskDelete(nullptr);
    for (;;) {
    }
}

bool phase0() {
    tof_task_start();
    TofStatus st{};
    for (int i = 0; i < 100; ++i) {  // up to 2 s
        vTaskDelay(pdMS_TO_TICKS(20));
        if (tof_status(&st) && st.state != TofState::kStarting) break;
    }
    const bool ok = st.state == TofState::kRunning;
    ESP_LOGI(TAG, "P0: bus B %s | init step %u (%s) | model ID 0x%04X | boot %u us after XSHUT release"
                  " | start_continuous %s | short mode, budget %u us, period %u ms",
             esp_err_to_name(st.bus_err), static_cast<unsigned>(st.init.step), esp_err_to_name(st.init.err),
             st.init.model_id, u(st.init.boot_us), esp_err_to_name(st.start_err),
             u(board::kTofTimingBudgetUs), u(board::kTofPeriodMs));
    ESP_LOGI(TAG, "SUMMARY P0: %s (model ID 0x%04X, expected 0xEACC)", ok ? "PASS" : "FAIL",
             st.init.model_id);
    return ok;
}

void phase1() {
    char h[200];
    countdown("P1: hold a matte target at about 300 mm");
    sync_seq();
    Acc total = start_acc();
    for (uint32_t w = 1; w <= kP1Seconds / kP1WindowSeconds; ++w) {
        Acc win = start_acc();
        collect(kP1WindowSeconds * 1000, &win, &total);
        ESP_LOGI(TAG, "P1 w%02u | rate %.2f Hz | mean %.1f sd %.2f min %u max %u mm | valid %.1f %%"
                      " | read min/avg/max %u/%.0f/%u us | status: %s",
                 u(w), rate(win), mean(win), sdev(win), min_mm(win), win.max, valid_pct(win),
                 u(win.read_min), read_avg(win), u(win.read_max), hist_str(win, h, sizeof(h)));
    }
    const double r = rate(total);
    const double m = mean(total);
    ESP_LOGI(TAG, "SUMMARY P1: rate %.2f Hz (%.0f +/- %.0f: %s) | valid %.2f %% (>= %.0f %%: %s)"
                  " | mean %.1f sd %.2f min %u max %u mm | sd vs datasheet repeatability 1 %% of mean ="
                  " %.1f mm (DS p.21, 100 ms budget; for review) | 17-byte result read min/avg/max"
                  " %u/%.0f/%u us | no-data %u, stale %u, missed records %u | status: %s",
             r, kRateHz, kRateToleranceHz, std::fabs(r - kRateHz) <= kRateToleranceHz ? "PASS" : "FAIL",
             valid_pct(total), kValidMinPercent, valid_pct(total) >= kValidMinPercent ? "PASS" : "FAIL",
             m, sdev(total), min_mm(total), total.max, m * kDsRepeatabilityFraction,
             u(total.read_min), read_avg(total), u(total.read_max), u(total.no_data), u(total.stale),
             u(total.seq_gaps), hist_str(total, h, sizeof(h)));
}

void phase2() {
    char h[200];
    char what[64];
    double err[5] = {};
    int flagged = 0;
    for (int i = 0; i < 5; ++i) {
        const uint16_t n = kP2NominalMm[i];
        snprintf(what, sizeof(what), "P2: move target to %u mm", n);
        countdown(what);
        sync_seq();
        Acc a = start_acc();
        collect(kP2StepSeconds * 1000, &a);
        err[i] = a.valid ? mean(a) - n : NAN;
        const bool flag = !a.valid || std::fabs(err[i]) > kDsRangingErrorMm + kTapeErrorMm;
        flagged += flag ? 1 : 0;
        ESP_LOGI(TAG, "P2 %4u mm | mean %.1f sd %.2f min %u max %u mm | error %+.1f mm%s | valid %.1f %%"
                      " | status: %s",
                 n, mean(a), sdev(a), min_mm(a), a.max, err[i],
                 flag ? " FLAG (> 25 mm, for review)" : "", valid_pct(a), hist_str(a, h, sizeof(h)));
    }
    ESP_LOGI(TAG, "SUMMARY P2: error at 100/200/400/800/1200 mm = %+.1f/%+.1f/%+.1f/%+.1f/%+.1f mm;"
                  " %d step(s) above %.0f mm (datasheet +/-%.0f mm, DS p.22 Table 19 at 100 ms budget,"
                  " plus %.0f mm tape); for review",
             err[0], err[1], err[2], err[3], err[4], flagged, kDsRangingErrorMm + kTapeErrorMm,
             kDsRangingErrorMm, kTapeErrorMm);
}

void phase3() {
    char h[200];
    countdown("P3: cover the sensor with your hand at about 20 mm");
    sync_seq();
    Acc hand = start_acc();
    collect(kP3Seconds * 1000, &hand);
    ESP_LOGI(TAG, "P3 hand  | readings %u valid %u no-data %u stale %u | status: %s",
             u(hand.readings), u(hand.valid), u(hand.no_data), u(hand.stale), hist_str(hand, h, sizeof(h)));
    countdown("P3: point the sensor at open space (nothing within 1.3 m)");
    sync_seq();
    Acc open = start_acc();
    collect(kP3Seconds * 1000, &open);
    ESP_LOGI(TAG, "P3 open  | readings %u valid %u no-data %u stale %u | status: %s",
             u(open.readings), u(open.valid), u(open.no_data), u(open.stale), hist_str(open, h, sizeof(h)));
    const bool no_stale = hand.stale == 0 && open.stale == 0;
    ESP_LOGI(TAG, "SUMMARY P3: invalid statuses published as invalid with no distance; stale distances"
                  " %u (%s) | valid readings: hand %u, open %u (valid needs \"range valid\" and >= %u mm)",
             u(hand.stale + open.stale), no_stale ? "PASS" : "FAIL", u(hand.valid), u(open.valid),
             u(board::kTofMinValidMm));
}

void phase4() {
    countdown("P4: hold the target at about 300 mm for the reset cycles");
    uint32_t times[kP4Cycles] = {};
    int recovered = 0;
    TofStatus st{};
    tof_status(&st);
    const uint32_t base = st.resets_done;
    for (int i = 0; i < kP4Cycles; ++i) {
        tof_request_reset();
        bool ok = false;
        for (uint32_t waited = 0; waited < kP4TimeoutMs; waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (tof_status(&st) && st.resets_done == base + i + 1 &&
                (st.recovery_us != 0 || st.state == TofState::kFailed)) {
                ok = st.recovery_us != 0;
                break;
            }
        }
        times[i] = ok ? st.recovery_us : 0;
        recovered += ok ? 1 : 0;
        ESP_LOGI(TAG, "P4 reset %d: %s | release to first valid reading %u us (boot %u us) | init step %u",
                 i + 1, ok ? "recovered" : "NOT RECOVERED", u(times[i]), u(st.init.boot_us),
                 static_cast<unsigned>(st.init.step));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "SUMMARY P4: %d/%d recovered (%s) | release to first valid %u/%u/%u us",
             recovered, kP4Cycles, recovered == kP4Cycles ? "PASS" : "FAIL", u(times[0]), u(times[1]),
             u(times[2]));
}

void phase5() {
    ESP_LOGI(TAG, "P5: keep the board still; M3 Phase B runs %u s on Core 1 while the ToF runs on Core 0",
             u(kP5Seconds));
    sync_seq();
    m3_phase_b_start(kP5Seconds);
    Acc tof = start_acc();
    M3PhaseBResult imu{};
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(kP5Seconds + 60) * 1000 * 1000;
    bool have = false;
    while (!(have = m3_phase_b_result(&imu)) && esp_timer_get_time() < deadline) {
        collect(1000, &tof);
    }
    if (!have) {
        ESP_LOGE(TAG, "SUMMARY P5: FAIL, no M3 Phase B result (see the m3 log lines)");
        return;
    }
    const bool ok_missed = imu.missed == 0 && imu.timeouts == 0 && imu.i2c_errors == 0 &&
                           imu.reads == imu.wakes;
    const bool ok_sd = imu.iv_sd_us < kP5IvSdMaxUs;
    const bool ok_jit = imu.jitter_us < kP5JitterMaxUs;
    const bool ok_wake = imu.wake_max_us < kP5WakeMaxUs;
    ESP_LOGI(TAG, "SUMMARY P5: IMU interval sd %.2f us (M3 %.2f; < %.0f: %s) | jitter %.1f us (M3 %.1f;"
                  " < %.0f: %s) | wake max %u us (M3 %u; < %u: %s) | burst avg %.1f us (M3 %.1f)"
                  " | missed %u timeouts %u i2c_err %u (%s) | IMU rate %.2f Hz | ToF rate %.2f Hz,"
                  " valid %.1f %%, no-data %u",
             imu.iv_sd_us, kM3IvSdUs, kP5IvSdMaxUs, ok_sd ? "PASS" : "FAIL", imu.jitter_us, kM3JitterUs,
             kP5JitterMaxUs, ok_jit ? "PASS" : "FAIL", u(imu.wake_max_us), u(kM3WakeMaxUs),
             u(kP5WakeMaxUs), ok_wake ? "PASS" : "FAIL", imu.burst_avg_us, kM3BurstAvgUs,
             u(imu.missed), u(imu.timeouts), u(imu.i2c_errors), ok_missed ? "PASS" : "FAIL", imu.rate_hz,
             rate(tof), valid_pct(tof), u(tof.no_data));
}

void test_task(void*) {
    ESP_LOGI(TAG, "M4 bench test: P0 init, P1 %us static, P2 sweep, P3 invalid cases, P4 resets,"
                  " P5 %us with M3 Phase B. About 15 minutes.", u(kP1Seconds), u(kP5Seconds));
    if (!phase0()) {
        finish("stopped after P0");
    }
    phase1();
    phase2();
    phase3();
    phase4();
    phase5();
    finish("finished");
}

}  // namespace

void m4_start() {
    xTaskCreatePinnedToCore(test_task, "m4_test", 6144, nullptr, 3, nullptr, 0);
}

#endif  // FE_M4_TEST
