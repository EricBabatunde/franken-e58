#include "m7b_bench.h"

#include "board_config.h"

#if FE_M7B_TEST

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "flight.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_fs.h"
#include "hal_mcpwm.h"
#include "http_static.h"
#include "link.h"
#include "safety.h"
#include "sdkconfig.h"
#include "udp_server.h"
#include "wdt.h"
#include "wifi_ap.h"
#include "ws_server.h"

#if !FE_IMU_ISR_INSTRUMENT
#error "The M7b bench test needs FE_IMU_ISR_INSTRUMENT"
#endif

namespace {

const char* TAG = "m7b";
namespace ap = comms::wifi_ap;
namespace lk = comms::link;
namespace sp = comms::setpoint;

constexpr uint32_t kPollMs = 50;
constexpr uint32_t kStatusMs = 1000;
constexpr double kCpuMhz = 240.0;  // CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240

// ---- RTC memory (survives software, panic, watchdog and brownout resets, not power-on; verify
// against IDF docs: RTC_NOINIT_ATTR). Written on Core 0 only.
constexpr uint32_t kRtcMagic = 0x4D374221;  // "M7B!"
struct RtcRecord {
    uint32_t magic;
    uint32_t brownouts;         // BROWNOUT resets since power-on
    uint32_t restarts;          // controlled restarts (D5) since power-on
    uint32_t hang_kind;         // fault key whose reset is pending (0: none)
    uint64_t hang_rtc_us;       // esp_rtc_get_time_us() when the key was handled
    uint32_t restart_pending;   // 1: the last reset was a controlled restart (D5)
};
RTC_NOINIT_ATTR RtcRecord s_rtc;

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }

const char* imu_step_name(drv_mpu6500::Step s) {
    using drv_mpu6500::Step;
    switch (s) {
        case Step::kNone:        return "none";
        case Step::kProbe:       return "probe";
        case Step::kWhoAmI:      return "WHO_AM_I";
        case Step::kReset:       return "DEVICE_RESET";
        case Step::kResetCheck:  return "PWR_MGMT_1 after reset";
        case Step::kWrite:       return "register write";
        case Step::kReadback:    return "register read-back";
        case Step::kBadArgument: return "bad argument";
    }
    return "?";
}

const char* reset_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        default:                return "UNKNOWN";
    }
}

const char* fault_key(uint32_t k) {
    switch (static_cast<flight::Fault>(k)) {
        case flight::Fault::kStallShort:  return "h";
        case flight::Fault::kStallMasked: return "H";
        case flight::Fault::kStallLong:   return "T";
        case flight::Fault::kAbort:       return "X";
        case flight::Fault::kImuSleep:    return "z";
        default:                          return "?";
    }
}

unsigned u_min(uint32_t v) { return v == UINT32_MAX ? 0u : static_cast<unsigned>(v); }
double avg(uint64_t sum, uint32_t n) { return n ? static_cast<double>(sum) / n : 0.0; }
double sdev(uint64_t sum, uint64_t sumsq, uint32_t n) {
    if (n < 2) return 0.0;
    const double m = static_cast<double>(sum) / n;
    const double v = static_cast<double>(sumsq) / n - m * m;
    return v > 0.0 ? std::sqrt(v) : 0.0;
}
double cyc_us(double c) { return c / kCpuMhz; }
uint32_t heap_free() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }
uint32_t heap_min() { return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)); }

void blocking_names(uint16_t mask, char* buf, size_t len) {
    size_t n = 0;
    buf[0] = '\0';
    for (unsigned r = 1; r < static_cast<unsigned>(safety::Refusal::kCount); ++r) {
        if ((mask & (1u << r)) && n < len) {
            n += std::snprintf(buf + n, len - n, "%s%s", n ? "," : "", safety::name(static_cast<safety::Refusal>(r)));
        }
    }
    if (n == 0) std::snprintf(buf, len, "none");
}

double tilt_deg(const float a[3]) {
    const double m = std::sqrt(static_cast<double>(a[0]) * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (m <= 0.0) return 0.0;
    double c = a[2] / m;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * 180.0 / M_PI;
}

const char* transport_name(uint8_t t) { return t == sp::kWs ? "ws" : "udp"; }

void print_event(const flight::EventRecord& r) {
    const safety::Event& e = r.ev;
    char mask[200];
    switch (e.kind) {
        case safety::EventKind::kState:
            ESP_LOGW(TAG, "EV #%u t=%lld us %s %s -> %s | thr %u flags 0x%02X", u(e.seq), e.t_us,
                     e.to == safety::State::kArmed ? "ARMED" : "STATE", safety::name(e.from), safety::name(e.to),
                     e.throttle, e.flags);
            break;
        case safety::EventKind::kRefusal:
            blocking_names(e.mask, mask, sizeof(mask));
            ESP_LOGW(TAG, "EV #%u t=%lld us REFUSED %s | all reasons: %s | state %s | thr %u flags 0x%02X", u(e.seq),
                     e.t_us, safety::name(e.refusal), mask, safety::name(e.from), e.throttle, e.flags);
            break;
        case safety::EventKind::kDisarm:
            ESP_LOGW(TAG, "EV #%u t=%lld us DISARM (arm flag cleared) | force-low written %.2f us after the decision"
                          " | ARMED -> DISARMED, not latched",
                     u(e.seq), e.t_us, cyc_us(r.force_cycles));
            break;
        case safety::EventKind::kCut: {
            const double force_us = cyc_us(r.force_cycles);
            switch (e.cut) {
                case safety::Cut::kLinkSilence:
                    ESP_LOGE(TAG, "EV #%u t=%lld us CUT link_silence | decision age %.3f ms (last frame rx t=%lld us)"
                                  " | force-low written %.2f us after the decision (t=%lld us) | ARMED -> DISARMED, latched",
                             u(e.seq), e.t_us, (e.t_us - e.sp_rx_us) / 1000.0, e.sp_rx_us, force_us, r.t_force_us);
                    break;
                case safety::Cut::kKill:
                    ESP_LOGE(TAG, "EV #%u t=%lld us CUT kill | frame rx -> decision %lld us, frame rx -> force-low %lld us"
                                  " (rx t=%lld us) | force-low %.2f us after the decision | ARMED -> DISARMED, latched",
                             u(e.seq), e.t_us, e.t_us - e.sp_rx_us, r.t_force_us - e.sp_rx_us, e.sp_rx_us, force_us);
                    break;
                case safety::Cut::kWdHeartbeat:
                case safety::Cut::kWdImuMissing:
                    ESP_LOGE(TAG, "EV #%u t=%lld us CUT %s | %s -> watchdog decision %u us | force-low written %.2f us"
                                  " after the decision, in the ISR (decision t=%u us) | latch seen by the flight task %lld us"
                                  " after the decision | ARMED -> DISARMED, latched",
                             u(e.seq), e.t_us, safety::name(e.cut),
                             e.cut == safety::Cut::kWdHeartbeat ? "last heartbeat" : "last data-ready",
                             u(r.wd_decide_us - r.wd_ref_us), cyc_us(r.wd_force_cycles), u(r.wd_decide_us),
                             static_cast<long long>(static_cast<int32_t>(static_cast<uint32_t>(e.t_us) - r.wd_decide_us)));
                    break;
                case safety::Cut::kImuMissing:
                    ESP_LOGE(TAG, "EV #%u t=%lld us CUT imu_missing_tick (backup check) | last data-ready -> decision %lld us"
                                  " | force-low %.2f us after the decision | ARMED -> DISARMED, latched",
                             u(e.seq), e.t_us, e.t_us - e.imu_last_us, force_us);
                    break;
                default:
                    ESP_LOGE(TAG, "EV #%u t=%lld us CUT %s | force-low %.2f us after the decision | ARMED -> DISARMED,"
                                  " latched",
                             u(e.seq), e.t_us, safety::name(e.cut), force_us);
                    break;
            }
            break;
        }
    }
}

void print_recovery(const flight::RecoveryRecord& r) {
    ESP_LOGW(TAG, "REC #%u t=%lld us attempt %u/%u state %s | data-ready missing %.1f ms | bus_reset %s | IMU init %s"
                  " (step %s, %s, WHO_AM_I 0x%02X) | %.1f ms",
             u(r.seq), r.t_us, u(r.attempt), u(board::kRecoveryAttempts), safety::name(r.state), r.missing_us / 1000.0,
             esp_err_to_name(r.bus_reset_err), r.ok ? "OK" : "FAILED", imu_step_name(r.step), esp_err_to_name(r.init_err),
             r.who_am_i, r.duration_us / 1000.0);
}

void print_status(const flight::Status& s) {
    char block[200];
    blocking_names(s.blocking, block, sizeof(block));
    char spbuf[96];
    if (s.have_sp) {
        std::snprintf(spbuf, sizeof(spbuf), "%s age %.1f ms seq %u thr %u arm %u kill %u", transport_name(s.sp_transport),
                      s.sp_age_us / 1000.0, s.sp_seq, s.throttle, s.flags & safety::kFlagArm ? 1u : 0u,
                      s.flags & safety::kFlagKill ? 1u : 0u);
    } else {
        std::snprintf(spbuf, sizeof(spbuf), "none yet");
    }
    char cal[128];
    if (s.cal_done) {
        std::snprintf(cal, sizeof(cal), "cal done bias %.2f/%.2f/%.2f dps sd %.3f g0 %.4f%s", s.bias[0], s.bias[1],
                      s.bias[2], s.cal_sd, s.g0, s.ref_ok ? "" : " (OUT OF RANGE)");
    } else {
        std::snprintf(cal, sizeof(cal), "cal running %u/%u restarts %u", u(s.cal_progress), u(board::kGyroCalSamples),
                      u(s.cal_restarts));
    }
    ESP_LOGI(TAG, "ST %s%s last_cut %s | arm blocked by: %s | sp %s | out duty %u permille, gates %s | imu %s, since"
                  " pulse %.1f ms, int %s | %s | rest %s tilt %.1f deg accel %s | arms %u cuts %u refusals %u disarms %u"
                  " | overruns %u sp giveups %u torn %u | wd %s alarms %u hard latch %u | rec attempts %u ok %u episode %u%s"
                  " | flash writes %s",
             safety::name(s.state), s.latched ? " (latched)" : "", safety::name(s.last_cut), block, spbuf,
             s.duty_permille, s.forced_low ? "forced low" : "released", s.imu_healthy ? "healthy" : "UNHEALTHY",
             s.since_pulse_us / 1000.0, s.int_enabled ? "on" : "OFF", cal, s.at_rest ? "yes" : "no", tilt_deg(s.accel),
             s.accel_ok ? "ok" : "BAD", u(s.arms), u(s.cuts), u(s.refusals), u(s.disarms), u(s.overruns),
             u(s.sp_giveups), u(s.sp_torn), s.wd_alive ? "alive" : "DEAD", u(s.wd_count), s.hard_latched,
             u(s.rec_attempts), u(s.rec_ok), u(s.rec_episode), s.rec_exhausted ? " EXHAUSTED" : "",
             s.flash_ok ? "allowed" : "REFUSED");
}

lk::Counters s_prev[sp::kTransports];

void print_window(const flight::Window& w) {
    const double secs = w.length_us / 1e6;
    const double rate = w.length_us ? w.raw_pulses * 1e6 / w.length_us : 0.0;
    const double scale = drv_mpu6500::gyro_dps(1);
    double gm[3], gsd[3];
    for (int i = 0; i < 3; ++i) {
        const double m = w.samples ? static_cast<double>(w.gyro_sum[i]) / w.samples : 0.0;
        const double v = w.samples ? static_cast<double>(w.gyro_sumsq[i]) / w.samples - m * m : 0.0;
        gm[i] = m * scale;
        gsd[i] = (v > 0 ? std::sqrt(v) : 0.0) * scale;
    }
    const double amag = w.samples ? std::sqrt(w.accel_sq_sum / w.samples) : 0.0;
    // Same layout as the M3 "B wNN" line, so the M3 to M7a figures compare directly.
    ESP_LOGI(TAG, "B  w%02u %.2fs | rate %.1f Hz | iv min/max/sd %u/%u/%.1f us | burst min/avg/max %u/%.1f/%u us"
                  " | wake min/avg/max %u/%.1f/%u us | missed %u timeouts %u i2c_err %u | |a| %.4f g"
                  " | gyro mean %.3f/%.3f/%.3f sd %.3f/%.3f/%.3f dps",
             u(w.index), secs, rate, u_min(w.iv_min), u(w.iv_max), sdev(w.iv_sum, w.iv_sumsq, w.iv_n),
             u_min(w.burst_min), avg(w.burst_sum, w.reads), u(w.burst_max), u_min(w.lat_min), avg(w.lat_sum, w.wakes),
             u(w.lat_max), u(w.missed), u(w.timeouts), u(w.i2c_errors), amag, gm[0], gm[1], gm[2], gsd[0], gsd[1],
             gsd[2]);
    lk::Counters c[sp::kTransports];
    uint32_t rx[2], ctl[2], pings[2];
    double wr_avg[2], wr_in_avg[2];
    uint32_t wr_max[2], wr_in_max[2];
    for (int t = 0; t < sp::kTransports; ++t) {
        lk::counters(static_cast<sp::Transport>(t)).read(&c[t]);
        rx[t] = c[t].rx - s_prev[t].rx;
        ctl[t] = c[t].controls - s_prev[t].controls;
        pings[t] = c[t].pings - s_prev[t].pings;
        wr_avg[t] = cyc_us(avg(c[t].wr_cyc_sum - s_prev[t].wr_cyc_sum, ctl[t]));
        wr_in_avg[t] = cyc_us(avg(c[t].wr_in_cyc_sum - s_prev[t].wr_in_cyc_sum, ctl[t]));
        wr_max[t] = c[t].wr_cyc_max;
        wr_in_max[t] = c[t].wr_in_cyc_max;
        s_prev[t] = c[t];
    }
    wifi_sta_list_t list = {};
    int rssi = 0;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0) rssi = list.sta[0].rssi;
    ESP_LOGI(TAG, "w%02u loop | iterations %u work min/avg/max %.1f/%.1f/%.1f us overruns %u | sp reads %u giveups %u"
                  " retry_max %u torn %u | armed %u ms | udp rx=%u ping=%u ctl=%u wr avg/max %.2f/%.2f us (inside"
                  " %.2f/%.2f) | ws rx=%u ping=%u ctl=%u wr avg/max %.2f/%.2f us (inside %.2f/%.2f) | rssi=%d"
                  " | heap=%u min=%u",
             u(w.index), u(w.iterations), cyc_us(u_min(w.work_cyc_min)), cyc_us(avg(w.work_cyc_sum, w.iterations)),
             cyc_us(w.work_cyc_max), u(w.overruns), u(w.sp_reads), u(w.sp_giveups), u(w.sp_retries_max), u(w.sp_torn),
             u(w.armed_iterations), u(rx[0]), u(pings[0]), u(ctl[0]), wr_avg[0], cyc_us(wr_max[0]), wr_in_avg[0],
             cyc_us(wr_in_max[0]), u(rx[1]), u(pings[1]), u(ctl[1]), wr_avg[1], cyc_us(wr_max[1]), wr_in_avg[1],
             cyc_us(wr_in_max[1]), rssi, u(heap_free()), u(heap_min()));
    const double tick_us = 1e6 / board::kWdtResolutionHz;
    ESP_LOGI(TAG, "w%02u wdt | alarms %u (expected %.0f) | ISR body min/avg/max %.2f/%.2f/%.2f us | alarm -> ISR entry"
                  " avg/max %.2f/%.2f us",
             u(w.index), u(w.wd.count), w.length_us / static_cast<double>(board::kWdtPeriodUs),
             cyc_us(u_min(w.wd.cyc_min)), cyc_us(avg(w.wd.cyc_sum, w.wd.count)), cyc_us(w.wd.cyc_max),
             avg(w.wd.late_sum_ticks, w.wd.count) * tick_us, w.wd.late_max_ticks * tick_us);
}

// Task layout (M7b acceptance 6). Core 1 may run only the flight task, IDLE1 and ipc1 (plus
// interrupt handlers, listed by esp_intr_dump). An unpinned task can run on Core 1: FAIL.
bool core1_allowed(const char* name) {
    return std::strcmp(name, "flight") == 0 || std::strncmp(name, "IDLE", 4) == 0 || std::strcmp(name, "ipc1") == 0;
}

#if configUSE_TRACE_FACILITY
TaskStatus_t s_tasks[48];

bool print_task_table() {
    const UBaseType_t n = uxTaskGetSystemState(s_tasks, sizeof(s_tasks) / sizeof(s_tasks[0]), nullptr);
    bool ok = n > 0;
    ESP_LOGI(TAG, "TASKS table: %u tasks | name core prio(base/current) stack-min-free(B) | verdict", u(n));
    for (UBaseType_t i = 0; i < n; ++i) {
        const TaskStatus_t& t = s_tasks[i];
        const BaseType_t core = xTaskGetCoreID(t.xHandle);
        char c[16];
        if (core == tskNO_AFFINITY) {
            std::snprintf(c, sizeof(c), "any");
        } else {
            std::snprintf(c, sizeof(c), "%d", static_cast<int>(core));
        }
        const bool may_run_on_1 = core == 1 || core == tskNO_AFFINITY;
        const bool good = !may_run_on_1 || (core == 1 && core1_allowed(t.pcTaskName));
        ok = ok && good;
        ESP_LOGI(TAG, "TASK %-16s core %-3s prio %2u/%2u stack-min-free %5u | %s", t.pcTaskName, c,
                 u(t.uxBasePriority), u(t.uxCurrentPriority), u(t.usStackHighWaterMark), good ? "ok" : "NOT ALLOWED ON CORE 1");
    }
    return ok;
}
#else
bool print_task_table() {
    // Without CONFIG_FREERTOS_USE_TRACE_FACILITY (the bench overlay sets it) only known names.
    static const char* kNames[] = {"IDLE0", "IDLE1", "ipc0", "ipc1", "esp_timer", "wifi", "tiT", "httpd", "sys_evt",
                                   "Tmr Svc", "udp_rx", "main", "m7b_bench", "m7b_keys", "flight"};
    bool ok = true;
    ESP_LOGW(TAG, "TASKS table: CONFIG_FREERTOS_USE_TRACE_FACILITY not set (flash the bench environment): known names only");
    for (const char* name : kNames) {
        const TaskHandle_t h = xTaskGetHandle(name);
        if (h == nullptr) {
            ESP_LOGI(TAG, "TASK %-16s missing", name);
            continue;
        }
        const BaseType_t core = xTaskGetCoreID(h);
        const bool may_run_on_1 = core == 1 || core == tskNO_AFFINITY;
        const bool good = !may_run_on_1 || (core == 1 && core1_allowed(name));
        ok = ok && good;
        ESP_LOGI(TAG, "TASK %-16s core %d prio %2u stack-min-free %5u | %s", name, static_cast<int>(core),
                 u(uxTaskPriorityGet(h)), u(uxTaskGetStackHighWaterMark(h)), good ? "ok" : "NOT ALLOWED ON CORE 1");
    }
    return ok;
}
#endif

hal_mcpwm::InitReport s_out_report;
ap::Report s_ap_report;
volatile bool s_summary_request = false;

void print_summary() {
    flight::Status s;
    flight::status(&s);
    ESP_LOGI(TAG, "SUMMARY (key s) ---------------------------------------------");
    print_status(s);
    ESP_LOGI(TAG, "SUMMARY outputs: GPIO %d/%d/%d/%d, %u Hz, %u ticks at %u Hz, cap %u permille, gates %s, duty %u"
                  " permille, hard latch %u",
             board::kMotorGpio[0], board::kMotorGpio[1], board::kMotorGpio[2], board::kMotorGpio[3], u(board::kPwmHz),
             u(s_out_report.period_ticks), u(s_out_report.resolution_hz), u(board::kBenchDutyCapPermille),
             hal_mcpwm::all_forced_low() ? "forced low" : "released", hal_mcpwm::duty_permille(),
             hal_mcpwm::hard_latched());
    for (int t = 0; t < sp::kTransports; ++t) {
        lk::Counters c;
        lk::counters(static_cast<sp::Transport>(t)).read(&c);
        ESP_LOGI(TAG, "SUMMARY %s: rx %u controls %u pings %u errors %u/%u/%u/%u | setpoint write (critical section %s)"
                      " avg/max %.2f/%.2f us, seqlock write alone avg/max %.2f/%.2f us, added avg %.2f us",
                 transport_name(static_cast<uint8_t>(t)), u(c.rx), u(c.controls), u(c.pings), u(c.err_length),
                 u(c.err_magic), u(c.err_crc), u(c.err_type), board::kSetpointCriticalSection ? "ON" : "OFF",
                 cyc_us(avg(c.wr_cyc_sum, c.controls)), cyc_us(c.wr_cyc_max), cyc_us(avg(c.wr_in_cyc_sum, c.controls)),
                 cyc_us(c.wr_in_cyc_max), cyc_us(avg(c.wr_cyc_sum - c.wr_in_cyc_sum, c.controls)));
    }
    ESP_LOGI(TAG, "SUMMARY Core 1 setpoint reads: giveups %u (last good used) torn %u retry_max %u | Wi-Fi beacon %u TU"
                  " DTIM %u | heap %u min %u | resets this power-on: brownouts %u, controlled restarts %u/%u | VBAT check:"
                  " NOT IMPLEMENTED (O4), hook passes",
             u(s.sp_giveups), u(s.sp_torn), u(s.sp_retries_max), s_ap_report.beacon_interval, s_ap_report.dtim_period,
             u(heap_free()), u(heap_min()), u(s_rtc.brownouts), u(s_rtc.restarts), u(board::kRestartLimit));
    (void)print_task_table();
}

// Fault keys that end in a reset store the RTC time first; the next boot reports hang -> reset.
void arm_hang_record(flight::Fault f) {
    s_rtc.hang_kind = static_cast<uint32_t>(f);
    s_rtc.hang_rtc_us = esp_rtc_get_time_us();
}

// Console keys, read from stdin (the primary console, UART0). The USB Serial/JTAG port is a
// secondary console and output only (verify against IDF docs), so keys work on UART0 only. No key
// can arm or drive an output, and none of this is reachable from the network.
void keys_task(void*) {
    bool int_on = true;
    for (;;) {
        const int ch = std::fgetc(stdin);
        if (ch == EOF) {
            std::clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        switch (ch) {
            case 'm':
                int_on = !int_on;
                flight::request_imu_int(int_on);
                if (int_on) {
                    ESP_LOGW(TAG, "KEY m: data-ready interrupt RESTORED");
                } else {
                    ESP_LOGW(TAG, "KEY m: data-ready interrupt DISABLED (missing data-ready injection, recovery"
                                  " suppressed); press m to restore");
                }
                break;
            case 's':
                s_summary_request = true;
                break;
            case 'h':
                ESP_LOGW(TAG, "KEY h: flight task stalls %u ms, interrupts on (heartbeat watchdog test)",
                         u(board::kFaultStallShortUs / 1000));
                flight::request_fault(flight::Fault::kStallShort);
                break;
            case 'H':
                ESP_LOGW(TAG, "KEY H: flight task stalls %u ms with interrupts MASKED on Core 1: expect an interrupt"
                              " watchdog panic (%d ms) and a reset",
                         u(board::kFaultStallMaskedUs / 1000), CONFIG_ESP_INT_WDT_TIMEOUT_MS);
                arm_hang_record(flight::Fault::kStallMasked);
                flight::request_fault(flight::Fault::kStallMasked);
                break;
            case 'T':
                ESP_LOGW(TAG, "KEY T: flight task stalls %u s, interrupts on: expect a heartbeat cut (if ARMED), then a"
                              " task watchdog panic (%d s) and a reset",
                         u(board::kFaultStallLongUs / 1000000), CONFIG_ESP_TASK_WDT_TIMEOUT_S);
                arm_hang_record(flight::Fault::kStallLong);
                flight::request_fault(flight::Fault::kStallLong);
                break;
            case 'X':
                ESP_LOGW(TAG, "KEY X: flight task calls abort(): expect the panic wrapper to force low, a backtrace"
                              " and a reset");
                arm_hang_record(flight::Fault::kAbort);
                flight::request_fault(flight::Fault::kAbort);
                break;
            case 'z':
                ESP_LOGW(TAG, "KEY z: IMU put to sleep (data-ready stops); expect a cut if ARMED, then recovery once"
                              " SAFE or DISARMED");
                flight::request_fault(flight::Fault::kImuSleep);
                break;
            case '\r':
            case '\n':
                break;
            default:
                ESP_LOGI(TAG, "keys: m = data-ready interrupt off/on, s = summary, h = stall 20 ms, H = stall 1 s with"
                              " interrupts masked (reset), T = stall 10 s (reset), X = abort (reset), z = IMU sleep."
                              " Nothing here can arm or drive the outputs.");
                break;
        }
    }
}

// Reset reason, brownout and restart counters (RTC memory), and the hang report of a fault key.
void boot_report() {
    const esp_reset_reason_t why = esp_reset_reason();
    if (why == ESP_RST_POWERON || s_rtc.magic != kRtcMagic) {
        std::memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = kRtcMagic;
    }
    if (why == ESP_RST_BROWNOUT) s_rtc.brownouts++;
    const bool controlled = s_rtc.restart_pending != 0 && why == ESP_RST_SW;
    s_rtc.restart_pending = 0;
    ESP_LOGW(TAG, "RESET: reason %s%s | brownout resets this power-on %u | controlled restarts this power-on %u/%u",
             reset_name(why), controlled ? " (controlled restart after failed IMU recovery)" : "", u(s_rtc.brownouts),
             u(s_rtc.restarts), u(board::kRestartLimit));
    if (s_rtc.hang_kind != 0) {
        // esp_timer counts from the chip reset (verify against IDF docs), so the reset instant on the
        // RTC clock is now_rtc - esp_timer. The gates were forced low by the panic wrapper at the panic
        // and are held low by the 10 kOhm pull-downs from the reset on.
        const uint64_t now_rtc = esp_rtc_get_time_us();
        const int64_t since_reset = esp_timer_get_time();
        const double hang_to_reset_ms =
            (static_cast<double>(now_rtc) - static_cast<double>(since_reset) - static_cast<double>(s_rtc.hang_rtc_us)) / 1000.0;
        ESP_LOGW(TAG, "HANG: key %s | hang -> reset %.1f ms (RTC clock) | reset reason %s | gates: panic wrapper force-low"
                      " at the panic, pull-downs from the reset | rtc now %llu us, since reset %lld us, at key %llu us",
                 fault_key(s_rtc.hang_kind), hang_to_reset_ms, reset_name(why), static_cast<unsigned long long>(now_rtc),
                 static_cast<long long>(since_reset), static_cast<unsigned long long>(s_rtc.hang_rtc_us));
        s_rtc.hang_kind = 0;
    }
}

void config_report() {
    ESP_LOGI(TAG, "CONFIG: interrupt watchdog %s %d ms (CPU1 tick %s) | task watchdog %d s, panic %s, idle CPU0 %s CPU1 %s"
                  " | panic: %s | trace facility %s | setpoint critical section %s | IMU boot retry %s",
#if CONFIG_ESP_INT_WDT
             "on", CONFIG_ESP_INT_WDT_TIMEOUT_MS,
#else
             "OFF", 0,
#endif
#if CONFIG_ESP_INT_WDT_CHECK_CPU1
             "watched",
#else
             "NOT watched",
#endif
             CONFIG_ESP_TASK_WDT_TIMEOUT_S,
#if CONFIG_ESP_TASK_WDT_PANIC
             "on",
#else
             "OFF",
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
             "watched",
#else
             "no",
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
             "watched",
#else
             "no",
#endif
#if CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT
             "print and reboot",
#elif CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT
             "silent reboot",
#else
             "other",
#endif
#if configUSE_TRACE_FACILITY
             "on (bench overlay)",
#else
             "off",
#endif
             board::kSetpointCriticalSection ? "ON" : "OFF", board::kImuBootRetry ? "ON" : "OFF (instrument only)");
}

void bench_task(void*) {
    for (int i = 0; i < 3; ++i) {
        ESP_LOGW(TAG, "################ %s ################", board::kBenchBanner);
    }
    ESP_LOGW(TAG, "GPIO %d to %d: 10 kOhm pull-downs and a multimeter only. Bench duty cap %u permille (at most %.2f V"
                  " average at full throttle). Motors under load remain gated by the VBAT cutoff and brownout tuning"
                  " (blueprint 6.1 O4, Phase 3 step 3). VBAT check NOT IMPLEMENTED: its hook always passes.",
             board::kMotorGpio[0], board::kMotorGpio[3], u(board::kBenchDutyCapPermille),
             3.3 * board::kBenchDutyCapPermille / 1000.0);
    boot_report();
    config_report();

    s_out_report = hal_mcpwm::init();
    const hal_mcpwm::InitReport& o = s_out_report;
    const bool out_ok = o.err == ESP_OK && o.ids_verified && o.forced_before_start && hal_mcpwm::all_forced_low();
    ESP_LOGI(TAG, "OUTPUTS: %s (%s) | GPIO %d/%d/%d/%d, %u Hz, period %u ticks at %u Hz | operator/generator IDs"
                  " %d.%d %d.%d %d.%d %d.%d %s | all forced low before the timer started: %s | now: %s | panic force-low"
                  " %s (gen_force 0x%08X 0x%08X)",
             out_ok ? "PASS" : "FAIL", esp_err_to_name(o.err), board::kMotorGpio[0], board::kMotorGpio[1],
             board::kMotorGpio[2], board::kMotorGpio[3], u(board::kPwmHz), u(o.period_ticks), u(o.resolution_hz),
             o.oper_id[0], o.gen_id[0], o.oper_id[1], o.gen_id[1], o.oper_id[2], o.gen_id[2], o.oper_id[3], o.gen_id[3],
             o.ids_verified ? "verified" : "NOT VERIFIED", o.forced_before_start ? "yes" : "NO",
             hal_mcpwm::all_forced_low() ? "forced low" : "NOT FORCED", o.panic_force_ready ? "ready" : "NOT READY",
             u(o.panic_force_val[0]), u(o.panic_force_val[1]));

    const esp_err_t fs_err = hal_fs::mount();
    ESP_LOGI(TAG, "FS: mount %s at %s (read-only)", esp_err_to_name(fs_err), board::kFsBasePath);

    const esp_err_t ap_err = ap::start();
    s_ap_report = ap::report();
    const ap::Report& r = s_ap_report;
    const bool ap_ok = ap_err == ESP_OK && r.started && r.channel == board::kWifiChannel &&
                       r.authmode == WIFI_AUTH_WPA2_PSK && r.max_connection == board::kWifiMaxStations &&
                       r.ps == WIFI_PS_NONE && r.bandwidth == WIFI_BW_HT20;
    ESP_LOGI(TAG, "AP: %s (%s) | channel %u, WPA2 %s, max %u, power save %s, %s", ap_ok ? "PASS" : "FAIL",
             esp_err_to_name(ap_err), r.channel, r.authmode == WIFI_AUTH_WPA2_PSK ? "yes" : "NO", r.max_connection,
             r.ps == WIFI_PS_NONE ? "none" : "ON", r.bandwidth == WIFI_BW_HT20 ? "HT20" : "HT40");
    const bool beacon_ok = r.beacon_interval == board::kWifiBeaconIntervalTu && r.dtim_period == board::kWifiDtimPeriod;
    ESP_LOGI(TAG, "WIFI (B-M6-08): beacon_interval %u TU, dtim_period %u, read back with esp_wifi_get_config | %s",
             r.beacon_interval, r.dtim_period, beacon_ok ? "PASS" : "FAIL");
    if (r.placeholder_credentials) {
        ESP_LOGW(TAG, "the Wi-Fi credentials header still holds the template values: set your own in"
                      " components/comms/include/wifi_credentials.h");
    }
    const esp_err_t http_err = comms::http_static::start(comms::ws_server::uri());
    const esp_err_t udp_err = comms::udp_server::start();
    ESP_LOGI(TAG, "LINK: HTTP+WS %s, UDP port %u %s", esp_err_to_name(http_err), u(board::kUdpPort),
             esp_err_to_name(udp_err));

    const flight::StartReport fr = flight::start(out_ok);
    ESP_LOGI(TAG, "FLIGHT: %s | bus A %s (lines %s, %u clocks) | IMU init %s after %u attempt(s) WHO_AM_I 0x%02X |"
                  " data-ready interrupt %s | task stack free %u B | outputs %s",
             fr.ok ? "PASS" : "FAIL", esp_err_to_name(fr.bus_err), hal_i2c::line_action_name(fr.bus_lines.action),
             u(fr.bus_lines.clocks), fr.init.step == drv_mpu6500::Step::kNone ? "OK" : "FAILED", u(fr.attempts),
             fr.init.who_am_i, esp_err_to_name(fr.int_err), u(fr.stack_free),
             out_ok ? "ready" : "NOT READY: the machine stays in BOOT, gates forced low");
    for (uint32_t a = 0; a < fr.attempts && a < 3; ++a) {
        const drv_mpu6500::InitStatus& st = fr.attempt[a];
        if (st.step == drv_mpu6500::Step::kNone) {
            ESP_LOGI(TAG, "FLIGHT: IMU init attempt %u OK%s", u(a + 1), a > 0 ? " (after retry)" : "");
        } else {
            ESP_LOGE(TAG, "FLIGHT: IMU init attempt %u failed at %s | error %s (0x%x) | register 0x%02X expected 0x%02X"
                          " read 0x%02X | before it: bus_reset %s, DEVICE_RESET %s",
                     u(a + 1), imu_step_name(st.step), esp_err_to_name(st.err), static_cast<unsigned>(st.err), st.reg,
                     st.expected, st.actual, a > 0 ? esp_err_to_name(fr.retry_bus_reset_err[a]) : "-",
                     a > 0 ? esp_err_to_name(fr.retry_soft_reset_err[a]) : "-");
        }
    }
    const wdt::StartReport& wr = fr.wdt;
    ESP_LOGI(TAG, "WDT: %s (%s) | GPTimer alarm every %u us (%u ticks at %u Hz), interrupt level %d on core %d |"
                  " enforced only while ARMED: heartbeat > %u us, data-ready missing > %u us | backup tick check > %u us"
                  " | watchdog dead after %u us without an alarm",
             wr.err == ESP_OK && wr.core == 1 && fr.ok ? "PASS" : "FAIL", esp_err_to_name(wr.err), u(wr.period_us),
             u(wr.alarm_ticks), u(wr.resolution_hz), wr.intr_level, wr.core, u(board::kWdtHeartbeatStallUs),
             u(board::kImuMissingCutUs), u(board::kImuMissingTickCutUs), u(board::kWdtAliveUs));

    xTaskCreatePinnedToCore(keys_task, "m7b_keys", 3072, nullptr, 2, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    const bool tasks_ok = print_task_table();
    ESP_LOGI(TAG, "TASKS: %s | Core 1 may run only flight, IDLE1 and ipc1 (and interrupt handlers, below)",
             tasks_ok ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "INTERRUPTS (esp_intr_dump) ----------------------------------");
    esp_intr_dump(stdout);
    ESP_LOGI(TAG, "keys (UART0 console): m = data-ready off/on, s = summary, h = stall 20 ms, H = stall 1 s masked,"
                  " T = stall 10 s, X = abort, z = IMU sleep. Client: .venv/bin/python tools/control_client.py --transport"
                  " udp|ws");

    static flight::EventLog log;
    static flight::Window win;
    static flight::RecoveryLog rec;
    uint32_t printed = 0;
    uint32_t rec_printed = 0;
    uint32_t win_seen = 0;
    bool restart_handled = false;
    int64_t next_status = esp_timer_get_time();
    for (;;) {
        if (flight::events(&log) && log.count != printed) {
            if (log.count - printed > static_cast<uint32_t>(flight::kEventLog)) {
                ESP_LOGE(TAG, "EV %u events not printed (log overrun)",
                         u(log.count - printed - static_cast<uint32_t>(flight::kEventLog)));
                printed = log.count - flight::kEventLog;
            }
            while (printed != log.count) {
                ++printed;
                print_event(log.rec[(printed - 1) % flight::kEventLog]);
            }
        }
        if (flight::recovery_log(&rec) && rec.count != rec_printed) {
            if (rec.count - rec_printed > static_cast<uint32_t>(flight::kRecoveryLog)) {
                rec_printed = rec.count - flight::kRecoveryLog;
            }
            while (rec_printed != rec.count) {
                ++rec_printed;
                print_recovery(rec.rec[(rec_printed - 1) % flight::kRecoveryLog]);
            }
        }
        uint32_t seq = 0;
        if (flight::window(&win, &seq) && seq != win_seen) {
            win_seen = seq;
            print_window(win);
        }
        const int64_t now = esp_timer_get_time();
        flight::Status s;
        const bool have_status = flight::status(&s);
        if (now >= next_status) {
            next_status += kStatusMs * 1000;
            if (have_status) print_status(s);
        }
        // D5: after board::kRecoveryAttempts failed recoveries, a controlled restart (at most
        // board::kRestartLimit per power-on, RTC memory), never while ARMED; then BOOT, gates low.
        if (have_status && s.rec_exhausted && !restart_handled && s.state != safety::State::kArmed) {
            restart_handled = true;
            if (s_rtc.restarts < board::kRestartLimit) {
                s_rtc.restarts++;
                s_rtc.restart_pending = 1;
                ESP_LOGE(TAG, "RESTART: IMU recovery failed %u times; controlled restart %u/%u (state %s, gates %s)",
                         u(board::kRecoveryAttempts), u(s_rtc.restarts), u(board::kRestartLimit), safety::name(s.state),
                         hal_mcpwm::all_forced_low() ? "forced low" : "NOT FORCED");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "RESTART: limit reached (%u/%u this power-on): staying in BOOT with the gates low",
                         u(s_rtc.restarts), u(board::kRestartLimit));
                flight::request_boot_state();
            }
        }
        if (s_summary_request) {
            s_summary_request = false;
            print_summary();
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

void m7b_start() { xTaskCreatePinnedToCore(bench_task, "m7b_bench", 8192, nullptr, 3, nullptr, 0); }

#else

void m7b_start() {}

#endif  // FE_M7B_TEST
