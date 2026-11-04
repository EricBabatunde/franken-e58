#include "flight.h"

#include <atomic>
#include <cmath>

#include "board_config.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_mcpwm.h"
#include "seqlock.h"
#include "setpoint.h"

namespace flight {

namespace {

namespace sp = comms::setpoint;

constexpr int64_t kWindowUs = 10 * 1000 * 1000;
constexpr TickType_t kWaitTicks = 3;          // data-ready wait; see the missing-IMU logic below
constexpr int kSetpointTries = 8;             // seqlock attempts before using the last good setpoint
constexpr uint32_t kStatusEvery = 100;        // iterations between status publications (~100 ms)
constexpr uint32_t kStackBytes = 6144;
constexpr uint32_t kCpuMhz = 240;             // CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240
constexpr uint32_t kOverrunCycles = board::kLoopOverrunUs * kCpuMhz;

// ---- Hand-offs to Core 0 (single writer: the flight task).
SeqlockSlot<Status> s_status_slot;
SeqlockSlot<Window> s_window_slot;
SeqlockSlot<EventLog> s_event_slot;
std::atomic<uint32_t> s_window_seq{0};
StartReport s_start;                          // written before s_started is set
std::atomic<bool> s_started{false};
std::atomic<bool> s_int_request{true};
bool s_outputs_ok = false;

// ---- Core 1 state (flight task only). File-scope objects, constructed before app_main (no
// function-local statics: their thread-safe guard could take a lock).
safety::Machine machine(safety::Config{board::kLinkSilenceCutUs, board::kThrottleMinPermille,
                                       board::kThrottleMinHoldUs, board::kBenchDutyCapPermille});
safety::RestDetector rest(safety::RestConfig{
    board::kGyroCalSamples, board::kGyroCalSdMaxDps, board::kGyroCalRangeMaxDps, board::kGyroCalBiasMaxDps,
    board::kAccelCalNormSdMaxG, board::kAccelRefMinG, board::kAccelRefMaxG, board::kMotionBlockSamples,
    board::kMotionGyroDps, board::kMotionAccelFrac, board::kRestHoldUs, board::kAccelValidMinFrac,
    board::kAccelValidMaxFrac, std::cos(board::kTiltLimitDeg * 3.14159265f / 180.0f)});
Status s_st;
Window s_win;
EventLog s_log;

void keep_min(uint32_t& m, uint32_t v) { if (v < m) m = v; }
void keep_max(uint32_t& m, uint32_t v) { if (v > m) m = v; }

void take_isr_instrument(Window& w) {
#if FE_IMU_ISR_INSTRUMENT
    const drv_mpu6500::IsrInstrument in = drv_mpu6500::isr_instrument();
    w.raw_iv_min = in.raw_iv_min_us;
    w.raw_iv_max = in.raw_iv_max_us;
    w.isr_cycles_max = in.handler_cycles_max;
#else
    (void)w;
#endif
}

void publish_events() { s_event_slot.write(s_log); }

void record(const safety::Event& e, int64_t t_force_us, uint32_t force_cycles) {
    EventRecord& r = s_log.rec[(e.seq - 1) % kEventLog];
    r.ev = e;
    r.t_force_us = t_force_us;
    r.force_cycles = force_cycles;
    s_log.count = e.seq;
    switch (e.kind) {
        case safety::EventKind::kState:   if (e.to == safety::State::kArmed) s_st.arms++; break;
        case safety::EventKind::kRefusal: s_st.refusals++; break;
        case safety::EventKind::kCut:     s_st.cuts++; break;
        case safety::EventKind::kDisarm:  s_st.disarms++; break;
    }
}

[[noreturn]] void park() {
    for (;;) vTaskDelay(portMAX_DELAY);
}

void task(void*) {
    // ---- Init (Core 1): bus A, IMU, data-ready interrupt, as in M3.
    StartReport& sr = s_start;
    sr.bus_err = hal_i2c::init(hal_i2c::Bus::A, board::kImuSda, board::kImuScl, board::kI2cHz);
    sr.bus_lines = hal_i2c::line_check(hal_i2c::Bus::A);
    if (sr.bus_err == ESP_OK) {
        sr.init = drv_mpu6500::init(hal_i2c::Bus::A);
        if (sr.init.step == drv_mpu6500::Step::kNone) {
            sr.int_err = drv_mpu6500::int_install(xTaskGetCurrentTaskHandle());
        }
    }
    sr.ok = sr.bus_err == ESP_OK && sr.init.step == drv_mpu6500::Step::kNone && sr.int_err == ESP_OK;
    sr.stack_free = uxTaskGetStackHighWaterMark(nullptr);
    sr.done = true;
    s_started.store(true, std::memory_order_release);
    if (!sr.ok) park();  // outputs stay forced low; the machine stays in BOOT

    const float gyro_scale = drv_mpu6500::gyro_dps(1);
    const float accel_scale = drv_mpu6500::accel_g(1);

    safety::Outputs out;
    safety::Inputs in;
    if (s_outputs_ok) {
        machine.begin(esp_timer_get_time(), &out);
        for (int i = 0; i < out.n_events; ++i) record(out.events[i], 0, 0);
        publish_events();
    }
    safety::publish_state(machine.state());

    // ---- Loop. After this point: no logging, no allocation, no lock shared with Core 0.
    drv_mpu6500::isr_set_notify_every(board::kImuIsrDecimation);
    drv_mpu6500::isr_instrument_reset();
    ulTaskNotifyTake(pdTRUE, 0);
    int64_t t_win = esp_timer_get_time();
    uint32_t raw_mark = drv_mpu6500::raw_pulses();
    uint32_t index = 0;
    uint32_t prev_irq = 0;
    bool have_prev = false;
    TickType_t wait = kWaitTicks;
    uint32_t good_reads = 0;
    bool forced = true;        // hal_mcpwm::init() leaves every generator forced low
    uint16_t duty = 0;
    bool int_on = true;
    uint32_t since_status = 0;
    sp::Setpoint last_good[sp::kTransports] = {};
    bool have_good[sp::kTransports] = {false, false};
    drv_mpu6500::RawSample sample{};

    for (;;) {
        const int64_t now0 = esp_timer_get_time();
        if (now0 - t_win >= kWindowUs) {
            const uint32_t raw = drv_mpu6500::raw_pulses();
            s_win.index = ++index;
            s_win.length_us = static_cast<uint32_t>(now0 - t_win);
            s_win.raw_pulses = raw - raw_mark;
            raw_mark = raw;
            take_isr_instrument(s_win);
            s_window_slot.write(s_win);
            // Single writer: a load and a store, not a read-modify-write (no __atomic_fetch_add call).
            s_window_seq.store(s_window_seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
            s_win = Window{};
            t_win = now0;
        }

        // Data-ready wait. With a 1 ms tick, a 3-tick timeout can expire 2 to 3 ms after the last
        // pulse, so a timeout only cuts once more than kImuMissingCutUs have really passed since the
        // last ISR timestamp; until then (and until data returns) the wait is one tick at a time.
        const uint32_t n = ulTaskNotifyTake(pdTRUE, wait);
        const int64_t woke64 = esp_timer_get_time();
        const uint32_t woke = static_cast<uint32_t>(woke64);
        const uint32_t c_wake = esp_cpu_get_cycle_count();
        s_win.iterations++;

        if (n > 0) {
            wait = kWaitTicks;
            s_win.wakes++;
            if (n > 1) s_win.missed += n - 1;
            const uint32_t t_irq = drv_mpu6500::last_notify_us();
            const uint32_t lat = woke - t_irq;
            keep_min(s_win.lat_min, lat);
            keep_max(s_win.lat_max, lat);
            s_win.lat_sum += lat;
            if (have_prev) {
                const uint32_t iv = t_irq - prev_irq;
                s_win.iv_n++;
                keep_min(s_win.iv_min, iv);
                keep_max(s_win.iv_max, iv);
                s_win.iv_sum += iv;
                s_win.iv_sumsq += static_cast<uint64_t>(iv) * iv;
            }
            prev_irq = t_irq;
            have_prev = true;

            const uint32_t t0 = static_cast<uint32_t>(esp_timer_get_time());
            const esp_err_t e = drv_mpu6500::read_burst(t_irq, &sample);
            const uint32_t dur = static_cast<uint32_t>(esp_timer_get_time()) - t0;
            if (e != ESP_OK) {
                s_win.i2c_errors++;
                s_st.i2c_errors++;
                good_reads = 0;
            } else {
                s_win.reads++;
                keep_min(s_win.burst_min, dur);
                keep_max(s_win.burst_max, dur);
                s_win.burst_sum += dur;
                if (static_cast<int32_t>(t_irq - drv_mpu6500::gyro_valid_from_us()) >= 0) {
                    const float g[3] = {sample.gyro[0] * gyro_scale, sample.gyro[1] * gyro_scale,
                                        sample.gyro[2] * gyro_scale};
                    const float a[3] = {sample.accel[0] * accel_scale, sample.accel[1] * accel_scale,
                                        sample.accel[2] * accel_scale};
                    rest.sample(woke64, g, a);
                    if (good_reads < board::kImuHealthySamples) good_reads++;
                    s_win.samples++;
                    s_win.accel_sq_sum += a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
                    for (int i = 0; i < 3; ++i) {
                        const int64_t raw = sample.gyro[i];
                        s_win.gyro_sum[i] += raw;
                        s_win.gyro_sumsq[i] += static_cast<uint64_t>(raw * raw);
                        s_st.accel[i] = a[i];
                    }
                }
            }
        } else {
            s_win.timeouts++;
            have_prev = false;
            wait = 1;
        }
        const uint32_t last_pulse = drv_mpu6500::last_notify_us();
        const uint32_t since = woke - last_pulse;
        const bool missing = since > board::kImuMissingCutUs;
        if (missing) good_reads = 0;

        // Bench fault injection request from Core 0 (atomic, no lock).
        const bool want_int = s_int_request.load(std::memory_order_acquire);
        if (want_int != int_on) {
            drv_mpu6500::int_enable(want_int);
            int_on = want_int;
        }

        // Freshest setpoint over both transports. On a give-up, the last good one.
        bool have_sp = false;
        sp::Setpoint best{};
        for (int t = 0; t < sp::kTransports; ++t) {
            const SeqlockSlot<sp::Setpoint>& slot = sp::slot(static_cast<sp::Transport>(t));
            if (slot.writes() == 0) continue;
            sp::Setpoint s;
            int tries = 0;
            const bool ok = slot.read(&s, kSetpointTries, &tries);
            s_win.sp_reads++;
            keep_max(s_win.sp_retries_max, static_cast<uint32_t>(tries - 1));
            if (!ok) {
                s_win.sp_giveups++;
                s_st.sp_giveups++;
            } else if (s.check != sp::check_word(s)) {
                s_win.sp_torn++;
                s_st.sp_torn++;
            } else {
                last_good[t] = s;
                have_good[t] = true;
            }
            if (have_good[t] && (!have_sp || last_good[t].t_rx_us > best.t_rx_us)) {
                best = last_good[t];
                have_sp = true;
            }
        }

        in.now_us = woke64;
        in.have_setpoint = have_sp;
        in.sp_rx_us = best.t_rx_us;
        in.sp_transport = best.transport;
        in.throttle = best.throttle;
        in.flags = best.flags;
        in.imu_missing = missing;
        in.imu_last_us = woke64 - static_cast<int64_t>(since);
        in.imu_healthy = good_reads >= board::kImuHealthySamples && !missing;
        in.gyro_cal_done = rest.cal_done();
        in.at_rest = rest.at_rest(woke64);
        in.accel_ok = rest.accel_ok();
        in.tilt_ok = rest.tilt_ok();

        if (s_outputs_ok) {
            machine.step(in, &out);
        } else {
            out = safety::Outputs{};
        }
        const uint32_t c_decided = esp_cpu_get_cycle_count();

        // Outputs. A cut or disarm always writes the force, and the time to it is recorded.
        bool must_force = false;
        for (int i = 0; i < out.n_events; ++i) {
            const safety::EventKind k = out.events[i].kind;
            must_force = must_force || k == safety::EventKind::kCut || k == safety::EventKind::kDisarm;
        }
        int64_t t_force = 0;
        uint32_t force_cyc = 0;
        if (out.force_low) {
            if (!forced || must_force) {
                hal_mcpwm::force_low_all();
                force_cyc = esp_cpu_get_cycle_count() - c_decided;
                t_force = esp_timer_get_time();
                forced = true;
            }
            if (duty != 0) {
                hal_mcpwm::set_all_permille(0);
                duty = 0;
            }
        } else {
            if (out.duty_permille != duty) {
                hal_mcpwm::set_all_permille(out.duty_permille);
                duty = out.duty_permille;
            }
            if (forced) {
                hal_mcpwm::release_all();
                forced = false;
            }
        }
        safety::publish_state(out.state);
        if (out.state == safety::State::kArmed) s_win.armed_iterations++;

        for (int i = 0; i < out.n_events; ++i) {
            const bool forcing = out.events[i].kind == safety::EventKind::kCut ||
                                 out.events[i].kind == safety::EventKind::kDisarm;
            record(out.events[i], forcing ? t_force : 0, forcing ? force_cyc : 0);
        }
        if (out.n_events > 0) publish_events();

        const uint32_t work = esp_cpu_get_cycle_count() - c_wake;
        keep_min(s_win.work_cyc_min, work);
        keep_max(s_win.work_cyc_max, work);
        s_win.work_cyc_sum += work;
        if (work > kOverrunCycles) {
            s_win.overruns++;
            s_st.overruns++;
        }
        keep_max(s_st.sp_retries_max, s_win.sp_retries_max);

        if (++since_status >= kStatusEvery || out.n_events > 0) {
            since_status = 0;
            s_st.publish_seq++;
            s_st.t_us = woke64;
            s_st.state = out.state;
            s_st.latched = out.latched;
            s_st.last_cut = machine.last_cut();
            s_st.blocking = out.blocking;
            s_st.have_sp = have_sp;
            s_st.sp_age_us = have_sp ? woke64 - best.t_rx_us : 0;
            s_st.sp_transport = best.transport;
            s_st.sp_seq = best.seq;
            s_st.throttle = best.throttle;
            s_st.flags = best.flags;
            s_st.duty_permille = duty;
            s_st.forced_low = hal_mcpwm::all_forced_low();
            s_st.imu_healthy = in.imu_healthy;
            s_st.imu_missing = missing;
            s_st.int_enabled = int_on;
            s_st.since_pulse_us = since;
            s_st.cal_done = rest.cal_done();
            s_st.cal_progress = rest.cal_progress();
            s_st.cal_restarts = rest.cal_restarts();
            for (int i = 0; i < 3; ++i) s_st.bias[i] = rest.bias()[i];
            s_st.g0 = rest.g0();
            s_st.cal_sd = rest.cal_sd_max();
            s_st.ref_ok = rest.ref_ok();
            s_st.at_rest = in.at_rest;
            s_st.accel_ok = in.accel_ok;
            s_st.tilt_ok = in.tilt_ok;
            s_st.flash_ok = safety::flash_writes_allowed();
            s_status_slot.write(s_st);
        }
    }
}

}  // namespace

StartReport start(bool outputs_ok) {
    s_outputs_ok = outputs_ok;
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(task, "flight", kStackBytes, nullptr, configMAX_PRIORITIES - 2, &h, 1);
    for (int i = 0; i < 300 && !s_started.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    StartReport r;
    if (s_started.load(std::memory_order_acquire)) r = s_start;
    return r;
}

bool status(Status* out) { return s_status_slot.read(out); }

bool window(Window* out, uint32_t* seq) {
    *seq = s_window_seq.load(std::memory_order_acquire);
    return s_window_slot.read(out);
}

bool events(EventLog* out) { return s_event_slot.read(out); }

void request_imu_int(bool enabled) { s_int_request.store(enabled, std::memory_order_release); }

}  // namespace flight
