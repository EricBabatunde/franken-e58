#include "m3_imu_test.h"

#include "board_config.h"

#if FE_M3_TEST || FE_M4_TEST

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "drv_mpu6500.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_i2c.h"
#include "seqlock.h"

#if !FE_IMU_ISR_INSTRUMENT
#error "The M3 bench test needs FE_IMU_ISR_INSTRUMENT"
#endif

namespace {

const char* TAG = "m3";
using hal_i2c::Bus;
using drv_mpu6500::Step;

constexpr uint32_t kA1Seconds = 10;
constexpr uint32_t kA2Seconds = 60;
constexpr uint32_t kBSeconds = 600;
constexpr int64_t kWindowUs = 10 * 1000 * 1000;
constexpr uint8_t kPhaseADlpfCfg = 0;        // 8 kHz data-ready expected (RM p.14)
constexpr uint32_t kA2Decimation = 8;
constexpr uint32_t kWaitTimeoutMs = 100;     // a wait this long without a notification is a timeout
constexpr uint32_t kImuTaskStackBytes = 4096;
constexpr uint32_t kReporterStackBytes = 6144;
constexpr double kCpuMhz = 240.0;            // CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240
constexpr double kRateTolerance = 0.01;      // steadiness, and 1000 +/- 10 Hz (PLL +/-1 %, PS p.11)
constexpr uint32_t kFlagUs = 200;            // acceptance 6
constexpr double kAccelMagMin = 0.90;        // acceptance 7: 60 mg zero-g offset and
constexpr double kAccelMagMax = 1.10;        // 3 % sensitivity tolerance, worst case (PS p.8)
constexpr double kGyroBiasMaxDps = 5.0;      // initial zero-rate output tolerance (PS p.7)

enum Phase : uint8_t { kA1 = 0, kA2 = 1, kB = 2, kNumPhases = 3 };
const char* const kPhaseName[kNumPhases] = {"A1", "A2", "B"};
constexpr uint32_t kMaxWindows = 64;

enum class State : uint8_t { kStarting, kRunning, kDone, kFailed };

// One 10 s window or a whole phase. Plain data, copied through the seqlock.
struct Stats {
    uint32_t index = 0;            // window number within the phase (0 for a phase total)
    uint32_t length_us = 0;
    uint32_t raw_pulses = 0;       // every data-ready pulse, counted by the ISR
    uint32_t wakes = 0;            // notifications received
    uint32_t missed = 0;           // sum of (notification count - 1)
    uint32_t timeouts = 0;         // waits of kWaitTimeoutMs with no notification
    uint32_t reads = 0;            // successful burst reads
    uint32_t i2c_errors = 0;
    uint32_t iv_n = 0;             // intervals between successive notify timestamps
    uint32_t iv_min = UINT32_MAX;
    uint32_t iv_max = 0;
    uint64_t iv_sum = 0;
    uint64_t iv_sumsq = 0;
    uint32_t burst_min = UINT32_MAX;
    uint32_t burst_max = 0;
    uint64_t burst_sum = 0;
    uint32_t lat_min = UINT32_MAX;  // ISR timestamp to task wake-up
    uint32_t lat_max = 0;
    uint64_t lat_sum = 0;
    uint32_t samples = 0;           // samples used for the static values
    double accel_mag_sum = 0.0;     // g
    int64_t gyro_sum[3] = {0, 0, 0};      // raw LSB
    uint64_t gyro_sumsq[3] = {0, 0, 0};   // raw LSB squared
    // ISR instrumentation, cumulative over the phase so far.
    uint32_t raw_iv_min = 0;
    uint32_t raw_iv_max = 0;
    uint32_t isr_cycles_max = 0;    // our handler only, not the IDF GPIO dispatcher
};

struct ConfigRecord {
    bool valid = false;
    drv_mpu6500::InitStatus st{};
};

struct Snapshot {
    State state = State::kStarting;
    uint8_t phase = kA1;
    esp_err_t bus_err = ESP_OK;
    esp_err_t int_err = ESP_OK;
    ConfigRecord config[4];        // 0 = init, 1 = before A1, 2 = before A2, 3 = before B
    uint32_t window_seq = 0;       // increments with every published window
    uint8_t window_phase = kA1;
    Stats window;
    bool summary_valid[kNumPhases] = {false, false, false};
    Stats summary[kNumPhases];
};

// ---- Seqlock slot (components/seqlock): Core 1 writes, Core 0 reads. No locks.
SeqlockSlot<Snapshot> s_slot;

void publish(const Snapshot& s) { s_slot.write(s); }
bool read_snapshot(Snapshot* out) { return s_slot.read(out); }

// Phase-B-only mode for the M4 interference test (P5). Set before the tasks start.
bool s_b_only = false;
uint32_t s_b_seconds = kBSeconds;

// Phase B figures handed to the M4 test once the Phase B summary has been printed.
M3PhaseBResult s_b_result{};
std::atomic<bool> s_b_result_ready{false};

// ---- Core 1 task. After init: no logging, no allocation, no lock shared with Core 0.

TaskHandle_t s_imu_task = nullptr;
Snapshot s_work;  // Core 1 task only

void keep_min(uint32_t& m, uint32_t v) { if (v < m) m = v; }
void keep_max(uint32_t& m, uint32_t v) { if (v > m) m = v; }

void merge(Stats& t, const Stats& w) {
    t.length_us += w.length_us;
    t.raw_pulses += w.raw_pulses;
    t.wakes += w.wakes;
    t.missed += w.missed;
    t.timeouts += w.timeouts;
    t.reads += w.reads;
    t.i2c_errors += w.i2c_errors;
    t.iv_n += w.iv_n;
    keep_min(t.iv_min, w.iv_min);
    keep_max(t.iv_max, w.iv_max);
    t.iv_sum += w.iv_sum;
    t.iv_sumsq += w.iv_sumsq;
    keep_min(t.burst_min, w.burst_min);
    keep_max(t.burst_max, w.burst_max);
    t.burst_sum += w.burst_sum;
    keep_min(t.lat_min, w.lat_min);
    keep_max(t.lat_max, w.lat_max);
    t.lat_sum += w.lat_sum;
    t.samples += w.samples;
    t.accel_mag_sum += w.accel_mag_sum;
    for (int i = 0; i < 3; ++i) {
        t.gyro_sum[i] += w.gyro_sum[i];
        t.gyro_sumsq[i] += w.gyro_sumsq[i];
    }
}

void take_isr_instrument(Stats& s) {
    const drv_mpu6500::IsrInstrument in = drv_mpu6500::isr_instrument();
    s.raw_iv_min = in.raw_iv_min_us;
    s.raw_iv_max = in.raw_iv_max_us;
    s.isr_cycles_max = in.handler_cycles_max;
}

void account_sample(Stats& w, const drv_mpu6500::RawSample& s) {
    const float ax = drv_mpu6500::accel_g(s.accel[0]);
    const float ay = drv_mpu6500::accel_g(s.accel[1]);
    const float az = drv_mpu6500::accel_g(s.accel[2]);
    w.accel_mag_sum += sqrtf(ax * ax + ay * ay + az * az);
    for (int i = 0; i < 3; ++i) {
        const int64_t g = s.gyro[i];
        w.gyro_sum[i] += g;
        w.gyro_sumsq[i] += static_cast<uint64_t>(g * g);
    }
    w.samples++;
}

[[noreturn]] void park() {
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

// Runs the driver's full init sequence (slot 0) or reconfigure() and publishes the result.
bool configure(int slot, uint8_t dlpf_cfg) {
    const drv_mpu6500::InitStatus st =
        (slot == 0) ? drv_mpu6500::init(Bus::A) : drv_mpu6500::reconfigure(dlpf_cfg);
    s_work.config[slot].valid = true;
    s_work.config[slot].st = st;
    if (st.step != Step::kNone) {
        s_work.state = State::kFailed;
    }
    publish(s_work);
    return st.step == Step::kNone;
}

void run_phase(Phase ph, uint32_t seconds, uint32_t notify_every, bool reads) {
    drv_mpu6500::isr_set_notify_every(notify_every);
    drv_mpu6500::isr_instrument_reset();
    ulTaskNotifyTake(pdTRUE, 0);  // drop notifications left over from the reconfigure

    s_work.phase = ph;
    publish(s_work);

    Stats total{};
    Stats win{};
    uint32_t index = 0;
    const int64_t t_phase = esp_timer_get_time();
    const int64_t phase_us = static_cast<int64_t>(seconds) * 1000 * 1000;
    int64_t t_win = t_phase;
    uint32_t raw_mark = drv_mpu6500::raw_pulses();
    uint32_t prev_irq = 0;
    bool have_prev = false;
    drv_mpu6500::RawSample sample{};

    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now - t_win >= kWindowUs) {
            const uint32_t raw = drv_mpu6500::raw_pulses();
            win.index = ++index;
            win.length_us = static_cast<uint32_t>(now - t_win);
            win.raw_pulses = raw - raw_mark;
            raw_mark = raw;
            take_isr_instrument(win);
            merge(total, win);
            s_work.window = win;
            s_work.window_phase = ph;
            s_work.window_seq++;
            publish(s_work);
            win = Stats{};
            t_win = now;
            if (now - t_phase >= phase_us) {
                break;
            }
        }

        if (!reads) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kWaitTimeoutMs));
        const uint32_t woke = static_cast<uint32_t>(esp_timer_get_time());
        if (n == 0) {
            win.timeouts++;
            have_prev = false;
            continue;
        }
        win.wakes++;
        if (n > 1) {
            win.missed += n - 1;
        }
        const uint32_t t_irq = drv_mpu6500::last_notify_us();
        const uint32_t lat = woke - t_irq;
        keep_min(win.lat_min, lat);
        keep_max(win.lat_max, lat);
        win.lat_sum += lat;
        if (have_prev) {
            const uint32_t iv = t_irq - prev_irq;
            win.iv_n++;
            keep_min(win.iv_min, iv);
            keep_max(win.iv_max, iv);
            win.iv_sum += iv;
            win.iv_sumsq += static_cast<uint64_t>(iv) * iv;
        }
        prev_irq = t_irq;
        have_prev = true;

        const uint32_t t0 = static_cast<uint32_t>(esp_timer_get_time());
        const esp_err_t e = drv_mpu6500::read_burst(t_irq, &sample);
        const uint32_t dur = static_cast<uint32_t>(esp_timer_get_time()) - t0;
        if (e != ESP_OK) {
            win.i2c_errors++;
            continue;
        }
        win.reads++;
        keep_min(win.burst_min, dur);
        keep_max(win.burst_max, dur);
        win.burst_sum += dur;
        if (static_cast<int32_t>(t_irq - drv_mpu6500::gyro_valid_from_us()) >= 0) {
            account_sample(win, sample);
        }
    }

    take_isr_instrument(total);
    s_work.summary[ph] = total;
    s_work.summary_valid[ph] = true;
    publish(s_work);
}

void imu_task(void*) {
    // Init phase: creates bus A and the interrupt on Core 1 (bus A's I2C interrupt and the
    // GPIO ISR service are allocated on the calling core). Results go to Core 0 to print.
    s_work.bus_err = hal_i2c::init(Bus::A, board::kImuSda, board::kImuScl, board::kI2cHz);
    if (s_work.bus_err != ESP_OK) {
        s_work.state = State::kFailed;
        publish(s_work);
        park();
    }
    if (!configure(0, static_cast<uint8_t>(board::kImuDlpfCfg))) {
        park();
    }
    s_work.int_err = drv_mpu6500::int_install(xTaskGetCurrentTaskHandle());
    if (s_work.int_err != ESP_OK) {
        s_work.state = State::kFailed;
        publish(s_work);
        park();
    }
    s_work.state = State::kRunning;

    if (s_b_only) {
        if (!configure(3, static_cast<uint8_t>(board::kImuDlpfCfg))) {
            park();
        }
        run_phase(kB, s_b_seconds, board::kImuIsrDecimation, true);
        drv_mpu6500::isr_set_notify_every(0);
        s_work.state = State::kDone;
        publish(s_work);
        park();
    }

    // Phase A1: raw pulses only, no reads (the status flag is never cleared by a read).
    if (!configure(1, kPhaseADlpfCfg)) {
        park();
    }
    run_phase(kA1, kA1Seconds, 0, false);

    // Phase A2: read on every 8th pulse; the ISR still counts every pulse.
    if (!configure(2, kPhaseADlpfCfg)) {
        park();
    }
    run_phase(kA2, kA2Seconds, kA2Decimation, true);

    // Phase B: the compiled-in flight settings.
    if (!configure(3, static_cast<uint8_t>(board::kImuDlpfCfg))) {
        park();
    }
    run_phase(kB, kBSeconds, board::kImuIsrDecimation, true);

    drv_mpu6500::isr_set_notify_every(0);
    s_work.state = State::kDone;
    publish(s_work);
    park();
}

// ---- Core 0 reporter.

unsigned u(uint32_t v) { return static_cast<unsigned>(v); }
unsigned u_min(uint32_t v) { return v == UINT32_MAX ? 0u : static_cast<unsigned>(v); }
double avg(uint64_t sum, uint32_t n) { return n ? static_cast<double>(sum) / n : 0.0; }
double sdev(uint64_t sum, uint64_t sumsq, uint32_t n) {
    if (n < 2) {
        return 0.0;
    }
    const double m = static_cast<double>(sum) / n;
    const double v = static_cast<double>(sumsq) / n - m * m;
    return v > 0.0 ? std::sqrt(v) : 0.0;
}
double rate_hz(uint32_t count, uint32_t length_us) {
    return length_us ? count * 1e6 / length_us : 0.0;
}
double isr_us(uint32_t cycles) { return cycles / kCpuMhz; }

struct GyroStats {
    double mean[3];
    double sd[3];
};

GyroStats gyro_stats(const Stats& s) {
    const double scale = drv_mpu6500::gyro_dps(1);  // deg/s per LSB
    GyroStats g{};
    for (int i = 0; i < 3; ++i) {
        if (s.samples == 0) {
            continue;
        }
        const double m = static_cast<double>(s.gyro_sum[i]) / s.samples;
        const double v = static_cast<double>(s.gyro_sumsq[i]) / s.samples - m * m;
        g.mean[i] = m * scale;
        g.sd[i] = (v > 0.0 ? std::sqrt(v) : 0.0) * scale;
    }
    return g;
}

double accel_mag(const Stats& s) { return s.samples ? s.accel_mag_sum / s.samples : 0.0; }

const char* step_name(Step s) {
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

const char* const kConfigName[4] = {"init", "reconfigure for A1", "reconfigure for A2",
                                    "reconfigure for B"};

void print_config(int slot, const drv_mpu6500::InitStatus& st) {
    if (st.step == Step::kNone) {
        ESP_LOGI(TAG, "%s: WHO_AM_I 0x%02X, PWR_MGMT_1 after reset 0x%02X, DLPF_CFG %u, "
                      "%u registers written and read back OK",
                 kConfigName[slot], st.who_am_i, st.pwr_mgmt_1, st.dlpf_cfg, st.n_checked);
    } else {
        ESP_LOGE(TAG, "%s FAILED at %s: reg 0x%02X expected 0x%02X read 0x%02X (%s). "
                      "WHO_AM_I 0x%02X, PWR_MGMT_1 0x%02X. Test stopped.",
                 kConfigName[slot], step_name(st.step), st.reg, st.expected, st.actual,
                 esp_err_to_name(st.err), st.who_am_i, st.pwr_mgmt_1);
    }
}

void print_window(uint8_t ph, const Stats& w) {
    const double secs = w.length_us / 1e6;
    if (ph == kA1) {
        ESP_LOGI(TAG, "A1 w%02u %.2fs | raw %u pulses, %.1f Hz | raw iv min/max %u/%u us",
                 u(w.index), secs, u(w.raw_pulses), rate_hz(w.raw_pulses, w.length_us),
                 u(w.raw_iv_min), u(w.raw_iv_max));
        return;
    }
    if (ph == kA2) {
        ESP_LOGI(TAG, "A2 w%02u %.2fs | raw %.1f Hz | read %.1f Hz | iv min/max/mean %u/%u/%.1f us"
                      " | wake min/avg/max %u/%.1f/%u us | burst min/avg/max %u/%.1f/%u us"
                      " | missed %u timeouts %u i2c_err %u",
                 u(w.index), secs, rate_hz(w.raw_pulses, w.length_us), rate_hz(w.reads, w.length_us),
                 u_min(w.iv_min), u(w.iv_max), avg(w.iv_sum, w.iv_n), u_min(w.lat_min),
                 avg(w.lat_sum, w.wakes), u(w.lat_max), u_min(w.burst_min), avg(w.burst_sum, w.reads),
                 u(w.burst_max), u(w.missed), u(w.timeouts), u(w.i2c_errors));
        return;
    }
    const GyroStats g = gyro_stats(w);
    ESP_LOGI(TAG, "B  w%02u %.2fs | rate %.1f Hz | iv min/max/sd %u/%u/%.1f us"
                  " | burst min/avg/max %u/%.1f/%u us | wake min/avg/max %u/%.1f/%u us"
                  " | missed %u timeouts %u i2c_err %u | |a| %.4f g"
                  " | gyro mean %.3f/%.3f/%.3f sd %.3f/%.3f/%.3f dps",
             u(w.index), secs, rate_hz(w.raw_pulses, w.length_us), u_min(w.iv_min), u(w.iv_max),
             sdev(w.iv_sum, w.iv_sumsq, w.iv_n), u_min(w.burst_min), avg(w.burst_sum, w.reads),
             u(w.burst_max), u_min(w.lat_min), avg(w.lat_sum, w.wakes), u(w.lat_max), u(w.missed),
             u(w.timeouts), u(w.i2c_errors), accel_mag(w), g.mean[0], g.mean[1], g.mean[2],
             g.sd[0], g.sd[1], g.sd[2]);
}

// Window rates kept on Core 0 for the steadiness check.
double s_raw_rate[kNumPhases][kMaxWindows];
double s_read_rate[kNumPhases][kMaxWindows];
uint32_t s_n_rates[kNumPhases] = {0, 0, 0};

bool steady(int ph, double raw_mean, double read_mean, bool check_reads) {
    for (uint32_t i = 0; i < s_n_rates[ph]; ++i) {
        if (std::fabs(s_raw_rate[ph][i] - raw_mean) > kRateTolerance * raw_mean) {
            return false;
        }
        if (check_reads && std::fabs(s_read_rate[ph][i] - read_mean) > kRateTolerance * read_mean) {
            return false;
        }
    }
    return s_n_rates[ph] > 0;
}

void print_summary_a1(const Stats& s) {
    const char* cont = s.raw_pulses == 0   ? "NONE: no data-ready pulse at all"
                       : s.raw_pulses == 1 ? "STOPPED after the first pulse (status never cleared)"
                                           : "continue without any status read";
    ESP_LOGI(TAG, "SUMMARY A1: DLPF_CFG=0, no reads, %u raw pulses in %.2f s = %.1f Hz; pulses %s; "
                  "raw iv min/max %u/%u us; ISR handler max %u cycles (%.2f us, our handler only, "
                  "not the IDF GPIO dispatcher). Expected 8000 +/- 80 Hz if the gyro drives "
                  "data-ready (expectation only, not a pass threshold)",
             u(s.raw_pulses), s.length_us / 1e6, rate_hz(s.raw_pulses, s.length_us), cont,
             u(s.raw_iv_min), u(s.raw_iv_max), u(s.isr_cycles_max), isr_us(s.isr_cycles_max));
}

void print_summary_a2(const Stats& s) {
    const double raw = rate_hz(s.raw_pulses, s.length_us);
    const double rd = rate_hz(s.reads, s.length_us);
    ESP_LOGI(TAG, "SUMMARY A2: DLPF_CFG=0, read every %u pulses | raw %.1f Hz | read %.1f Hz"
                  " | iv min/max/mean %u/%u/%.1f us | raw iv min/max %u/%u us | steady (each 10 s "
                  "window within 1 %% of the mean): %s | wake min/avg/max %u/%.1f/%u us"
                  " | burst min/avg/max %u/%.1f/%u us | missed %u timeouts %u i2c_err %u"
                  " | ISR handler max %u cycles (%.2f us, our handler only)",
             u(kA2Decimation), raw, rd, u_min(s.iv_min), u(s.iv_max), avg(s.iv_sum, s.iv_n),
             u(s.raw_iv_min), u(s.raw_iv_max), steady(kA2, raw, rd, true) ? "yes" : "NO",
             u_min(s.lat_min), avg(s.lat_sum, s.wakes), u(s.lat_max), u_min(s.burst_min),
             avg(s.burst_sum, s.reads), u(s.burst_max), u(s.missed), u(s.timeouts),
             u(s.i2c_errors), u(s.isr_cycles_max), isr_us(s.isr_cycles_max));
}

void print_summary_b(const Stats& s, uint32_t heap_start, uint32_t heap_end, uint32_t stack_free,
                     M3PhaseBResult* out) {
    const double rate = rate_hz(s.raw_pulses, s.length_us);
    const double expected = 1000.0 * board::kImuIsrDecimation;
    const double iv_mean = avg(s.iv_sum, s.iv_n);
    const double jitter = std::fmax(s.iv_max - iv_mean, iv_mean - u_min(s.iv_min));
    const GyroStats g = gyro_stats(s);
    const double mag = accel_mag(s);

    const bool ok_rate = std::fabs(rate - expected) <= kRateTolerance * expected;
    const bool ok_samples = s.missed == 0 && s.timeouts == 0 && s.i2c_errors == 0 &&
                            s.reads == s.wakes;
    const bool flag_lat = s.lat_max > kFlagUs;
    const bool flag_jit = jitter > kFlagUs;
    const bool flag_mag = mag < kAccelMagMin || mag > kAccelMagMax;
    bool flag_bias = false;
    for (int i = 0; i < 3; ++i) {
        flag_bias = flag_bias || std::fabs(g.mean[i]) > kGyroBiasMaxDps;
    }
    const bool ok_heap = heap_start == heap_end;

    ESP_LOGI(TAG, "SUMMARY B: DLPF_CFG=%d decimation %d | rate %.2f Hz | iv min/max/sd %u/%u/%.2f us,"
                  " jitter %.1f us | burst min/avg/max %u/%.1f/%u us | wake min/avg/max %u/%.1f/%u us"
                  " | wakes %u reads %u missed %u timeouts %u i2c_err %u | |a| %.4f g"
                  " | gyro bias %.3f/%.3f/%.3f sd %.3f/%.3f/%.3f dps"
                  " | ISR handler max %u cycles (%.2f us, our handler only, not the IDF GPIO dispatcher)"
                  " | Core 1 stack free %u B | heap start %u end %u B",
             board::kImuDlpfCfg, board::kImuIsrDecimation, rate, u_min(s.iv_min), u(s.iv_max),
             sdev(s.iv_sum, s.iv_sumsq, s.iv_n), jitter, u_min(s.burst_min),
             avg(s.burst_sum, s.reads), u(s.burst_max), u_min(s.lat_min), avg(s.lat_sum, s.wakes),
             u(s.lat_max), u(s.wakes), u(s.reads), u(s.missed), u(s.timeouts), u(s.i2c_errors), mag,
             g.mean[0], g.mean[1], g.mean[2], g.sd[0], g.sd[1], g.sd[2], u(s.isr_cycles_max),
             isr_us(s.isr_cycles_max), u(stack_free), u(heap_start), u(heap_end));
    ESP_LOGI(TAG, "SUMMARY B checks: rate %.0f +/- %.0f Hz %s | zero missed/timeouts/I2C errors and "
                  "one sample per notification %s | wake-up max %s | interval jitter %s | |a| in "
                  "%.2f..%.2f g %s | gyro bias within %.0f dps %s | heap unchanged %s",
             expected, expected * kRateTolerance, ok_rate ? "PASS" : "FAIL",
             ok_samples ? "PASS" : "FAIL", flag_lat ? "FLAG (> 200 us)" : "ok",
             flag_jit ? "FLAG (> 200 us)" : "ok", kAccelMagMin, kAccelMagMax,
             flag_mag ? "FLAG" : "ok", kGyroBiasMaxDps, flag_bias ? "FLAG" : "ok",
             ok_heap ? "PASS" : "FAIL");

    out->rate_hz = rate;
    out->iv_sd_us = sdev(s.iv_sum, s.iv_sumsq, s.iv_n);
    out->jitter_us = jitter;
    out->burst_avg_us = avg(s.burst_sum, s.reads);
    out->wake_max_us = s.lat_max;
    out->wakes = s.wakes;
    out->reads = s.reads;
    out->missed = s.missed;
    out->timeouts = s.timeouts;
    out->i2c_errors = s.i2c_errors;
}

void report_task(void*) {
    static Snapshot s;
    bool config_printed[4] = {false, false, false, false};
    bool summary_printed[kNumPhases] = {false, false, false};
    bool failure_printed = false;
    uint32_t last_window_seq = 0;
    bool heap_start_taken = false;
    uint32_t heap_start = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!read_snapshot(&s)) {
            continue;
        }

        for (int i = 0; i < 4; ++i) {
            if (s.config[i].valid && !config_printed[i]) {
                print_config(i, s.config[i].st);
                config_printed[i] = true;
            }
        }
        if (s.state == State::kFailed && !failure_printed) {
            if (s.bus_err != ESP_OK) {
                ESP_LOGE(TAG, "bus A init failed: %s. Test stopped.", esp_err_to_name(s.bus_err));
            }
            if (s.int_err != ESP_OK) {
                ESP_LOGE(TAG, "data-ready interrupt install failed: %s. Test stopped.",
                         esp_err_to_name(s.int_err));
            }
            failure_printed = true;
        }

        if (s.phase == kB && s.state == State::kRunning && !heap_start_taken) {
            heap_start = esp_get_free_heap_size();
            heap_start_taken = true;
        }

        if (s.window_seq != last_window_seq) {
            if (s.window_seq - last_window_seq > 1) {
                ESP_LOGW(TAG, "reporter missed %u window(s)", u(s.window_seq - last_window_seq - 1));
            }
            last_window_seq = s.window_seq;
            const uint8_t ph = s.window_phase;
            if (s_n_rates[ph] < kMaxWindows) {
                s_raw_rate[ph][s_n_rates[ph]] = rate_hz(s.window.raw_pulses, s.window.length_us);
                s_read_rate[ph][s_n_rates[ph]] = rate_hz(s.window.reads, s.window.length_us);
                s_n_rates[ph]++;
            }
            print_window(ph, s.window);
        }

        if (s.summary_valid[kA1] && !summary_printed[kA1]) {
            print_summary_a1(s.summary[kA1]);
            summary_printed[kA1] = true;
        }
        if (s.summary_valid[kA2] && !summary_printed[kA2]) {
            print_summary_a2(s.summary[kA2]);
            summary_printed[kA2] = true;
        }
        if (s.summary_valid[kB] && !summary_printed[kB]) {
            const uint32_t heap_end = esp_get_free_heap_size();
            const uint32_t stack_free = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_imu_task));
            print_summary_b(s.summary[kB], heap_start, heap_end, stack_free, &s_b_result);
            s_b_result_ready.store(true, std::memory_order_release);
            summary_printed[kB] = true;
        }

        if (s.state == State::kDone && summary_printed[kB]) {
            ESP_LOGI(TAG, "%s", s_b_only ? "M3 Phase B finished."
                                         : "M3 bench test finished. Reset the board for a fresh run.");
            vTaskDelete(nullptr);
        }
        if (s.state == State::kFailed && failure_printed) {
            vTaskDelete(nullptr);
        }
    }
}

}  // namespace

void m3_start() {
    ESP_LOGI(TAG, "M3 bench test: A1 %us, A2 %us, B %us at DLPF_CFG %d, decimation %d",
             u(kA1Seconds), u(kA2Seconds), u(kBSeconds), board::kImuDlpfCfg, board::kImuIsrDecimation);
    xTaskCreatePinnedToCore(report_task, "m3_report", kReporterStackBytes, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(imu_task, "imu", kImuTaskStackBytes, nullptr, configMAX_PRIORITIES - 2,
                            &s_imu_task, 1);
}

void m3_phase_b_start(uint32_t seconds) {
    s_b_only = true;
    s_b_seconds = seconds;
    ESP_LOGI(TAG, "M3 Phase B only: %us at DLPF_CFG %d, decimation %d", u(seconds),
             board::kImuDlpfCfg, board::kImuIsrDecimation);
    xTaskCreatePinnedToCore(report_task, "m3_report", kReporterStackBytes, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(imu_task, "imu", kImuTaskStackBytes, nullptr, configMAX_PRIORITIES - 2,
                            &s_imu_task, 1);
}

bool m3_phase_b_result(M3PhaseBResult* out) {
    if (!s_b_result_ready.load(std::memory_order_acquire)) {
        return false;
    }
    *out = s_b_result;
    return true;
}

#endif  // FE_M3_TEST || FE_M4_TEST
