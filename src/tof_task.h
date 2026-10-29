#pragma once
#include <cstdint>

#include "drv_vl53l1x.h"
#include "esp_err.h"

// ToF task: Core 0, owns bus B and the VL53L1X driver, polls data-ready and publishes
// every reading through a seqlock slot. Single writer; never logs.

// One published record. An invalid reading never carries a distance (distance_mm = 0).
struct TofSample {
    uint32_t seq;            // increments with every record
    uint32_t t_us;           // 32-bit esp_timer time of the reading (wraps after about 71.6 min)
    uint16_t distance_mm;    // 0 unless valid
    bool valid;              // status "range valid" and at least board::kTofMinValidMm
    bool no_data;            // no data-ready for board::kTofNoDataPeriods periods
    uint8_t raw_status;      // RESULT__RANGE_STATUS (0 for a no-data record)
    uint8_t status;          // drv_vl53l1x::RangeStatus
    float signal_mcps;
    float ambient_mcps;
    uint32_t read_us;        // duration of the 17-byte result read
};

enum class TofState : uint8_t { kStarting, kRunning, kFailed };

struct TofStatus {
    TofState state;
    esp_err_t bus_err;
    esp_err_t start_err;            // start_continuous() after the latest init
    drv_vl53l1x::InitStatus init;   // latest init (first, or after a reset)
    uint32_t inits;                 // init attempts so far
    uint32_t resets_done;           // reset requests handled
    uint32_t recovery_us;           // XSHUT release to first valid reading after the latest init (0 = not yet)
    uint32_t io_errors;             // failed data-ready polls or result reads
};

void tof_task_start();
bool tof_read(TofSample* out);
bool tof_status(TofStatus* out);
void tof_request_reset();  // bench test: XSHUT reset and re-init while ranging
