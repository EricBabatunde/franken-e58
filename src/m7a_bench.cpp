#include "m7a_bench.h"

#include "board_config.h"

#if FE_M7A_TEST

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
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
#include "udp_server.h"
#include "wifi_ap.h"
#include "ws_server.h"

#if !FE_IMU_ISR_INSTRUMENT
#error "The M7a bench test needs FE_IMU_ISR_INSTRUMENT"
#endif

namespace {

const char* TAG = "m7a";
namespace ap = comms::wifi_ap;
namespace lk = comms::link;
namespace sp = comms::setpoint;

constexpr uint32_t kPollMs = 50;
constexpr uint32_t kStatusMs = 1000;
constexpr double kCpuMhz = 240.0;  // CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }

// Where drv_mpu6500::init() stopped (same names as the M3 bench).
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
    char mask[160];
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
            if (e.cut == safety::Cut::kLinkSilence) {
                ESP_LOGE(TAG, "EV #%u t=%lld us CUT link_silence | decision age %.3f ms (last frame rx t=%lld us)"
                              " | force-low written %.2f us after the decision (t=%lld us) | ARMED -> DISARMED, latched",
                         u(e.seq), e.t_us, (e.t_us - e.sp_rx_us) / 1000.0, e.sp_rx_us, force_us, r.t_force_us);
            } else if (e.cut == safety::Cut::kKill) {
                ESP_LOGE(TAG, "EV #%u t=%lld us CUT kill | frame rx -> decision %lld us, frame rx -> force-low %lld us"
                              " (rx t=%lld us) | force-low %.2f us after the decision | ARMED -> DISARMED, latched",
                         u(e.seq), e.t_us, e.t_us - e.sp_rx_us, r.t_force_us - e.sp_rx_us, e.sp_rx_us, force_us);
            } else {
                ESP_LOGE(TAG, "EV #%u t=%lld us CUT imu_missing | last data-ready -> decision %lld us, -> force-low"
                              " %lld us (last pulse t=%lld us) | force-low %.2f us after the decision | ARMED -> "
                              "DISARMED, latched",
                         u(e.seq), e.t_us, e.t_us - e.imu_last_us, r.t_force_us - e.imu_last_us, e.imu_last_us,
                         force_us);
            }
            break;
        }
    }
}

void print_status(const flight::Status& s) {
    char block[160];
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
                  " | overruns %u sp giveups %u torn %u | flash writes %s",
             safety::name(s.state), s.latched ? " (latched)" : "", safety::name(s.last_cut), block, spbuf,
             s.duty_permille, s.forced_low ? "forced low" : "released", s.imu_healthy ? "healthy" : "UNHEALTHY",
             s.since_pulse_us / 1000.0, s.int_enabled ? "on" : "OFF", cal, s.at_rest ? "yes" : "no", tilt_deg(s.accel),
             s.accel_ok ? "ok" : "BAD", u(s.arms), u(s.cuts), u(s.refusals), u(s.disarms), u(s.overruns),
             u(s.sp_giveups), u(s.sp_torn), s.flash_ok ? "allowed" : "REFUSED");
}

lk::Counters s_prev[sp::kTransports];

void print_window(const flight::Window& w) {
    const double secs = w.length_us / 1e6;
    const double rate = w.length_us ? w.raw_pulses * 1e6 / w.length_us : 0.0;
    const double scale = drv_mpu6500::gyro_dps(1);  // deg/s per LSB, as the M3 line
    double gm[3], gsd[3];
    for (int i = 0; i < 3; ++i) {
        const double m = w.samples ? static_cast<double>(w.gyro_sum[i]) / w.samples : 0.0;
        const double v = w.samples ? static_cast<double>(w.gyro_sumsq[i]) / w.samples - m * m : 0.0;
        gm[i] = m * scale;
        gsd[i] = (v > 0 ? std::sqrt(v) : 0.0) * scale;
    }
    const double amag = w.samples ? std::sqrt(w.accel_sq_sum / w.samples) : 0.0;
    // Same layout as the M3 "B wNN" line, so the M3 to M6 figures compare directly.
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
}

bool tasks_on_expected_cores(char* buf, size_t len) {
    struct Want {
        const char* name;
        BaseType_t core;
    };
    static const Want kWant[] = {{"wifi", 0},      {"tiT", 0},       {"httpd", 0},    {"udp_rx", 0},
                                 {"sys_evt", 0},   {"esp_timer", 0}, {"m7a_bench", 0}, {"m7a_keys", 0},
                                 {"flight", 1}};
    bool ok = true;
    size_t n = 0;
    buf[0] = '\0';
    for (const Want& w : kWant) {
        const TaskHandle_t h = xTaskGetHandle(w.name);
        const BaseType_t core = h ? xTaskGetCoreID(h) : -1;
        const bool good = core == w.core;
        ok = ok && good;
        char c[12];
        if (h == nullptr) {
            std::snprintf(c, sizeof(c), "missing");
        } else if (core == tskNO_AFFINITY) {
            std::snprintf(c, sizeof(c), "any");
        } else {
            std::snprintf(c, sizeof(c), "%d", static_cast<int>(core));
        }
        if (n < len) n += std::snprintf(buf + n, len - n, "%s%s %s%s", n ? ", " : "", w.name, c, good ? "" : " (!)");
    }
    return ok;
}

hal_mcpwm::InitReport s_out_report;
ap::Report s_ap_report;
volatile bool s_summary_request = false;

void print_summary() {
    flight::Status s;
    flight::status(&s);
    ESP_LOGI(TAG, "SUMMARY (key s) ---------------------------------------------");
    print_status(s);
    ESP_LOGI(TAG, "SUMMARY outputs: GPIO %d/%d/%d/%d, %u Hz, %u ticks at %u Hz, cap %u permille, gates %s, duty %u"
                  " permille",
             board::kMotorGpio[0], board::kMotorGpio[1], board::kMotorGpio[2], board::kMotorGpio[3], u(board::kPwmHz),
             u(s_out_report.period_ticks), u(s_out_report.resolution_hz), u(board::kBenchDutyCapPermille),
             hal_mcpwm::all_forced_low() ? "forced low" : "released", hal_mcpwm::duty_permille());
    for (int t = 0; t < sp::kTransports; ++t) {
        lk::Counters c;
        lk::counters(static_cast<sp::Transport>(t)).read(&c);
        ESP_LOGI(TAG, "SUMMARY %s: rx %u controls %u pings %u errors %u/%u/%u/%u | setpoint write (B-M6-10) avg/max"
                      " %.2f/%.2f us, seqlock write alone avg/max %.2f/%.2f us, added avg %.2f us",
                 transport_name(static_cast<uint8_t>(t)), u(c.rx), u(c.controls), u(c.pings), u(c.err_length),
                 u(c.err_magic), u(c.err_crc), u(c.err_type), cyc_us(avg(c.wr_cyc_sum, c.controls)),
                 cyc_us(c.wr_cyc_max), cyc_us(avg(c.wr_in_cyc_sum, c.controls)), cyc_us(c.wr_in_cyc_max),
                 cyc_us(avg(c.wr_cyc_sum - c.wr_in_cyc_sum, c.controls)));
    }
    ESP_LOGI(TAG, "SUMMARY Core 1 setpoint reads: giveups %u (last good used) torn %u retry_max %u | Wi-Fi beacon %u TU"
                  " DTIM %u | heap %u min %u | VBAT check: NOT IMPLEMENTED (O4), hook passes",
             u(s.sp_giveups), u(s.sp_torn), u(s.sp_retries_max), s_ap_report.beacon_interval, s_ap_report.dtim_period,
             u(heap_free()), u(heap_min()));
}

// Console keys, read from stdin (the primary console, UART0). The USB Serial/JTAG port is a
// secondary console and output only (verify against IDF docs), so keys work on UART0 only.
void keys_task(void*) {
    bool int_on = true;
    for (;;) {
        const int ch = std::fgetc(stdin);
        if (ch == EOF) {
            std::clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == 'm') {
            int_on = !int_on;
            flight::request_imu_int(int_on);
            if (int_on) {
                ESP_LOGW(TAG, "KEY m: data-ready interrupt RESTORED");
            } else {
                ESP_LOGW(TAG, "KEY m: data-ready interrupt DISABLED (missing data-ready injection); press m to restore");
            }
        } else if (ch == 's') {
            s_summary_request = true;
        } else if (ch != '\r' && ch != '\n') {
            ESP_LOGI(TAG, "keys: m = toggle data-ready interrupt (missing-IMU injection), s = summary. Nothing here"
                          " can arm or drive the outputs.");
        }
    }
}

void bench_task(void*) {
    for (int i = 0; i < 3; ++i) {
        ESP_LOGW(TAG, "################ %s ################", board::kBenchBanner);
    }
    ESP_LOGW(TAG, "GPIO %d to %d are measured with a multimeter only. Bench duty cap %u permille (at most %.2f V"
                  " average at full throttle). Motors under load remain gated by the VBAT cutoff and brownout tuning"
                  " (blueprint 6.1 O4, Phase 3 step 3). VBAT check NOT IMPLEMENTED: its hook always passes.",
             board::kMotorGpio[0], board::kMotorGpio[3], u(board::kBenchDutyCapPermille),
             3.3 * board::kBenchDutyCapPermille / 1000.0);

    s_out_report = hal_mcpwm::init();
    const hal_mcpwm::InitReport& o = s_out_report;
    const bool out_ok = o.err == ESP_OK && o.ids_verified && o.forced_before_start && hal_mcpwm::all_forced_low();
    ESP_LOGI(TAG, "OUTPUTS: %s (%s) | GPIO %d/%d/%d/%d, %u Hz, period %u ticks at %u Hz | operator/generator IDs"
                  " %d.%d %d.%d %d.%d %d.%d %s | all forced low before the timer started: %s | now: %s",
             out_ok ? "PASS" : "FAIL", esp_err_to_name(o.err), board::kMotorGpio[0], board::kMotorGpio[1],
             board::kMotorGpio[2], board::kMotorGpio[3], u(board::kPwmHz), u(o.period_ticks), u(o.resolution_hz),
             o.oper_id[0], o.gen_id[0], o.oper_id[1], o.gen_id[1], o.oper_id[2], o.gen_id[2], o.oper_id[3], o.gen_id[3],
             o.ids_verified ? "verified" : "NOT VERIFIED", o.forced_before_start ? "yes" : "NO",
             hal_mcpwm::all_forced_low() ? "forced low" : "NOT FORCED");

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
    ESP_LOGI(TAG, "FLIGHT: %s | bus A %s (lines %s) | IMU init %s WHO_AM_I 0x%02X | data-ready interrupt %s"
                  " | task stack free %u B | outputs %s",
             fr.ok ? "PASS" : "FAIL", esp_err_to_name(fr.bus_err), hal_i2c::line_action_name(fr.bus_lines.action),
             fr.init.step == drv_mpu6500::Step::kNone ? "OK" : "FAILED", fr.init.who_am_i, esp_err_to_name(fr.int_err),
             u(fr.stack_free), out_ok ? "ready" : "NOT READY: the machine stays in BOOT, gates forced low");
    if (fr.init.step != drv_mpu6500::Step::kNone) {
        // The error behind an IMU init failure (an ESP_OK error with a wrong value means the read
        // succeeded but returned something else). The machine stays in BOOT, gates forced low.
        ESP_LOGE(TAG, "FLIGHT: IMU init failed at %s | error %s (0x%x) | register 0x%02X expected 0x%02X read 0x%02X",
                 imu_step_name(fr.init.step), esp_err_to_name(fr.init.err), static_cast<unsigned>(fr.init.err),
                 fr.init.reg, fr.init.expected, fr.init.actual);
    }

    xTaskCreatePinnedToCore(keys_task, "m7a_keys", 3072, nullptr, 2, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    char cores[300];
    const bool cores_ok = tasks_on_expected_cores(cores, sizeof(cores));
    ESP_LOGI(TAG, "TASKS: %s | cores: %s", cores_ok ? "PASS" : "FAIL", cores);
    ESP_LOGI(TAG, "keys (UART0 console): m = toggle data-ready interrupt (missing-IMU injection), s = summary."
                  " Client: .venv/bin/python tools/control_client.py --transport udp|ws");

    static flight::EventLog log;
    static flight::Window win;
    uint32_t printed = 0;
    uint32_t win_seen = 0;
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
        uint32_t seq = 0;
        if (flight::window(&win, &seq) && seq != win_seen) {
            win_seen = seq;
            print_window(win);
        }
        const int64_t now = esp_timer_get_time();
        if (now >= next_status) {
            next_status += kStatusMs * 1000;
            flight::Status s;
            if (flight::status(&s)) print_status(s);
        }
        if (s_summary_request) {
            s_summary_request = false;
            print_summary();
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

void m7a_start() { xTaskCreatePinnedToCore(bench_task, "m7a_bench", 8192, nullptr, 3, nullptr, 0); }

#else

void m7a_start() {}

#endif  // FE_M7A_TEST
