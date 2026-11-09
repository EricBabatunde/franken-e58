#include "flight.h"

#include <atomic>
#include <cmath>
#include <cstdlib>

#include "board_config.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_mcpwm.h"
#include "sdkconfig.h"
#include "seqlock.h"
#include "setpoint.h"
#include "wdt.h"

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
SeqlockSlot<RecoveryLog> s_recovery_slot;
std::atomic<uint32_t> s_window_seq{0};
StartReport s_start;                          // written before s_started is set
std::atomic<bool> s_started{false};
std::atomic<bool> s_int_request{true};
std::atomic<uint8_t> s_fault_request{0};      // Fault, written by Core 0, cleared by the flight task
std::atomic<bool> s_boot_request{false};
bool s_outputs_ok = false;
// M7c timing (bench builds). Requests from Core 0 are counters and a value: plain loads and stores,
// one writer each (no read-modify-write on Core 1).
std::atomic<uint32_t> s_tm_epoch_req{0};
std::atomic<uint32_t> s_tm_pub_req{0};
std::atomic<uint32_t> s_load_req{0};
#if CONFIG_FE_LATENCY_HIST
SeqlockSlot<Timing> s_timing_slot;
Timing s_tm;                                  // Core 1 running set (static: about 3 KB)
#endif

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
RecoveryLog s_rec;

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

EventRecord& record(const safety::Event& e, int64_t t_force_us, uint32_t force_cycles) {
    EventRecord& r = s_log.rec[(e.seq - 1) % kEventLog];
    r = EventRecord{};
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
    return r;
}

void record_all(const safety::Outputs& o) {
    for (int i = 0; i < o.n_events; ++i) record(o.events[i], 0, 0);
    if (o.n_events > 0) publish_events();
}

#if CONFIG_FE_LATENCY_HIST
// New condition epoch: every counter and histogram to zero, in place (no 3 KB temporary on the
// flight task's stack).
void timing_clear(Timing& t, uint32_t epoch, int64_t now_us, uint32_t load_us) {
    t.epoch = epoch;
    t.t_first_us = now_us;
    t.t_last_us = now_us;
    t.iterations = t.wakes = t.missed = t.timeouts = t.i2c_errors = t.overruns = t.accel_stale = 0;
    t.load_us = load_us;
    t.load_changes = 0;
    t.period_dev_min = 0;
    t.period_dev_max = 0;
    t.period.clear();
    t.wake.clear();
    t.burst14.clear();
    t.burst6.clear();
    t.work.clear();
}
#endif

[[noreturn]] void park() {
    for (;;) vTaskDelay(portMAX_DELAY);
}

// M7b console fault injection. Runs between the safety decision and the output write, so a
// resumed stall exercises the hard latch in the middle of an output update.
void run_fault(Fault f) {
    switch (f) {
        case Fault::kStallShort: {
            const int64_t t0 = esp_timer_get_time();
            while (esp_timer_get_time() - t0 < board::kFaultStallShortUs) {
            }
            break;
        }
        case Fault::kStallMasked: {
            // Interrupts masked on Core 1 (up to the FreeRTOS critical level): the GPTimer watchdog
            // cannot run, the interrupt watchdog (CPU1 tick) panics and the panic wrapper forces low.
            const UBaseType_t m = portSET_INTERRUPT_MASK_FROM_ISR();
            const int64_t t0 = esp_timer_get_time();  // reads the systimer: works with interrupts masked
            while (esp_timer_get_time() - t0 < board::kFaultStallMaskedUs) {
            }
            portCLEAR_INTERRUPT_MASK_FROM_ISR(m);
            break;
        }
        case Fault::kStallLong: {
            const int64_t t0 = esp_timer_get_time();
            while (esp_timer_get_time() - t0 < board::kFaultStallLongUs) {
            }
            break;
        }
        case Fault::kAbort:
            std::abort();  // deliberate panic: the wrapper forces low, then the IDF handler prints
        default:
            break;
    }
}

// M7b boot: IMU init, retried when board::kImuBootRetry (bus reset, best-effort DEVICE_RESET,
// wait, full sequence). The outputs were forced low by hal_mcpwm::init() before this task
// started, and the machine cannot leave BOOT until this returns success.
void imu_boot(StartReport& sr) {
    sr.bus_err = hal_i2c::init(hal_i2c::Bus::A, board::kImuSda, board::kImuScl, board::kI2cHz, board::kImuBootRetry);
    sr.bus_lines = hal_i2c::line_check(hal_i2c::Bus::A);
    if (sr.bus_err != ESP_OK) return;
    const uint32_t n = board::kImuBootRetry ? board::kImuBootAttempts : 1;
    for (uint32_t a = 0; a < n && a < 3; ++a) {
        if (a > 0) {
            sr.retry_bus_reset_err[a] = hal_i2c::bus_reset(hal_i2c::Bus::A);
            sr.retry_soft_reset_err[a] = drv_mpu6500::soft_reset();
            vTaskDelay(pdMS_TO_TICKS(board::kImuBootRetryWaitMs) + 1);
        }
        sr.init = drv_mpu6500::init(hal_i2c::Bus::A);
        sr.attempt[a] = sr.init;
        sr.attempts = a + 1;
        if (sr.init.step == drv_mpu6500::Step::kNone) return;
    }
}

void task(void*) {
    // ---- Init (Core 1): bus A, IMU, data-ready interrupt (as in M3), then the watchdog, whose
    // interrupt is allocated here, on Core 1.
    StartReport& sr = s_start;
    imu_boot(sr);
    if (sr.bus_err == ESP_OK && sr.init.step == drv_mpu6500::Step::kNone) {
        sr.int_err = drv_mpu6500::int_install(xTaskGetCurrentTaskHandle());
    }
    const bool imu_ok = sr.bus_err == ESP_OK && sr.init.step == drv_mpu6500::Step::kNone && sr.int_err == ESP_OK;
    if (imu_ok) sr.wdt = wdt::start();
    sr.ok = imu_ok && sr.wdt.err == ESP_OK;
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
        record_all(out);
    }
    safety::publish_state(machine.state());

    // ---- Loop. After this point: no logging, no allocation, no lock shared with Core 0.
    drv_mpu6500::isr_set_notify_every(board::kImuIsrDecimation);
    drv_mpu6500::isr_instrument_reset();
    ulTaskNotifyTake(pdTRUE, 0);
    int64_t t_win = esp_timer_get_time();
    uint32_t raw_mark = drv_mpu6500::raw_pulses();
    (void)wdt::take_window();
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
    // Watchdog alive check.
    uint32_t wd_count_seen = wdt::isr_count();
    int64_t wd_change_us = esp_timer_get_time();
    // IMU recovery episode.
    uint32_t rec_episode = 0;
    int64_t rec_last_us = 0;
    bool rec_exhausted = false;
    // M7c: accelerometer age (every build) and the R5 (d) schedule.
    int64_t accel_t_us = 0;          // wake time of the last good full (14-byte) read; 0: none yet
#if CONFIG_FE_R5_GYRO_ONLY
    uint32_t since_full = 0;         // data frames since the last good full read
#endif
#if CONFIG_FE_LATENCY_HIST
    uint32_t prev_wake = 0;
#endif
#if CONFIG_FE_LATENCY_HIST
    timing_clear(s_tm, s_tm_epoch_req.load(std::memory_order_acquire), esp_timer_get_time(), 0);
    int64_t t_tm_pub = esp_timer_get_time();
    const uint32_t cyc_per_us = kCpuMhz;
#endif

    for (;;) {
        const int64_t now0 = esp_timer_get_time();
#if CONFIG_FE_LATENCY_HIST
        // Publish before the wait (outside the measured work): every second, or at once on request.
        const uint32_t pub = s_tm_pub_req.load(std::memory_order_acquire);
        if (pub != s_tm.publish_ack || now0 - t_tm_pub >= static_cast<int64_t>(board::kTimingPublishUs)) {
            s_tm.publish_ack = pub;
            s_timing_slot.write(s_tm);
            t_tm_pub = now0;
        }
#endif
        if (now0 - t_win >= kWindowUs) {
            const uint32_t raw = drv_mpu6500::raw_pulses();
            s_win.index = ++index;
            s_win.length_us = static_cast<uint32_t>(now0 - t_win);
            s_win.raw_pulses = raw - raw_mark;
            raw_mark = raw;
            take_isr_instrument(s_win);
            s_win.wd = wdt::take_window();
            s_window_slot.write(s_win);
            // Single writer: a load and a store, not a read-modify-write (no __atomic_fetch_add call).
            s_window_seq.store(s_window_seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
            s_win = Window{};
            t_win = now0;
        }

        // Data-ready wait. With a 1 ms tick, a 3-tick timeout can expire 2 to 3 ms after the last
        // pulse; until data returns the wait is one tick at a time. The heartbeat is stored on every
        // wake-up, data or timeout, so its gap stays below 3 ms plus one tick (threshold 5 ms).
        const uint32_t n = ulTaskNotifyTake(pdTRUE, wait);
        const int64_t woke64 = esp_timer_get_time();
        const uint32_t woke = static_cast<uint32_t>(woke64);
        wdt::heartbeat(woke);
        const uint32_t c_wake = esp_cpu_get_cycle_count();
        s_win.iterations++;
#if CONFIG_FE_LATENCY_HIST
        const uint32_t load_us = s_load_req.load(std::memory_order_relaxed);
        {
            const uint32_t ep = s_tm_epoch_req.load(std::memory_order_acquire);
            if (ep != s_tm.epoch) timing_clear(s_tm, ep, woke64, load_us);
            if (load_us != s_tm.load_us) {
                s_tm.load_changes++;
                s_tm.load_us = load_us;
            }
            s_tm.iterations++;
            s_tm.t_last_us = woke64;
        }
#endif

        if (n > 0) {
            wait = kWaitTicks;
            s_win.wakes++;
            if (n > 1) s_win.missed += n - 1;
#if CONFIG_FE_LATENCY_HIST
            s_tm.wakes++;
            if (n > 1) s_tm.missed += n - 1;
#endif
            const uint32_t t_irq = drv_mpu6500::last_notify_us();
            // A pulse that arrived after the wake stamp gives a negative difference: clamp to 0.
            const int32_t lat_s = static_cast<int32_t>(woke - t_irq);
            const uint32_t lat = lat_s > 0 ? static_cast<uint32_t>(lat_s) : 0;
            keep_min(s_win.lat_min, lat);
            keep_max(s_win.lat_max, lat);
            s_win.lat_sum += lat;
#if CONFIG_FE_LATENCY_HIST
            s_tm.wake.add(lat);
            if (have_prev) {  // consecutive data wake-ups: loop period
                const int32_t dev = static_cast<int32_t>(woke - prev_wake) - static_cast<int32_t>(board::kNominalPeriodUs);
                s_tm.period.add(static_cast<uint32_t>(dev < 0 ? -dev : dev));
                if (s_tm.period.count == 1 || dev < s_tm.period_dev_min) s_tm.period_dev_min = dev;
                if (s_tm.period.count == 1 || dev > s_tm.period_dev_max) s_tm.period_dev_max = dev;
            }
            prev_wake = woke;
#endif
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

            // Full 14-byte burst (accelerometer, temperature, gyro of one sample) every frame, or with
            // CONFIG_FE_R5_GYRO_ONLY every board::kR5AccelDivider frames and the 6-byte gyro read
            // otherwise; a failed full read is repeated on the next frame. `sample` keeps the last
            // accelerometer value between full reads; its age is checked below.
#if CONFIG_FE_R5_GYRO_ONLY
            const bool full = accel_t_us == 0 || since_full + 1 >= board::kR5AccelDivider;
#else
            const bool full = true;
#endif
            const uint32_t t0 = static_cast<uint32_t>(esp_timer_get_time());
            const esp_err_t e = full ? drv_mpu6500::read_burst(t_irq, &sample) : drv_mpu6500::read_gyro(t_irq, &sample);
            const uint32_t dur = static_cast<uint32_t>(esp_timer_get_time()) - t0;
            if (e != ESP_OK) {
                s_win.i2c_errors++;
                s_st.i2c_errors++;
                good_reads = 0;
#if CONFIG_FE_LATENCY_HIST
                s_tm.i2c_errors++;
#endif
            } else {
                if (full) accel_t_us = woke64;
#if CONFIG_FE_R5_GYRO_ONLY
                since_full = full ? 0 : since_full + 1;
#endif
#if CONFIG_FE_LATENCY_HIST
                if (full) {
                    s_tm.burst14.add(dur);
                } else {
                    s_tm.burst6.add(dur);
                }
#endif
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
#if CONFIG_FE_LATENCY_HIST
            // Synthetic compute load after the IMU read (board::kSyntheticLoadUs, key L): a busy-wait
            // on the cycle counter standing in for filters, attitude, PID and mixer.
            if (load_us != 0) {
                const uint32_t c_load = esp_cpu_get_cycle_count();
                while (esp_cpu_get_cycle_count() - c_load < load_us * cyc_per_us) {
                }
            }
#endif
        } else {
            s_win.timeouts++;
            have_prev = false;
            wait = 1;
#if CONFIG_FE_LATENCY_HIST
            s_tm.timeouts++;
#endif
        }
        // Accelerometer age (M7c addition 1): stale after board::kAccelStaleUs without a good full
        // read; the rest, tilt and accelerometer arm checks then fail.
        const int64_t accel_age = accel_t_us == 0 ? INT64_MAX : woke64 - accel_t_us;
        const bool accel_stale = accel_age > static_cast<int64_t>(board::kAccelStaleUs);
#if CONFIG_FE_LATENCY_HIST
        if (accel_stale) s_tm.accel_stale++;
#endif
        const uint32_t last_pulse = drv_mpu6500::last_notify_us();
        // The pulse is read after the burst read, so a newer pulse than the wake stamp is possible
        // (late wake): that is not missing, and the unsigned difference would wrap to ~4295 s.
        const int32_t since_s = static_cast<int32_t>(woke - last_pulse);
        const uint32_t since = since_s > 0 ? static_cast<uint32_t>(since_s) : 0;
        const bool missing = since > board::kImuMissingCutUs;           // health and refusals (3 ms)
        const bool missing_cut = since > board::kImuMissingTickCutUs;   // backup cut (6 ms)
        if (missing) good_reads = 0;
        if (!missing && good_reads >= board::kImuHealthySamples) rec_episode = 0;  // episode over

        // Bench fault injection requests from Core 0 (atomics, no lock).
        const bool want_int = s_int_request.load(std::memory_order_acquire);
        if (want_int != int_on) {
            drv_mpu6500::int_enable(want_int);
            int_on = want_int;
        }
        const Fault fault = static_cast<Fault>(s_fault_request.load(std::memory_order_acquire));
        if (fault != Fault::kNone) s_fault_request.store(0, std::memory_order_relaxed);
        if (fault == Fault::kImuSleep) (void)drv_mpu6500::sleep();

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

        // Watchdog alive: its alarm count must advance. Checked against the time of the last change,
        // so a long block of this task (an IMU recovery) does not count as a dead watchdog.
        const uint32_t wd_count = wdt::isr_count();
        if (wd_count != wd_count_seen) {
            wd_count_seen = wd_count;
            wd_change_us = woke64;
        }
        const bool wd_alive = woke64 - wd_change_us <= static_cast<int64_t>(board::kWdtAliveUs);
        const uint8_t hard = hal_mcpwm::hard_latched();

        in.now_us = woke64;
        in.have_setpoint = have_sp;
        in.sp_rx_us = best.t_rx_us;
        in.sp_transport = best.transport;
        in.throttle = best.throttle;
        in.flags = best.flags;
        in.imu_missing = missing;
        in.imu_missing_cut = missing_cut;
        in.imu_last_us = woke64 - static_cast<int64_t>(since);
        in.imu_healthy = good_reads >= board::kImuHealthySamples && !missing;
        in.gyro_cal_done = rest.cal_done();
        in.at_rest = rest.at_rest(woke64) && !accel_stale;
        in.accel_ok = rest.accel_ok() && !accel_stale;
        in.tilt_ok = rest.tilt_ok() && !accel_stale;
        in.hard_cut = hard;
        in.wd_alive = wd_alive;

        if (s_outputs_ok) {
            machine.step(in, &out);
        } else {
            out = safety::Outputs{};
        }
        const uint32_t c_decided = esp_cpu_get_cycle_count();

        // The hard latch is cleared ONLY here, in the iteration where the machine enters ARMED (the
        // normal arm sequence: throttle low plus a fresh arm edge), before the outputs are released.
        bool must_force = false;
        bool entered_armed = false;
        for (int i = 0; i < out.n_events; ++i) {
            const safety::EventKind k = out.events[i].kind;
            must_force = must_force || k == safety::EventKind::kCut || k == safety::EventKind::kDisarm;
            entered_armed = entered_armed || (k == safety::EventKind::kState && out.events[i].to == safety::State::kArmed);
        }
        if (entered_armed) hal_mcpwm::clear_hard_cut();

        // Fault stalls: after the decision, before the write.
        if (fault != Fault::kNone && fault != Fault::kImuSleep) run_fault(fault);

        // Outputs. A cut or disarm always writes the force, and the time to it is recorded. While the
        // hard latch is set, release_all() refuses and set_all_permille() writes 0 (output_gate.h).
        if (hal_mcpwm::hard_latched() != 0) forced = true;  // the ISR forced every generator low
        int64_t t_force = 0;
        uint32_t force_cyc = 0;
        if (out.force_low) {
            if (!forced || must_force) {
                hal_mcpwm::force_low_all();
                force_cyc = esp_cpu_get_cycle_count() - c_decided;
                t_force = esp_timer_get_time();
                forced = true;
            }
            if (duty != 0) duty = hal_mcpwm::set_all_permille(0);
        } else {
            if (out.duty_permille != duty) duty = hal_mcpwm::set_all_permille(out.duty_permille);
            if (forced) forced = !hal_mcpwm::release_all();
        }
        safety::publish_state(out.state);
        if (out.state == safety::State::kArmed) s_win.armed_iterations++;

        for (int i = 0; i < out.n_events; ++i) {
            const safety::Event& ev = out.events[i];
            const bool forcing = ev.kind == safety::EventKind::kCut || ev.kind == safety::EventKind::kDisarm;
            EventRecord& r = record(ev, forcing ? t_force : 0, forcing ? force_cyc : 0);
            if (ev.kind == safety::EventKind::kCut &&
                (ev.cut == safety::Cut::kWdHeartbeat || ev.cut == safety::Cut::kWdImuMissing)) {
                const wdt::Trip tr = wdt::last_trip();
                r.wd_decide_us = tr.t_decide_us;
                r.wd_ref_us = tr.ref_us;
                r.wd_force_cycles = tr.force_cycles;
            }
        }
        if (out.n_events > 0) publish_events();

        // ---- IMU recovery: SAFE or DISARMED only, never while ARMED (a missing IMU while ARMED is cut
        // by the watchdog first; recovery waits for DISARMED). Suppressed while 'm' has the interrupt
        // disabled on purpose. Bounded: board::kRecoveryAttempts per episode, board::kRecoveryBackoffUs
        // apart; then Core 0 decides a controlled restart or BOOT.
        if (s_outputs_ok && safety::recovery_allowed(machine.state()) && int_on && !rec_exhausted &&
            since > board::kRecoveryMissingUs && (rec_episode == 0 || woke64 - rec_last_us >= board::kRecoveryBackoffUs)) {
            RecoveryRecord rr;
            rr.seq = s_rec.count + 1;
            rr.t_us = woke64;
            rr.state = machine.state();
            rr.attempt = ++rec_episode;
            rr.missing_us = since;
            const int64_t t0 = esp_timer_get_time();
            rr.bus_reset_err = hal_i2c::bus_reset(hal_i2c::Bus::A);
            const drv_mpu6500::InitStatus st = drv_mpu6500::init(hal_i2c::Bus::A);  // blocks ~100 ms
            rr.duration_us = static_cast<uint32_t>(esp_timer_get_time() - t0);
            rr.step = st.step;
            rr.init_err = st.err;
            rr.who_am_i = st.who_am_i;
            rr.ok = st.step == drv_mpu6500::Step::kNone;
            rec_last_us = esp_timer_get_time();
            s_rec.rec[(rr.seq - 1) % kRecoveryLog] = rr;
            s_rec.count = rr.seq;
            s_recovery_slot.write(s_rec);
            s_st.rec_attempts++;
            if (rr.ok) {
                s_st.rec_ok++;
                rest.reset();
                good_reads = 0;
                have_prev = false;
                ulTaskNotifyTake(pdTRUE, 0);
                wait = kWaitTicks;
                machine.to_safe(rec_last_us, &out);  // back through the gyro calibration
                record_all(out);
                safety::publish_state(machine.state());
            } else if (rec_episode >= board::kRecoveryAttempts) {
                rec_exhausted = true;
            }
        }
        if (s_boot_request.load(std::memory_order_acquire) && machine.state() != safety::State::kBoot) {
            if (machine.to_boot(esp_timer_get_time(), &out)) {
                record_all(out);
                safety::publish_state(machine.state());
            }
        }

        const uint32_t work = esp_cpu_get_cycle_count() - c_wake;
        keep_min(s_win.work_cyc_min, work);
        keep_max(s_win.work_cyc_max, work);
        s_win.work_cyc_sum += work;
        if (work > kOverrunCycles) {
            s_win.overruns++;
            s_st.overruns++;
#if CONFIG_FE_LATENCY_HIST
            s_tm.overruns++;
#endif
        }
#if CONFIG_FE_LATENCY_HIST
        s_tm.work.add(work / cyc_per_us);
#endif
        keep_max(s_st.sp_retries_max, s_win.sp_retries_max);

        if (++since_status >= kStatusEvery || out.n_events > 0) {
            since_status = 0;
            s_st.publish_seq++;
            s_st.t_us = woke64;
            s_st.state = machine.state();
            s_st.latched = machine.latched();
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
            s_st.hard_latched = hal_mcpwm::hard_latched();
            s_st.wd_alive = wd_alive;
            s_st.wd_count = wd_count;
            s_st.rec_episode = rec_episode;
            s_st.rec_exhausted = rec_exhausted;
            s_st.accel_stale = accel_stale;
            s_st.accel_age_us = accel_age > static_cast<int64_t>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(accel_age);
#if CONFIG_FE_LATENCY_HIST
            s_st.load_us = load_us;
#endif
            s_status_slot.write(s_st);
        }
    }
}

}  // namespace

StartReport start(bool outputs_ok) {
    s_outputs_ok = outputs_ok;
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(task, "flight", kStackBytes, nullptr, configMAX_PRIORITIES - 2, &h, 1);
    // Boot retry: up to 3 attempts of the IMU sequence (~110 ms each) plus 100 ms waits.
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

bool recovery_log(RecoveryLog* out) { return s_recovery_slot.read(out); }

void request_imu_int(bool enabled) { s_int_request.store(enabled, std::memory_order_release); }

void request_fault(Fault f) { s_fault_request.store(static_cast<uint8_t>(f), std::memory_order_release); }

void request_boot_state() { s_boot_request.store(true, std::memory_order_release); }

// M7c timing requests (Core 0 is the only writer of each counter).
uint32_t timing_reset() {
    const uint32_t e = s_tm_epoch_req.load(std::memory_order_relaxed) + 1;
    s_tm_epoch_req.store(e, std::memory_order_release);
    return e;
}

uint32_t timing_publish() {
    const uint32_t p = s_tm_pub_req.load(std::memory_order_relaxed) + 1;
    s_tm_pub_req.store(p, std::memory_order_release);
    return p;
}

void request_load(uint32_t load_us) { s_load_req.store(load_us, std::memory_order_relaxed); }

#if CONFIG_FE_LATENCY_HIST
bool timing(Timing* out) { return s_timing_slot.read(out); }
bool timing_enabled() { return true; }
#else
bool timing(Timing*) { return false; }
bool timing_enabled() { return false; }
#endif

}  // namespace flight
