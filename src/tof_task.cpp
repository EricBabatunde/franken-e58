#include "tof_task.h"

#include <atomic>

#include "board_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_i2c.h"
#include "seqlock.h"

namespace {

constexpr uint32_t kStackBytes = 4096;
constexpr UBaseType_t kPriority = 6;
// Sleep this long after a reading before polling data-ready again (period minus 5 ms).
constexpr uint32_t kSleepAfterReadMs = board::kTofPeriodMs - 5;
constexpr uint32_t kNoDataUs = board::kTofNoDataPeriods * board::kTofPeriodMs * 1000;

SeqlockSlot<TofSample> s_sample;
SeqlockSlot<TofStatus> s_status;
std::atomic<bool> s_reset_request{false};

TofStatus s_st{};       // tof_task only
uint32_t s_seq = 0;     // tof_task only

uint32_t now_us() { return static_cast<uint32_t>(esp_timer_get_time()); }

void publish_sample(const drv_vl53l1x::Reading* r) {
    TofSample out{};
    out.seq = ++s_seq;
    if (r == nullptr) {
        out.t_us = now_us();
        out.no_data = true;
        out.status = static_cast<uint8_t>(drv_vl53l1x::RangeStatus::kNone);
    } else {
        out.t_us = r->t_us;
        out.valid = r->valid;
        out.distance_mm = r->valid ? r->range_mm : 0;
        out.raw_status = r->raw_status;
        out.status = static_cast<uint8_t>(r->status);
        out.signal_mcps = r->signal_mcps;
        out.ambient_mcps = r->ambient_mcps;
        out.read_us = r->read_us;
    }
    s_sample.write(out);
    // D-P1-1: cumulative counters, published with every record.
    if (r == nullptr) {
        s_st.no_data++;
        if (s_st.nodata_first_us == 0 && s_st.last_reading_us != 0) {
            s_st.nodata_first_us = out.t_us != 0 ? out.t_us : 1;
            s_st.nodata_episodes++;
        }
    } else {
        s_st.readings++;
        if (r->valid) s_st.valid++;
        const uint8_t st = static_cast<uint8_t>(r->status);
        s_st.status_count[st < 15 ? st : 15]++;
        s_st.last_reading_us = out.t_us != 0 ? out.t_us : 1;
        s_st.nodata_first_us = 0;
        s_st.last_distance_mm = out.distance_mm;
        s_st.last_valid = out.valid;
    }
    s_status.write(s_st);
}

// Full driver init (XSHUT reset included) and start of continuous ranging.
bool init_and_start() {
    s_st.recovery_us = 0;
    s_st.init = drv_vl53l1x::init(hal_i2c::Bus::B);
    s_st.inits++;
    s_st.start_err = ESP_OK;
    if (s_st.init.step == drv_vl53l1x::InitStep::kNone) {
        s_st.start_err = drv_vl53l1x::start_continuous(board::kTofPeriodMs);
    }
    const bool ok = s_st.init.step == drv_vl53l1x::InitStep::kNone && s_st.start_err == ESP_OK;
    s_st.state = ok ? TofState::kRunning : TofState::kFailed;
    s_status.write(s_st);
    return ok;
}

[[noreturn]] void park() {
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void tof_task(void*) {
    s_st.state = TofState::kStarting;
    s_st.bus_err = hal_i2c::init(hal_i2c::Bus::B, board::kTofSda, board::kTofScl, board::kI2cHz);
    s_st.bus_lines = hal_i2c::line_check(hal_i2c::Bus::B);
    if (s_st.bus_err != ESP_OK) {
        s_st.state = TofState::kFailed;
        s_status.write(s_st);
        park();
    }
    if (!init_and_start()) {
        park();
    }

    uint32_t last_event = now_us();
    for (;;) {
        if (s_reset_request.exchange(false)) {
            s_st.resets_done++;
            if (!init_and_start()) {
                park();
            }
            last_event = now_us();
            continue;
        }

        bool ready = false;
        bool io_error = false;
        if (drv_vl53l1x::data_ready(&ready) != ESP_OK) {
            s_st.io_errors++;
            s_status.write(s_st);
            io_error = true;
        }
        if (ready) {
            drv_vl53l1x::Reading r{};
            if (drv_vl53l1x::read(&r) != ESP_OK) {
                s_st.io_errors++;
                s_status.write(s_st);
                io_error = true;
            } else {
                publish_sample(&r);
                if (r.valid && s_st.recovery_us == 0) {
                    s_st.recovery_us = r.t_us - s_st.init.release_us;
                    s_status.write(s_st);
                }
                last_event = now_us();
                vTaskDelay(pdMS_TO_TICKS(kSleepAfterReadMs));
                continue;
            }
        }
        if (now_us() - last_event > kNoDataUs) {
            publish_sample(nullptr);  // no stale distance: say plainly that nothing arrived
            last_event = now_us();
        }
        // D-P1-2: after a bus B I/O error, wait one period before polling again. A failing bus (SDA
        // unplugged) would otherwise fail every 2 ms poll, and the IDF I2C driver logs every failure:
        // about 500 lines a second, more than the console carries. The wait is cut short at the no-data
        // deadline, so the no-data record is not delayed by it.
        uint32_t wait_ms = board::kTofPollMs;
        if (io_error) {
            const uint32_t silent_us = now_us() - last_event;
            const uint32_t left_ms = silent_us < kNoDataUs ? (kNoDataUs - silent_us) / 1000 + 1 : 1;
            wait_ms = left_ms < board::kTofErrorBackoffMs ? left_ms : board::kTofErrorBackoffMs;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

}  // namespace

void tof_task_start() {
    xTaskCreatePinnedToCore(tof_task, "tof", kStackBytes, nullptr, kPriority, nullptr, 0);
}

bool tof_read(TofSample* out) { return s_sample.read(out); }
bool tof_status(TofStatus* out) { return s_status.read(out); }
void tof_request_reset() { s_reset_request.store(true); }
