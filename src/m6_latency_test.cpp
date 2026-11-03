#include "m6_latency_test.h"

#include "board_config.h"

#if FE_M6_TEST

#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "control_frame.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_fs.h"
#include "http_static.h"
#include "link.h"
#include "m3_imu_test.h"
#include "seqlock.h"
#include "setpoint.h"
#include "udp_server.h"
#include "wifi_ap.h"
#include "ws_server.h"

#if !FE_IMU_ISR_INSTRUMENT
#error "The M6 bench test needs FE_IMU_ISR_INSTRUMENT"
#endif

extern const char kVectorsStart[] asm("_binary_control_frame_vectors_txt_start");

namespace {

const char* TAG = "m6";
namespace ap = comms::wifi_ap;
namespace http = comms::http_static;
namespace sp = comms::setpoint;
namespace lk = comms::link;

constexpr uint32_t kPollMs = 200;
constexpr uint32_t kQuietSeconds = 10;          // no frame for this long: the clients have stopped
constexpr uint32_t kQuietTimeoutSeconds = 180;
constexpr uint32_t kIdlePrintSeconds = 10;      // comms line after the IMU run
constexpr uint32_t kLeakMaxBytes = 2048;        // acceptance 8
constexpr uint32_t kMinFramesPerTransport = 10000;
constexpr int64_t kLiveAgeUs = 1000 * 1000;     // ages above this are a pause, not a live link
constexpr double kCpuMhz = 240.0;               // CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240
// A 10 s window counts as a traffic window for one transport when it carried at least 80 %
// of the 500 pings expected at 50 Hz and the other transport was silent.
constexpr uint32_t kTrafficPings = 400;
// M3 and M5 baselines and the acceptance 6 limits (IMU).
constexpr double kM3IvSdUs = 0.44, kM3JitterUs = 3.7, kM3BurstAvgUs = 450.4;
constexpr uint32_t kM3WakeMaxUs = 12;
constexpr double kIvSdMaxUs = 2.0, kJitterMaxUs = 20.0;
constexpr uint32_t kWakeMaxLimitUs = 50;

// ---- Core 1: setpoint reader, called by the M3 Phase B loop after every read.
// No logging, no allocation, no lock shared with Core 0. Results go to Core 0 through a
// seqlock: on every window change, and every 1000 samples for the window in progress.

struct HookWindow {
    uint32_t window = 0;          // the "B wNN" number
    bool complete = false;        // false: the window is still in progress
    uint32_t calls = 0;
    uint32_t read_fail = 0;       // seqlock read gave up after its retries (slot skipped)
    uint32_t retries = 0;         // extra attempts summed over reads
    uint32_t retries_max = 0;     // most extra attempts in one read
    uint32_t torn = 0;            // records whose check word did not match (must stay 0)
    uint32_t with_setpoint = 0;   // calls where at least one slot held a setpoint
    uint32_t stale = 0;           // newest setpoint older than kSetpointStaleUs
    uint32_t age_max_us = 0;      // live ages only (below kLiveAgeUs)
    uint64_t age_sum_us = 0;
    uint32_t age_n = 0;
    uint32_t cyc_min = UINT32_MAX;
    uint32_t cyc_max = 0;
    uint64_t cyc_sum = 0;
};

SeqlockSlot<HookWindow> s_hook_slot;
HookWindow s_hw;  // Core 1 only

void sample_hook(uint32_t window) {
    const uint32_t c0 = esp_cpu_get_cycle_count();
    HookWindow& w = s_hw;
    if (window != w.window) {
        if (w.window != 0) {
            w.complete = true;
            s_hook_slot.write(w);
        }
        w = HookWindow{};
        w.window = window;
    }
    w.calls++;
    const int64_t now = esp_timer_get_time();
    int64_t newest = INT64_MIN;
    for (int t = 0; t < sp::kTransports; ++t) {
        const SeqlockSlot<sp::Setpoint>& slot = sp::slot(static_cast<sp::Transport>(t));
        if (slot.writes() == 0) {
            continue;
        }
        sp::Setpoint s;
        int tries = 0;
        const bool ok = slot.read(&s, 8, &tries);
        const uint32_t extra = static_cast<uint32_t>(tries - 1);
        w.retries += extra;
        if (extra > w.retries_max) w.retries_max = extra;
        if (!ok) {
            w.read_fail++;
            continue;
        }
        if (s.check != sp::check_word(s)) {
            w.torn++;
            continue;
        }
        if (s.t_rx_us > newest) newest = s.t_rx_us;
    }
    if (newest != INT64_MIN) {
        w.with_setpoint++;
        const int64_t age = now - newest;
        if (age > board::kSetpointStaleUs) w.stale++;
        if (age >= 0 && age < kLiveAgeUs) {
            const uint32_t a = static_cast<uint32_t>(age);
            if (a > w.age_max_us) w.age_max_us = a;
            w.age_sum_us += a;
            w.age_n++;
        }
    }
    const uint32_t cyc = esp_cpu_get_cycle_count() - c0;  // the publish below is not included
    if (cyc < w.cyc_min) w.cyc_min = cyc;
    if (cyc > w.cyc_max) w.cyc_max = cyc;
    w.cyc_sum += cyc;
    if (w.calls % 1000 == 0) {
        w.complete = false;
        s_hook_slot.write(w);
    }
}

// ---- Core 0.

enum Slot { kCrc, kFs, kAp, kHttp, kUdp, kTasks, kImu, kSetpoint, kCommsUdp, kCommsWs, kHeap, kSlots };
constexpr size_t kSummaryLen = 768;
char s_summary[kSlots][kSummaryLen];
uint32_t s_station_seq = 0;

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }
int64_t now_us() { return esp_timer_get_time(); }
uint32_t heap_free() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }
uint32_t heap_min() { return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)); }
double cyc_us(double c) { return c / kCpuMhz; }

__attribute__((format(printf, 2, 3))) const char* summary(int n, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_summary[n], kSummaryLen, fmt, args);
    va_end(args);
    return s_summary[n];
}

void recap() {
    ESP_LOGI(TAG, "===== M6 SUMMARY RECAP =====");
    for (int n = 0; n < kSlots; ++n) {
        if (s_summary[n][0] != '\0') {
            ESP_LOGI(TAG, "%s", s_summary[n]);
        }
    }
    ESP_LOGI(TAG, "===== end of recap =====");
}

void poll_stations() {
    ap::StationLog l;
    if (!ap::station_log(&l) || l.seq == s_station_seq) {
        return;
    }
    s_station_seq = l.seq;
    ESP_LOGI(TAG, "station %s %02x:%02x:%02x:%02x:%02x:%02x (reason %u) | joins %u leaves %u | associated now %d",
             l.last_join ? "joined" : "left", l.last_mac[0], l.last_mac[1], l.last_mac[2], l.last_mac[3],
             l.last_mac[4], l.last_mac[5], l.last_join ? 0u : l.last_reason, u(l.joins), u(l.leaves),
             ap::stations());
}

lk::Counters counters(sp::Transport t) {
    lk::Counters c;
    if (!lk::counters(t).read(&c)) {
        c = lk::Counters{};
    }
    return c;
}

int station_rssi() {
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num == 0) {
        return 0;
    }
    return list.sta[0].rssi;
}

bool tasks_on_expected_cores(char* buf, size_t len) {
    struct Want {
        const char* name;
        BaseType_t core;
    };
    static const Want kWant[] = {{"wifi", 0},      {"tiT", 0},     {"httpd", 0},  {"udp_rx", 0}, {"sys_evt", 0},
                                 {"esp_timer", 0}, {"m6_test", 0}, {"m3_report", 0}, {"imu", 1}};
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
        if (n < len) {
            n += std::snprintf(buf + n, len - n, "%s%s %s%s", n ? ", " : "", w.name, c, good ? "" : " (!)");
        }
    }
    return ok;
}

// Window bookkeeping on Core 0.
struct Totals {
    lk::Counters last[sp::kTransports];  // counters at the previous window line
    // Setpoint figures over the windows with control traffic (acceptance 7).
    uint32_t sp_windows = 0;
    uint32_t sp_retries_max = 0;
    uint32_t sp_read_fail = 0;
    uint32_t sp_age_max_us = 0;
    uint32_t sp_stale = 0;
    // Over every window.
    uint32_t torn = 0;
    uint32_t calls = 0;
    uint32_t cyc_min = UINT32_MAX;
    uint32_t cyc_max = 0;
    uint64_t cyc_sum = 0;
    uint32_t windows[4] = {0, 0, 0, 0};  // idle, udp, ws, mixed
    // Heap (acceptance 8): the last idle window with a station before the first traffic.
    bool traffic_seen = false;
    bool heap_idle_valid = false;
    uint32_t heap_idle = 0;
};
Totals s_tot;
uint32_t s_last_window = 0;
const char* const kClassName[4] = {"idle", "udp", "ws", "mixed"};

// Prints one line per completed Core 1 window, with the comms traffic since the previous line.
void window_line(const HookWindow& w) {
    lk::Counters c[sp::kTransports];
    lk::Counters d[sp::kTransports];
    for (int t = 0; t < sp::kTransports; ++t) {
        c[t] = counters(static_cast<sp::Transport>(t));
        const lk::Counters& p = s_tot.last[t];
        d[t].rx = c[t].rx - p.rx;
        d[t].tx = c[t].tx - p.tx;
        d[t].pings = c[t].pings - p.pings;
        d[t].controls = c[t].controls - p.controls;
        d[t].err_length = c[t].err_length - p.err_length;
        d[t].err_magic = c[t].err_magic - p.err_magic;
        d[t].err_crc = c[t].err_crc - p.err_crc;
        d[t].err_type = c[t].err_type - p.err_type;
        d[t].ta_n = c[t].ta_n - p.ta_n;
        d[t].ta_sum = c[t].ta_sum - p.ta_sum;
        s_tot.last[t] = c[t];
    }
    const lk::Counters& du = d[sp::kUdp];
    const lk::Counters& dw = d[sp::kWs];
    int cls = 3;
    if (du.rx == 0 && dw.rx == 0) {
        cls = 0;
    } else if (du.pings >= kTrafficPings && dw.rx == 0) {
        cls = 1;
    } else if (dw.pings >= kTrafficPings && du.rx == 0) {
        cls = 2;
    }
    s_tot.windows[cls]++;
    const uint32_t heap = heap_free();
    if (cls != 0) {
        s_tot.traffic_seen = true;
    } else if (!s_tot.traffic_seen && ap::stations() > 0) {
        s_tot.heap_idle = heap;
        s_tot.heap_idle_valid = true;
    }
    if ((cls == 1 || cls == 2) && (du.controls + dw.controls) >= kTrafficPings) {
        s_tot.sp_windows++;
        if (w.retries_max > s_tot.sp_retries_max) s_tot.sp_retries_max = w.retries_max;
        if (w.age_max_us > s_tot.sp_age_max_us) s_tot.sp_age_max_us = w.age_max_us;
        s_tot.sp_stale += w.stale;
        s_tot.sp_read_fail += w.read_fail;
    }
    s_tot.torn += w.torn;
    s_tot.calls += w.calls;
    if (w.calls > 0) {
        if (w.cyc_min < s_tot.cyc_min) s_tot.cyc_min = w.cyc_min;
        if (w.cyc_max > s_tot.cyc_max) s_tot.cyc_max = w.cyc_max;
        s_tot.cyc_sum += w.cyc_sum;
    }
    auto ta_mean = [](const lk::Counters& x) { return x.ta_n ? static_cast<double>(x.ta_sum) / x.ta_n : 0.0; };
    ESP_LOGI(TAG, "w%02u%s class=%s | udp rx=%u tx=%u ping=%u ctl=%u err=%u/%u/%u/%u ta_mean=%.0f"
                  " | ws rx=%u tx=%u ping=%u ctl=%u err=%u/%u/%u/%u ta_mean=%.0f sess=%u closed=%u"
                  " | sp calls=%u retries=%u retry_max=%u fail=%u torn=%u age_avg=%.0f age_max=%u stale=%u"
                  " | hook_us=%.2f/%.2f/%.2f | rssi=%d | heap=%u min=%u",
             u(w.window), w.complete ? "" : "(partial)", kClassName[cls], u(du.rx), u(du.tx), u(du.pings),
             u(du.controls), u(du.err_length), u(du.err_magic), u(du.err_crc), u(du.err_type), ta_mean(du),
             u(dw.rx), u(dw.tx), u(dw.pings), u(dw.controls), u(dw.err_length), u(dw.err_magic), u(dw.err_crc),
             u(dw.err_type), ta_mean(dw), u(c[sp::kWs].sessions), u(c[sp::kWs].closed_bad), u(w.calls),
             u(w.retries), u(w.retries_max), u(w.read_fail), u(w.torn),
             w.age_n ? static_cast<double>(w.age_sum_us) / w.age_n : 0.0, u(w.age_max_us), u(w.stale),
             cyc_us(w.cyc_min == UINT32_MAX ? 0 : w.cyc_min),
             cyc_us(w.calls ? static_cast<double>(w.cyc_sum) / w.calls : 0.0), cyc_us(w.cyc_max), station_rssi(),
             u(heap), u(heap_min()));
}

void poll_hook(bool final_partial) {
    HookWindow w;
    if (!s_hook_slot.read(&w) || w.window == 0) {
        return;
    }
    if (w.complete && w.window != s_last_window) {
        s_last_window = w.window;
        window_line(w);
    } else if (final_partial && !w.complete && w.window != s_last_window) {
        s_last_window = w.window;  // the last window of the run is never completed by Core 1
        window_line(w);
    }
}

void wait_ms(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += kPollMs) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        poll_stations();
        poll_hook(false);
    }
}

uint32_t frames_total() { return counters(sp::kUdp).rx + counters(sp::kWs).rx; }

void wait_quiet() {
    uint32_t last = frames_total();
    int64_t last_change = now_us();
    const int64_t deadline = now_us() + static_cast<int64_t>(kQuietTimeoutSeconds) * 1000000;
    while (now_us() < deadline && now_us() - last_change < static_cast<int64_t>(kQuietSeconds) * 1000000) {
        wait_ms(kPollMs);
        const uint32_t f = frames_total();
        if (f != last) {
            last = f;
            last_change = now_us();
        }
    }
}

// The control frame vectors embedded from components/comms/test_vectors (D-M6-02).
void crc_self_test() {
    int checked = 0;
    int first_fail = 0;
    const int failed = comms::frame::run_vectors(kVectorsStart, std::strlen(kVectorsStart), &checked, &first_fail);
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    const uint8_t c = comms::frame::crc8(check, sizeof(check));
    const bool ok = failed == 0 && checked >= 30 && c == 0xF4;
    ESP_LOGI(TAG, "%s", summary(kCrc, "SUMMARY CRC: %s | CRC-8 poly 0x07 init 0x00, check \"123456789\" -> 0x%02X"
                                " (want 0xF4) | embedded vectors: %d checks, %d failed%s",
                                ok ? "PASS" : "FAIL", c, checked, failed, failed ? " (see line below)" : ""));
    if (failed) {
        ESP_LOGE(TAG, "first failing vector on line %d of control_frame_vectors.txt", first_fail);
    }
}

void summarise_imu(bool have, const M3PhaseBResult& r) {
    if (!have) {
        ESP_LOGE(TAG, "%s", summary(kImu, "SUMMARY M6-IMU: FAIL, no M3 Phase B result (see the m3 lines)"));
        return;
    }
    const bool ok_missed = r.missed == 0 && r.timeouts == 0 && r.i2c_errors == 0 && r.reads == r.wakes;
    const bool ok = ok_missed && r.iv_sd_us < kIvSdMaxUs && r.jitter_us < kJitterMaxUs &&
                    r.wake_max_us < kWakeMaxLimitUs;
    ESP_LOGI(TAG, "%s", summary(kImu,
             "SUMMARY M6-IMU (whole run, %u s): %s | interval sd %.2f us (< %.0f) | jitter %.1f us (< %.0f)"
             " | wake min/avg/max %u/%.1f/%u us (max < %u) | burst avg/max %.1f/%u us (M3 avg %.1f, M5 load 605-633)"
             " | missed %u timeouts %u i2c_err %u | IMU rate %.2f Hz | windows idle %u, udp %u, ws %u, mixed %u"
             " | per-class figures: tools/analyse_m6.py",
             u(board::kM6ImuSeconds), ok ? "PASS" : "FAIL", r.iv_sd_us, kIvSdMaxUs, r.jitter_us, kJitterMaxUs,
             u(r.wake_min_us), r.wake_avg_us, u(r.wake_max_us), u(kWakeMaxLimitUs), r.burst_avg_us,
             u(r.burst_max_us), kM3BurstAvgUs, u(r.missed), u(r.timeouts), u(r.i2c_errors), r.rate_hz,
             u(s_tot.windows[0]), u(s_tot.windows[1]), u(s_tot.windows[2]), u(s_tot.windows[3])));
    if (r.iv_sd_us > kM3IvSdUs + 0.1 || r.jitter_us > kM3JitterUs + 1.0 || r.wake_max_us > kM3WakeMaxUs + 4) {
        ESP_LOGI(TAG, "R5: whole-run figures worse than the M3 baseline (sd %.2f, jitter %.1f, wake max %u us)",
                 kM3IvSdUs, kM3JitterUs, u(kM3WakeMaxUs));
    }
}

void summarise_comms(int slot, const char* name, const lk::Counters& c, bool ws) {
    char extra[96] = "";
    if (ws) {
        std::snprintf(extra, sizeof(extra), " | sessions %u, closed on a bad frame %u, plain GET 400 %u",
                      u(c.sessions), u(c.closed_bad), u(c.plain_get));
    }
    ESP_LOGI(TAG, "%s", summary(slot,
             "SUMMARY COMMS %s: rx %u, pongs sent %u, pings %u, control %u | errors length %u magic %u crc %u"
             " type %u, send errors %u | device turnaround min/mean/max %u/%.1f/%u us%s",
             name, u(c.rx), u(c.tx), u(c.pings), u(c.controls), u(c.err_length), u(c.err_magic), u(c.err_crc),
             u(c.err_type), u(c.send_errors), c.ta_n ? u(c.ta_min) : 0u,
             c.ta_n ? static_cast<double>(c.ta_sum) / c.ta_n : 0.0, u(c.ta_max), extra));
}

void test_task(void*) {
    ESP_LOGI(TAG, "M6 bench test: UDP port %u and WebSocket /ws latency, M3 Phase B IMU loop for %u s alongside",
             u(board::kUdpPort), u(board::kM6ImuSeconds));
    ESP_LOGI(TAG, ">>> plan (the IMU run starts once the servers are up):");
    ESP_LOGI(TAG, ">>>  1. join the laptop to the AP (channel %u); no traffic for the first 60 s (idle baseline)",
             board::kWifiChannel);
    ESP_LOGI(TAG, ">>>  2. .venv/bin/python tools/latency_test.py --transport udp   (10000 pings at 50 Hz, ~200 s)");
    ESP_LOGI(TAG, ">>>  3. .venv/bin/python tools/latency_test.py --transport ws    (the same over /ws)");
    ESP_LOGI(TAG, ">>>  4. laptop off the AP, phone on: http://192.168.4.1/latency.html, press Start (~200 s)");
    ESP_LOGI(TAG, ">>>  5. then leave the link quiet until the recap (heap check)");

    crc_self_test();

    const esp_err_t fs_err = hal_fs::mount();
    hal_fs::Info fi;
    const esp_err_t info_err = fs_err == ESP_OK ? hal_fs::info(&fi) : fs_err;
    ESP_LOGI(TAG, "%s", summary(kFs, "SUMMARY FS: mount %s at %s | used %u of %u B | %s", esp_err_to_name(fs_err),
                                board::kFsBasePath, u(fi.used_bytes), u(fi.total_bytes),
                                (fs_err == ESP_OK && info_err == ESP_OK) ? "PASS" : "FAIL"));

    const esp_err_t ap_err = ap::start();
    const ap::Report r = ap::report();
    const bool ap_ok = ap_err == ESP_OK && r.started && r.channel == board::kWifiChannel &&
                       r.authmode == WIFI_AUTH_WPA2_PSK && r.max_connection == board::kWifiMaxStations &&
                       r.ps == WIFI_PS_NONE && r.bandwidth == WIFI_BW_HT20;
    ESP_LOGI(TAG, "%s", summary(kAp, "SUMMARY AP: %s (%s) | channel %u, WPA2 %s, max %u, power save %s, %s",
                                ap_ok ? "PASS" : "FAIL", esp_err_to_name(ap_err), r.channel,
                                r.authmode == WIFI_AUTH_WPA2_PSK ? "yes" : "NO", r.max_connection,
                                r.ps == WIFI_PS_NONE ? "none" : "ON", r.bandwidth == WIFI_BW_HT20 ? "HT20" : "HT40"));
    if (r.placeholder_credentials) {
        ESP_LOGW(TAG, "the Wi-Fi credentials header still holds the template values: set your own in"
                      " components/comms/include/wifi_credentials.h");
    }

    const uint32_t heap_before_http = heap_free();
    const esp_err_t http_err = http::start(comms::ws_server::uri());
    ESP_LOGI(TAG, "%s", summary(kHttp, "SUMMARY HTTP+WS: %s | \"/version\", \"/ws\" (WebSocket, binary 14-byte"
                                " frames), \"/*\" files | internal heap %u -> %u B | %s", esp_err_to_name(http_err),
                                u(heap_before_http), u(heap_free()), http_err == ESP_OK ? "PASS" : "FAIL"));

    const uint32_t heap_before_udp = heap_free();
    const esp_err_t udp_err = comms::udp_server::start();
    ESP_LOGI(TAG, "%s", summary(kUdp, "SUMMARY UDP: %s | port %u, task udp_rx priority %u | internal heap %u -> %u B"
                                " | %s", esp_err_to_name(udp_err), u(board::kUdpPort), u(board::kUdpTaskPriority),
                                u(heap_before_udp), u(heap_free()), udp_err == ESP_OK ? "PASS" : "FAIL"));

    // The IMU run, with the setpoint reader on Core 1.
    m3_set_sample_hook(sample_hook);
    m3_phase_b_start(board::kM6ImuSeconds);
    wait_ms(1000);
    char cores[240];
    const bool cores_ok = tasks_on_expected_cores(cores, sizeof(cores));
    ESP_LOGI(TAG, "%s", summary(kTasks, "SUMMARY TASKS: %s | cores: %s | the /ws handler runs in httpd",
                                cores_ok ? "PASS" : "FAIL", cores));

    M3PhaseBResult imu{};
    bool have = false;
    const int64_t deadline = now_us() + static_cast<int64_t>(board::kM6ImuSeconds + 60) * 1000000;
    while (!(have = m3_phase_b_result(&imu)) && now_us() < deadline) {
        wait_ms(kPollMs);
    }
    poll_hook(true);
    summarise_imu(have, imu);

    // Setpoint slot (acceptance 7).
    const bool sp_ok = s_tot.torn == 0 && s_tot.sp_windows > 0;
    ESP_LOGI(TAG, "%s", summary(kSetpoint,
             "SUMMARY SETPOINT: %s | torn records reaching Core 1 %u (all windows, %u reads) | over %u control-traffic"
             " windows: max retries in one read %u, reads given up %u, max setpoint age %u us, stale (> %u ms) reads %u"
             " | Core 1 cost per loop iteration min/avg/max %.2f/%.2f/%.2f us",
             sp_ok ? "PASS" : s_tot.sp_windows == 0 ? "NOT TESTED (no control-traffic window)" : "FAIL",
             u(s_tot.torn), u(s_tot.calls), u(s_tot.sp_windows), u(s_tot.sp_retries_max), u(s_tot.sp_read_fail),
             u(s_tot.sp_age_max_us), u(static_cast<uint32_t>(board::kSetpointStaleUs / 1000)), u(s_tot.sp_stale),
             cyc_us(s_tot.cyc_min == UINT32_MAX ? 0 : s_tot.cyc_min),
             cyc_us(s_tot.calls ? static_cast<double>(s_tot.cyc_sum) / s_tot.calls : 0.0), cyc_us(s_tot.cyc_max)));

    ESP_LOGI(TAG, "waiting for the link to go quiet (no frame for %u s)", u(kQuietSeconds));
    wait_quiet();
    const lk::Counters cu = counters(sp::kUdp);
    const lk::Counters cw = counters(sp::kWs);
    summarise_comms(kCommsUdp, "UDP", cu, false);
    summarise_comms(kCommsWs, "WS", cw, true);

    // Heap (acceptance 8).
    const uint32_t heap_after = heap_free();
    const int delta = static_cast<int>(heap_after) - static_cast<int>(s_tot.heap_idle);
    const bool enough = cu.rx > kMinFramesPerTransport && cw.rx > kMinFramesPerTransport;
    const char* verdict = !s_tot.heap_idle_valid ? "NOT TESTED (no idle window with a station before the traffic)"
                          : !enough                ? "INCOMPLETE (fewer than 10000 frames on a transport)"
                          : static_cast<uint32_t>(std::abs(delta)) <= kLeakMaxBytes ? "PASS"
                                                                                    : "FAIL";
    ESP_LOGI(TAG, "%s", summary(kHeap, "SUMMARY HEAP: %s | internal free idle %u B, after the runs %u B, change %d B"
                                " (within %u B) | min free %u B | frames udp %u, ws %u", verdict,
                                u(s_tot.heap_idle), u(heap_after), delta, u(kLeakMaxBytes), u(heap_min()), u(cu.rx),
                                u(cw.rx)));
    recap();
    ESP_LOGI(TAG, "M6 run finished. The servers stay up; a comms line follows every %u s.", u(kIdlePrintSeconds));

    int64_t next = now_us();
    lk::Counters prev[sp::kTransports] = {cu, cw};
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        poll_stations();
        if (now_us() >= next) {
            const lk::Counters a = counters(sp::kUdp);
            const lk::Counters b = counters(sp::kWs);
            ESP_LOGI(TAG, "idle | udp rx +%u pings +%u ctl +%u | ws rx +%u pings +%u ctl +%u | rssi %d | heap %u min %u",
                     u(a.rx - prev[0].rx), u(a.pings - prev[0].pings), u(a.controls - prev[0].controls),
                     u(b.rx - prev[1].rx), u(b.pings - prev[1].pings), u(b.controls - prev[1].controls),
                     station_rssi(), u(heap_free()), u(heap_min()));
            prev[0] = a;
            prev[1] = b;
            next += static_cast<int64_t>(kIdlePrintSeconds) * 1000000;
        }
    }
}

}  // namespace

void m6_start() {
    xTaskCreatePinnedToCore(test_task, "m6_test", 8192, nullptr, 3, nullptr, 0);
}

#endif  // FE_M6_TEST
