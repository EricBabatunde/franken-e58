#include "m5_wifi_test.h"

#include "board_config.h"

#if FE_M5_TEST

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_fs.h"
#include "http_static.h"
#include "m3_imu_test.h"
#include "wifi_ap.h"

#if !FE_IMU_ISR_INSTRUMENT
#error "The M5 bench test needs FE_IMU_ISR_INSTRUMENT"
#endif

namespace {

const char* TAG = "m5";
namespace ap = comms::wifi_ap;
namespace http = comms::http_static;

// Runs (0 s skips a run).
constexpr uint32_t kImu1Seconds = 300;            // M5-IMU-1: AP up, no client
constexpr uint32_t kImu2Seconds = 300;            // M5-IMU-2: under the client's --load
constexpr uint32_t kHeapPrintSeconds = 10;        // internal heap line during the runs
constexpr uint32_t kJoinTimeoutSeconds = 300;     // wait for the laptop to join before IMU-2
constexpr uint32_t kSettleSeconds = 5;            // after the join, before the idle heap sample
constexpr uint32_t kLoadStartTimeoutSeconds = 120;
constexpr uint32_t kQuietSeconds = 10;            // no request for this long: load has stopped
constexpr uint32_t kQuietTimeoutSeconds = 180;
constexpr uint32_t kIdlePrintSeconds = 60;        // heap line after the recap
constexpr uint32_t kPollMs = 200;
constexpr uint32_t kLeakMaxBytes = 2048;          // acceptance 6
// M3 baseline (Phase B, Wi-Fi off) and the acceptance 8 limits.
constexpr double kM3IvSdUs = 0.44, kM3JitterUs = 3.7, kM3WakeAvgUs = 7.0, kM3BurstAvgUs = 450.4;
constexpr uint32_t kM3WakeMinUs = 6, kM3WakeMaxUs = 12;
constexpr double kIvSdMaxUs = 2.0, kJitterMaxUs = 20.0;
constexpr uint32_t kWakeMaxLimitUs = 50;
// R5 finding: a figure worse than the M3 baseline by more than the spread seen between the
// M3 and M4 runs (sd 0.44 to 0.49 us, jitter 3.6 to 3.7 us, wake max 12 to 16 us).
constexpr double kR5SdMarginUs = 0.1, kR5JitterMarginUs = 1.0, kR5BurstMarginUs = 1.0;
constexpr uint32_t kR5WakeMarginUs = 4;

// SUMMARY lines, printed again at the end.
enum Slot { kFs, kAp, kHttp, kTasks, kImu1, kImu2, kHeap, kSlots };
constexpr size_t kSummaryLen = 768;
char s_summary[kSlots][kSummaryLen];

uint32_t s_station_seq = 0;

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }
int64_t now_us() { return esp_timer_get_time(); }
uint32_t heap_free() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }
uint32_t heap_min() { return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)); }

__attribute__((format(printf, 2, 3))) const char* summary(int n, const char* fmt, ...) {
    va_list ap_args;
    va_start(ap_args, fmt);
    vsnprintf(s_summary[n], kSummaryLen, fmt, ap_args);
    va_end(ap_args);
    return s_summary[n];
}

void recap() {
    ESP_LOGI(TAG, "===== M5 SUMMARY RECAP =====");
    for (int n = 0; n < kSlots; ++n) {
        if (s_summary[n][0] != '\0') {
            ESP_LOGI(TAG, "%s", s_summary[n]);
        }
    }
    ESP_LOGI(TAG, "===== end of recap =====");
}

// Prints station joins and leaves (the event handler only records them).
void poll_stations() {
    ap::StationLog l;
    if (!ap::station_log(&l) || l.seq == s_station_seq) {
        return;
    }
    s_station_seq = l.seq;
    if (l.last_join) {
        ESP_LOGI(TAG, "station joined %02x:%02x:%02x:%02x:%02x:%02x | joins %u leaves %u | associated now %d",
                 l.last_mac[0], l.last_mac[1], l.last_mac[2], l.last_mac[3], l.last_mac[4], l.last_mac[5],
                 u(l.joins), u(l.leaves), ap::stations());
    } else {
        ESP_LOGI(TAG, "station left %02x:%02x:%02x:%02x:%02x:%02x (reason %u) | joins %u leaves %u | associated now %d",
                 l.last_mac[0], l.last_mac[1], l.last_mac[2], l.last_mac[3], l.last_mac[4], l.last_mac[5],
                 l.last_reason, u(l.joins), u(l.leaves), ap::stations());
    }
}

void wait_ms(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += kPollMs) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        poll_stations();
    }
}

void print_heap(const char* what) {
    ESP_LOGI(TAG, "heap %s: internal free %u B, min free %u B | HTTP requests %u", what, u(heap_free()),
             u(heap_min()), u(http::counters().requests));
}

const char* ps_name(wifi_ps_type_t p) {
    return p == WIFI_PS_NONE ? "none" : p == WIFI_PS_MIN_MODEM ? "min modem" : "max modem";
}

const char* auth_name(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN:          return "open";
        case WIFI_AUTH_WPA_PSK:       return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2-PSK";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
        default:                      return "other";
    }
}

// Core of each named task: 0, 1, "any" (no affinity) or "missing".
bool tasks_on_expected_cores(char* buf, size_t len) {
    struct Want {
        const char* name;
        BaseType_t core;
    };
    static const Want kWant[] = {{"wifi", 0}, {"tiT", 0}, {"httpd", 0}, {"sys_evt", 0},
                                 {"esp_timer", 0}, {"m5_test", 0}, {"m3_report", 0}, {"imu", 1}};
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

// One M3 Phase B run on Core 1 with heap lines from Core 0. False if no result arrived.
bool run_imu(const char* name, uint32_t seconds, M3PhaseBResult* out) {
    ESP_LOGI(TAG, "%s: M3 Phase B for %u s on Core 1", name, u(seconds));
    m3_phase_b_start(seconds);
    const int64_t t0 = now_us();
    const int64_t deadline = t0 + static_cast<int64_t>(seconds + 60) * 1000000;
    int64_t next_heap = t0 + static_cast<int64_t>(kHeapPrintSeconds) * 1000000;
    while (!m3_phase_b_result(out)) {
        if (now_us() > deadline) {
            return false;
        }
        wait_ms(kPollMs);
        if (now_us() >= next_heap) {
            char what[40];
            std::snprintf(what, sizeof(what), "%s t=%us", name, u(static_cast<uint32_t>((now_us() - t0) / 1000000)));
            print_heap(what);
            next_heap += static_cast<int64_t>(kHeapPrintSeconds) * 1000000;
        }
    }
    return true;
}

void summarise_imu(int slot, const char* name, const char* condition, bool have, const M3PhaseBResult& r,
                   uint32_t requests) {
    if (!have) {
        ESP_LOGE(TAG, "%s", summary(slot, "SUMMARY %s (%s): FAIL, no M3 Phase B result (see the m3 lines)",
                                    name, condition));
        return;
    }
    const bool ok_missed = r.missed == 0 && r.timeouts == 0 && r.i2c_errors == 0 && r.reads == r.wakes;
    const bool ok_sd = r.iv_sd_us < kIvSdMaxUs;
    const bool ok_jit = r.jitter_us < kJitterMaxUs;
    const bool ok_wake = r.wake_max_us < kWakeMaxLimitUs;
    char r5[96];
    size_t n = 0;
    r5[0] = '\0';
    if (r.iv_sd_us > kM3IvSdUs + kR5SdMarginUs) n += std::snprintf(r5 + n, sizeof(r5) - n, " sd");
    if (r.jitter_us > kM3JitterUs + kR5JitterMarginUs) n += std::snprintf(r5 + n, sizeof(r5) - n, " jitter");
    if (r.wake_max_us > kM3WakeMaxUs + kR5WakeMarginUs) n += std::snprintf(r5 + n, sizeof(r5) - n, " wake-max");
    if (r.burst_avg_us > kM3BurstAvgUs + kR5BurstMarginUs) n += std::snprintf(r5 + n, sizeof(r5) - n, " burst-avg");
    ESP_LOGI(TAG, "%s", summary(slot,
             "SUMMARY %s (%s): %s | interval sd %.2f us (M3 %.2f; < %.0f: %s) | jitter %.1f us (M3 %.1f; < %.0f: %s)"
             " | wake min/avg/max %u/%.1f/%u us (M3 %u/%.1f/%u; max < %u: %s) | burst avg/max %.1f/%u us (M3 avg %.1f)"
             " | missed %u timeouts %u i2c_err %u (%s) | IMU rate %.2f Hz | HTTP requests during run %u"
             " | R5: %s",
             name, condition, (ok_missed && ok_sd && ok_jit && ok_wake) ? "PASS" : "FAIL", r.iv_sd_us, kM3IvSdUs,
             kIvSdMaxUs, ok_sd ? "PASS" : "FAIL", r.jitter_us, kM3JitterUs, kJitterMaxUs, ok_jit ? "PASS" : "FAIL",
             u(r.wake_min_us), r.wake_avg_us, u(r.wake_max_us), u(kM3WakeMinUs), kM3WakeAvgUs, u(kM3WakeMaxUs),
             u(kWakeMaxLimitUs), ok_wake ? "PASS" : "FAIL", r.burst_avg_us, u(r.burst_max_us), kM3BurstAvgUs,
             u(r.missed), u(r.timeouts), u(r.i2c_errors), ok_missed ? "PASS" : "FAIL", r.rate_hz, u(requests),
             n ? r5 + 1 : "no deterioration beyond the M3 spread"));
}

// Waits until no HTTP request has arrived for kQuietSeconds (bounded).
void wait_quiet() {
    uint32_t last = http::counters().requests;
    int64_t last_change = now_us();
    const int64_t deadline = now_us() + static_cast<int64_t>(kQuietTimeoutSeconds) * 1000000;
    while (now_us() < deadline && now_us() - last_change < static_cast<int64_t>(kQuietSeconds) * 1000000) {
        wait_ms(kPollMs);
        const uint32_t c = http::counters().requests;
        if (c != last) {
            last = c;
            last_change = now_us();
        }
    }
}

void test_task(void*) {
    ESP_LOGI(TAG, "M5 bench test: LittleFS, AP on channel %u, static pages, M5-IMU-1 %u s (no client),"
                  " M5-IMU-2 %u s (client --load)", board::kWifiChannel, u(kImu1Seconds), u(kImu2Seconds));
    const uint32_t heap_boot = heap_free();
    print_heap("at start");

    // Filesystem.
    const esp_err_t fs_err = hal_fs::mount();
    hal_fs::Info fi;
    const esp_err_t info_err = fs_err == ESP_OK ? hal_fs::info(&fi) : fs_err;
    ESP_LOGI(TAG, "%s", summary(kFs, "SUMMARY FS: mount %s (read-only, no format) at %s, partition \"%s\" | total %u B,"
                                " used %u B, free %u B | %s", esp_err_to_name(fs_err), board::kFsBasePath,
                                board::kFsPartitionLabel, u(fi.total_bytes), u(fi.used_bytes),
                                u(fi.total_bytes - fi.used_bytes),
                                (fs_err == ESP_OK && info_err == ESP_OK) ? "PASS" : "FAIL"));

    // Access point.
    const uint32_t heap_before_ap = heap_free();
    const esp_err_t ap_err = ap::start();
    const ap::Report r = ap::report();
    const uint32_t heap_after_ap = heap_free();
    const bool ap_ok = ap_err == ESP_OK && r.started && r.channel == board::kWifiChannel &&
                       r.authmode == WIFI_AUTH_WPA2_PSK && r.max_connection == board::kWifiMaxStations &&
                       r.ps == WIFI_PS_NONE && r.bandwidth == WIFI_BW_HT20 &&
                       r.protocol == (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    ESP_LOGI(TAG, "%s", summary(kAp, "SUMMARY AP: %s (%s) | channel %u, %s, max connections %u, power save %s,"
                                " bandwidth %s, protocol b%s%s%s | esp_wifi_start to AP started %u us | internal heap"
                                " before %u B, after %u B (%d B), min free %u B", ap_ok ? "PASS" : "FAIL",
                                esp_err_to_name(ap_err), r.channel, auth_name(r.authmode), r.max_connection,
                                ps_name(r.ps), r.bandwidth == WIFI_BW_HT20 ? "HT20" : "HT40",
                                (r.protocol & WIFI_PROTOCOL_11B) ? "" : "(no b)",
                                (r.protocol & WIFI_PROTOCOL_11G) ? "/g" : "", (r.protocol & WIFI_PROTOCOL_11N) ? "/n" : "",
                                u(r.start_us), u(heap_before_ap), u(heap_after_ap),
                                static_cast<int>(heap_after_ap - heap_before_ap), u(heap_min())));
    if (r.placeholder_credentials) {
        ESP_LOGW(TAG, "the Wi-Fi credentials header still holds the template values: set your own in"
                      " components/comms/include/wifi_credentials.h");
    }

    // HTTP server.
    const esp_err_t http_err = http::start();
    ESP_LOGI(TAG, "%s", summary(kHttp, "SUMMARY HTTP: server %s | GET only, \"/\" -> index.html.gz, \"/version\","
                                " \"/ws\" reserved | internal heap after %u B | %s", esp_err_to_name(http_err),
                                u(heap_free()), http_err == ESP_OK ? "PASS" : "FAIL"));
    ESP_LOGI(TAG, "AP and HTTP up: internal heap %u B at start of test, %u B now", u(heap_boot), u(heap_free()));

    // M5-IMU-1: AP up, no client.
    M3PhaseBResult imu1{};
    bool have1 = false;
    if (kImu1Seconds > 0) {
        if (ap::stations() > 0) {
            ESP_LOGW(TAG, "a station is associated: M5-IMU-1 is meant to run with no client");
        }
        const uint32_t req0 = http::counters().requests;
        // Task cores, once Phase B's tasks exist.
        m3_phase_b_start(kImu1Seconds);
        wait_ms(1000);
        char cores[200];
        const bool cores_ok = tasks_on_expected_cores(cores, sizeof(cores));
        ESP_LOGI(TAG, "%s", summary(kTasks, "SUMMARY TASKS: %s | cores: %s", cores_ok ? "PASS" : "FAIL", cores));
        // The run is already going; collect it with the heap lines.
        const int64_t t0 = now_us();
        const int64_t deadline = t0 + static_cast<int64_t>(kImu1Seconds + 60) * 1000000;
        int64_t next_heap = t0 + static_cast<int64_t>(kHeapPrintSeconds) * 1000000;
        while (!(have1 = m3_phase_b_result(&imu1)) && now_us() < deadline) {
            wait_ms(kPollMs);
            if (now_us() >= next_heap) {
                char what[40];
                std::snprintf(what, sizeof(what), "M5-IMU-1 t=%us", u(static_cast<uint32_t>((now_us() - t0) / 1000000) + 1));
                print_heap(what);
                next_heap += static_cast<int64_t>(kHeapPrintSeconds) * 1000000;
            }
        }
        summarise_imu(kImu1, "M5-IMU-1", "AP up, no client", have1, imu1, http::counters().requests - req0);
    }
    const uint32_t heap_ap_idle = heap_free();
    print_heap("AP up, idle, no client");

    // M5-IMU-2: under the client's load.
    uint32_t heap_idle = heap_ap_idle;
    bool loaded = false;
    if (kImu2Seconds > 0) {
        ESP_LOGI(TAG, ">>> join the laptop to the AP (channel %u) within %u s", board::kWifiChannel,
                 u(kJoinTimeoutSeconds));
        const int64_t join_deadline = now_us() + static_cast<int64_t>(kJoinTimeoutSeconds) * 1000000;
        int64_t next_prompt = now_us() + 10 * 1000000;
        while (ap::stations() <= 0 && now_us() < join_deadline) {
            wait_ms(kPollMs);
            if (now_us() >= next_prompt) {
                ESP_LOGI(TAG, ">>> waiting for a station to join (%u s left)",
                         u(static_cast<uint32_t>((join_deadline - now_us()) / 1000000)));
                next_prompt += 10 * 1000000;
            }
        }
        wait_ms(kSettleSeconds * 1000);
        heap_idle = heap_free();
        print_heap("idle reference (AP up, station joined, no load)");
        ESP_LOGI(TAG, ">>> start the load now: python3 tools/m5_http_test.py --load %u",
                 u(kImu2Seconds + 30));
        const uint32_t req_before = http::counters().requests;
        const int64_t load_deadline = now_us() + static_cast<int64_t>(kLoadStartTimeoutSeconds) * 1000000;
        while (http::counters().requests == req_before && now_us() < load_deadline) {
            wait_ms(kPollMs);
        }
        loaded = http::counters().requests != req_before;
        if (!loaded) {
            ESP_LOGW(TAG, "no HTTP request within %u s: M5-IMU-2 runs without load", u(kLoadStartTimeoutSeconds));
        }
        const uint32_t req0 = http::counters().requests;
        M3PhaseBResult imu2{};
        const bool have2 = run_imu("M5-IMU-2", kImu2Seconds, &imu2);
        summarise_imu(kImu2, "M5-IMU-2", loaded ? "AP up, client load" : "AP up, NO LOAD SEEN", have2, imu2,
                      http::counters().requests - req0);
        ESP_LOGI(TAG, "waiting for the load to stop (no request for %u s)", u(kQuietSeconds));
        wait_quiet();
    }

    // Heap after the load (acceptance 6).
    const uint32_t heap_after = heap_free();
    const int delta = static_cast<int>(heap_after) - static_cast<int>(heap_idle);
    const http::Counters c = http::counters();
    ESP_LOGI(TAG, "%s", summary(kHeap, "SUMMARY HEAP: %s | internal free idle %u B, after load %u B, change %d B"
                                " (within %u B) | min free %u B | AP up with no client %u B | HTTP requests %u,"
                                " 200 %u, 404 %u, 400 %u, send errors %u, body %llu B", 
                                (static_cast<uint32_t>(std::abs(delta)) <= kLeakMaxBytes && loaded) ? "PASS"
                                : loaded ? "FAIL" : "NOT TESTED (no load)",
                                u(heap_idle), u(heap_after), delta, u(kLeakMaxBytes), u(heap_min()), u(heap_ap_idle),
                                u(c.requests), u(c.ok), u(c.not_found), u(c.bad_request), u(c.send_errors),
                                static_cast<unsigned long long>(c.body_bytes)));
    recap();
    ESP_LOGI(TAG, "M5 runs finished. The AP and the page stay up for the phone and client tests.");

    // Keep reporting joins, leaves and the heap.
    int64_t next = now_us();
    for (;;) {
        wait_ms(kPollMs);
        if (now_us() >= next) {
            print_heap("idle");
            next += static_cast<int64_t>(kIdlePrintSeconds) * 1000000;
        }
    }
}

}  // namespace

void m5_start() {
    xTaskCreatePinnedToCore(test_task, "m5_test", 8192, nullptr, 3, nullptr, 0);
}

#endif  // FE_M5_TEST
