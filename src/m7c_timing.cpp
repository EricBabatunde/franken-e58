#include "m7c_timing.h"

#include "board_config.h"

#if FE_M7C_TEST

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "flight.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "latency_hist.h"
#include "sdkconfig.h"

namespace {

const char* TAG = "m7c";

// Kconfig bools that are off are not defined at all in sdkconfig.h.
#if CONFIG_FE_R5_I2C_IRAM
constexpr bool kR5I2cIram = true;
#else
constexpr bool kR5I2cIram = false;
#endif
#if CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB
constexpr bool kR5Icache32 = true;
#else
constexpr bool kR5Icache32 = false;
#endif
#if CONFIG_FE_R5_GYRO_ONLY
constexpr bool kR5GyroOnly = true;
#else
constexpr bool kR5GyroOnly = false;
#endif

constexpr int64_t kPrintEveryUs = 60LL * 1000 * 1000;
constexpr int kEndWaitPolls = 40;     // up to 200 ms for Core 1 to publish the final set
constexpr uint32_t kEndPollMs = 5;

const char* const kConditions[] = {"idle", "udp", "ws", "page", "soak", "i1", "i2"};

// Only m7c_key() (keys task) and m7c_poll() (bench task) touch this state, both on Core 0; the
// keys task only sets requests, the bench task acts on them, so nothing is shared mid-update.
volatile int s_start_request = -1;    // condition index to start
volatile bool s_end_request = false;
volatile bool s_load_toggle_request = false;

int s_active = -1;                    // running condition, -1: none
uint32_t s_epoch = 0;
int64_t s_t_start = 0;
int64_t s_next_print = 0;
bool s_load_on = false;
flight::Timing s_t;                   // static: about 3 KB, too big for the task stack

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }

// R5 configuration of this build, from the options that define it (not from the environment name).
const char* r5_id() {
    static char id[16];
    const bool b = kR5I2cIram;
    const bool c = kR5Icache32;
    const bool d = kR5GyroOnly;
    const int n = (b ? 1 : 0) + (c ? 1 : 0) + (d ? 1 : 0);
    if (n == 0) return "a";
    if (n == 1) return b ? "b" : c ? "c" : "d";
    std::snprintf(id, sizeof(id), "e:%s%s%s", b ? "b" : "", c ? "c" : "", d ? "d" : "");
    return id;
}

template <class H>
void quant(const H& h, uint32_t q[4]) {
    q[0] = latency::percentile(h, 1, 2);
    q[1] = latency::percentile(h, 99, 100);
    q[2] = latency::percentile(h, 999, 1000);
    q[3] = h.max;
}

// One line per report, parsed by tools/analyse_m7b.py. Percentiles are upper bounds of their bin
// (1 us bins for period and wake, 10 us for burst and work; latency_hist.h).
void print_set(const char* phase, const flight::Timing& t) {
    uint32_t per[4], wk[4], b14[4], b6[4], wo[4];
    quant(t.period, per);
    quant(t.wake, wk);
    quant(t.burst14, b14);
    quant(t.burst6, b6);
    quant(t.work, wo);
    const double secs = (t.t_last_us - t.t_first_us) / 1e6;
    ESP_LOGI(TAG, "TM %s %s t=%.1f s | r5 %s load %u us%s | iter %u wakes %u missed %u timeouts %u i2c_err %u overruns %u"
                  " stale %u | period n %u dev p50/p99/p99.9/max %u/%u/%u/%u us signed min/max %d/%d over %u"
                  " | wake p50/p99/p99.9/max %u/%u/%u/%u us | burst14 n %u avg %.1f p50/p99/p99.9/max %u/%u/%u/%u us"
                  " | burst6 n %u avg %.1f p50/p99/p99.9/max %u/%u/%u/%u us | work avg %.1f p50/p99/p99.9/max"
                  " %u/%u/%u/%u us",
             s_active >= 0 ? kConditions[s_active] : "none", phase, secs, r5_id(), u(t.load_us),
             t.load_changes ? " (MIXED: load toggled in this condition)" : "", u(t.iterations), u(t.wakes), u(t.missed),
             u(t.timeouts), u(t.i2c_errors), u(t.overruns), u(t.accel_stale), u(t.period.count), u(per[0]), u(per[1]),
             u(per[2]), u(per[3]), static_cast<int>(t.period_dev_min), static_cast<int>(t.period_dev_max),
             u(t.period.overflow()), u(wk[0]), u(wk[1]), u(wk[2]), u(wk[3]), u(t.burst14.count),
             latency::mean(t.burst14), u(b14[0]), u(b14[1]), u(b14[2]), u(b14[3]), u(t.burst6.count),
             latency::mean(t.burst6), u(b6[0]), u(b6[1]), u(b6[2]), u(b6[3]), latency::mean(t.work), u(wo[0]),
             u(wo[1]), u(wo[2]), u(wo[3]));
}

void set_load(bool on, const char* why) {
    s_load_on = on;
    flight::request_load(on ? board::kSyntheticLoadUs : 0);
    if (on) {
        ESP_LOGW(TAG, "%s: synthetic load ON, busy-wait %u us after each IMU read", why, u(board::kSyntheticLoadUs));
    } else {
        ESP_LOGW(TAG, "%s: synthetic load off", why);
    }
}

void end_condition() {
    if (s_active < 0) return;
    const uint32_t req = flight::timing_publish();
    bool ok = false;
    for (int i = 0; i < kEndWaitPolls && !ok; ++i) {
        vTaskDelay(pdMS_TO_TICKS(kEndPollMs));
        ok = flight::timing(&s_t) && s_t.publish_ack == req && s_t.epoch == s_epoch;
    }
    if (ok) {
        print_set("END", s_t);
    } else {
        ESP_LOGE(TAG, "TM %s END: no final set from Core 1 (epoch %u)", kConditions[s_active], u(s_epoch));
    }
    s_active = -1;
}

void start_condition(int c) {
    end_condition();
    s_epoch = flight::timing_reset();
    s_active = c;
    s_t_start = esp_timer_get_time();
    s_next_print = s_t_start + kPrintEveryUs;
    ESP_LOGW(TAG, "TM START %s epoch %u | r5 %s | load %s | start the matching traffic now; n ends the condition",
             kConditions[c], u(s_epoch), r5_id(), s_load_on ? "ON" : "off");
}

}  // namespace

void m7c_boot_report() {
    ESP_LOGI(TAG, "M7C: latency histograms %s | R5 config %s | I2C transfer path in IRAM %s | instruction cache %u KB |"
                  " read schedule %s | synthetic load %u us (key L), off | accelerometer stale after %u us",
             flight::timing_enabled() ? "on" : "OFF (flash the esp32s3_bench or an esp32s3_r5* environment)", r5_id(),
             kR5I2cIram ? "yes" : "no", u(CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE / 1024),
             kR5GyroOnly ? "6-byte gyro, 14-byte burst every 4th frame (accelerometer 250 Hz)"
                                    : "14-byte burst every frame",
             u(board::kSyntheticLoadUs), u(board::kAccelStaleUs));
}

bool m7c_key(int ch) {
    if ((ch < '0' || ch > '6') && ch != 'n' && ch != 'L') return false;
    if (!flight::timing_enabled()) {
        ESP_LOGW(TAG, "KEY %c: latency histograms are off in this build (flash the esp32s3_bench or an esp32s3_r5*"
                      " environment)", ch);
        return true;
    }
    if (ch == 'n') {
        s_end_request = true;
    } else if (ch == 'L') {
        s_load_toggle_request = true;
    } else {
        s_start_request = ch - '0';
    }
    return true;
}

void m7c_poll() {
    if (!flight::timing_enabled()) return;
    if (s_load_toggle_request) {
        s_load_toggle_request = false;
        set_load(!s_load_on, "KEY L");
    }
    if (s_end_request) {
        s_end_request = false;
        if (s_active < 0) {
            ESP_LOGW(TAG, "KEY n: no condition running");
        } else {
            end_condition();
        }
    }
    const int c = s_start_request;
    if (c >= 0) {
        s_start_request = -1;
        start_condition(c);
    }
    if (s_active >= 0 && esp_timer_get_time() >= s_next_print) {
        s_next_print += kPrintEveryUs;
        if (flight::timing(&s_t) && s_t.epoch == s_epoch) print_set("60s", s_t);
    }
}

const char* m7c_keys_help() {
    return " | M7c: 0 idle, 1 udp, 2 ws, 3 page, 4 soak, 5 I1, 6 I2 (start a condition), n end it, L synthetic"
           " load on/off";
}

void m7c_set_load(bool on) {
    if (flight::timing_enabled()) set_load(on, "P1 integration");
}

#else

void m7c_boot_report() {}
bool m7c_key(int) { return false; }
void m7c_poll() {}
const char* m7c_keys_help() { return ""; }
void m7c_set_load(bool) {}

#endif  // FE_M7C_TEST
