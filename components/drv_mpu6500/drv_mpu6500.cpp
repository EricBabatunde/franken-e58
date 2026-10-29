#include "drv_mpu6500.h"

#include <atomic>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "hal_gpio.h"

namespace drv_mpu6500 {
namespace {

// Registers (docs/mpu6500_notes.md section 2).
constexpr uint8_t kRegSmplrtDiv = 0x19;     // RM p.12
constexpr uint8_t kRegConfig = 0x1A;        // RM p.13: [2:0] DLPF_CFG, [6] FIFO_MODE
constexpr uint8_t kRegGyroConfig = 0x1B;    // RM p.14: [4:3] GYRO_FS_SEL, [1:0] FCHOICE_B
constexpr uint8_t kRegAccelConfig = 0x1C;   // RM p.14-15: [4:3] ACCEL_FS_SEL
constexpr uint8_t kRegAccelConfig2 = 0x1D;  // RM p.15: [3] ACCEL_FCHOICE_B, [2:0] A_DLPF_CFG
constexpr uint8_t kRegIntPinCfg = 0x37;     // RM p.29
constexpr uint8_t kRegIntEnable = 0x38;     // RM p.29-30
constexpr uint8_t kRegAccelXoutH = 0x3B;    // RM p.7, p.31-35: 14-byte data block
constexpr uint8_t kRegPwrMgmt1 = 0x6B;      // RM p.41
constexpr uint8_t kRegWhoAmI = 0x75;        // RM p.44

// Values.
constexpr uint8_t kWhoAmI = 0x70;              // RM p.44
constexpr uint8_t kPwrDeviceReset = 0x80;      // PWR_MGMT_1.DEVICE_RESET, self-clearing (RM p.41)
constexpr uint8_t kPwrAfterReset = 0x01;       // SLEEP = 0, CLKSEL = 1 (RM p.9, p.41). Never write 0x00.
constexpr uint8_t kIntPinCfg = 0x10;           // push-pull, active high, 50 us pulse, INT_ANYRD_2CLEAR (RM p.29)
constexpr uint8_t kIntEnableRawRdy = 0x01;     // RAW_RDY_EN (RM p.30)
constexpr int kFsSelShift = 3;                 // GYRO_FS_SEL / ACCEL_FS_SEL bits [4:3] (RM p.14-15)

// Timing. The register map gives a post-reset wait only for SPI (100 ms, RM p.42); the
// product specification gives 100 ms maximum from power-up to register access (PS p.10).
// verify against datasheet: wait after DEVICE_RESET over I2C is not stated.
constexpr uint32_t kResetWaitMs = 100;
constexpr uint32_t kGyroStartupUs = 35000;  // gyro start-up from sleep (PS p.7)
constexpr int kTimeoutMs = 10;

// Full-scale codes and sensitivities (RM p.14-15; PS p.7-8).
constexpr int gyro_fs_sel(int dps) {
    return dps == 250 ? 0 : dps == 500 ? 1 : dps == 1000 ? 2 : dps == 2000 ? 3 : -1;
}
constexpr float gyro_lsb_per_dps(int dps) {
    return dps == 250 ? 131.0f : dps == 500 ? 65.5f : dps == 1000 ? 32.8f : 16.4f;
}
constexpr int accel_fs_sel(int g) {
    return g == 2 ? 0 : g == 4 ? 1 : g == 8 ? 2 : g == 16 ? 3 : -1;
}
constexpr float accel_lsb_per_g(int g) {
    return g == 2 ? 16384.0f : g == 4 ? 8192.0f : g == 8 ? 4096.0f : 2048.0f;
}
static_assert(gyro_fs_sel(board::kImuGyroRangeDps) >= 0, "gyro range must be 250, 500, 1000 or 2000 deg/s");
static_assert(accel_fs_sel(board::kImuAccelRangeG) >= 0, "accel range must be 2, 4, 8 or 16 g");
static_assert(board::kImuAccelDlpfCfg >= 0 && board::kImuAccelDlpfCfg <= 7, "A_DLPF_CFG is 3 bits");
static_assert(board::kImuSmplrtDiv >= 0 && board::kImuSmplrtDiv <= 255, "SMPLRT_DIV is 8 bits");

// FCHOICE_B = 00 keeps the DLPF and SMPLRT_DIV in use (RM p.12, p.14).
constexpr uint8_t kGyroConfig = static_cast<uint8_t>(gyro_fs_sel(board::kImuGyroRangeDps) << kFsSelShift);
// ACCEL_FCHOICE_B = 0 keeps the accel DLPF in use (RM p.15).
constexpr uint8_t kAccelConfig = static_cast<uint8_t>(accel_fs_sel(board::kImuAccelRangeG) << kFsSelShift);
constexpr uint8_t kAccelConfig2 = static_cast<uint8_t>(board::kImuAccelDlpfCfg);
constexpr float kGyroScale = 1.0f / gyro_lsb_per_dps(board::kImuGyroRangeDps);
constexpr float kAccelScale = 1.0f / accel_lsb_per_g(board::kImuAccelRangeG);

// Driver state, owned by the bus-owning task.
hal_i2c::Device s_dev{};
bool s_have_dev = false;
uint32_t s_gyro_valid_from = 0;
uint8_t s_burst[14];

// ISR state. Every field has a single writer: the ISR, except where noted.
TaskHandle_t s_task = nullptr;            // written once before the handler is installed
std::atomic<uint32_t> s_raw_count{0};
std::atomic<uint32_t> s_notify_ts{0};
uint32_t s_div_count = 0;                 // ISR only
#if FE_IMU_ISR_INSTRUMENT
std::atomic<uint32_t> s_notify_every{board::kImuIsrDecimation};  // written by the task
std::atomic<uint32_t> s_instr_epoch{0};                           // written by the task
uint32_t s_isr_epoch = 0;                 // ISR only
uint32_t s_prev_raw_ts = 0;               // ISR only
bool s_have_prev_raw = false;             // ISR only
std::atomic<uint32_t> s_raw_iv_min{UINT32_MAX};
std::atomic<uint32_t> s_raw_iv_max{0};
std::atomic<uint32_t> s_cycles_max{0};
#endif

constexpr auto kRelaxed = std::memory_order_relaxed;

InitStatus fail(InitStatus st, Step step, esp_err_t err, uint8_t reg, uint8_t expected, uint8_t actual) {
    st.step = step;
    st.err = (err == ESP_OK) ? ESP_ERR_INVALID_RESPONSE : err;
    st.reg = reg;
    st.expected = expected;
    st.actual = actual;
    return st;
}

// Writes `value` to `reg`, reads it back and compares. Returns false and fills `st` on failure.
bool write_checked(uint8_t reg, uint8_t value, InitStatus& st) {
    esp_err_t e = hal_i2c::write_reg8(s_dev, reg, value, kTimeoutMs);
    if (e != ESP_OK) {
        st = fail(st, Step::kWrite, e, reg, value, 0);
        return false;
    }
    uint8_t back = 0;
    e = hal_i2c::read_reg8(s_dev, reg, &back, 1, kTimeoutMs);
    if (e != ESP_OK || back != value) {
        st = fail(st, Step::kReadback, e, reg, value, back);
        return false;
    }
    st.n_checked++;
    return true;
}

// Init steps a to d of the M3 spec.
InitStatus run_sequence(hal_i2c::Bus bus, uint8_t dlpf_cfg) {
    InitStatus st{};
    st.dlpf_cfg = dlpf_cfg;

    // a. Probe and identify.
    esp_err_t e = hal_i2c::probe(bus, board::kMpuAddr, kTimeoutMs);
    if (e != ESP_OK) {
        return fail(st, Step::kProbe, e, 0, 0, 0);
    }
    e = hal_i2c::read_reg8(s_dev, kRegWhoAmI, &st.who_am_i, 1, kTimeoutMs);
    if (e != ESP_OK || st.who_am_i != kWhoAmI) {
        return fail(st, Step::kWhoAmI, e, kRegWhoAmI, kWhoAmI, st.who_am_i);
    }

    // b. Reset: the IMU is not power-cycled when the ESP32 resets, so it may still hold
    //    an earlier configuration.
    e = hal_i2c::write_reg8(s_dev, kRegPwrMgmt1, kPwrDeviceReset, kTimeoutMs);
    st.reset_us = static_cast<uint32_t>(esp_timer_get_time());
    if (e != ESP_OK) {
        return fail(st, Step::kReset, e, kRegPwrMgmt1, kPwrDeviceReset, 0);
    }
    s_gyro_valid_from = st.reset_us + kGyroStartupUs;
    vTaskDelay(pdMS_TO_TICKS(kResetWaitMs) + 1);  // +1 tick: at least the full wait
    e = hal_i2c::read_reg8(s_dev, kRegPwrMgmt1, &st.pwr_mgmt_1, 1, kTimeoutMs);
    if (e != ESP_OK || st.pwr_mgmt_1 != kPwrAfterReset) {
        return fail(st, Step::kResetCheck, e, kRegPwrMgmt1, kPwrAfterReset, st.pwr_mgmt_1);
    }

    // c. Configuration, each register read back.
    if (!write_checked(kRegConfig, dlpf_cfg, st) ||
        !write_checked(kRegGyroConfig, kGyroConfig, st) ||
        !write_checked(kRegAccelConfig, kAccelConfig, st) ||
        !write_checked(kRegAccelConfig2, kAccelConfig2, st) ||
        !write_checked(kRegSmplrtDiv, static_cast<uint8_t>(board::kImuSmplrtDiv), st) ||
        !write_checked(kRegIntPinCfg, kIntPinCfg, st)) {
        return st;
    }

    // d. Data-ready last, so no pulse arrives before the configuration is complete.
    write_checked(kRegIntEnable, kIntEnableRawRdy, st);
    return st;
}

hal_i2c::Bus s_bus = hal_i2c::Bus::A;

// Data-ready ISR. Flight builds: timestamp, count, notify. Bench builds add the
// instrumentation behind FE_IMU_ISR_INSTRUMENT.
void IRAM_ATTR isr(void*) {
#if FE_IMU_ISR_INSTRUMENT
    const uint32_t c0 = esp_cpu_get_cycle_count();
#endif
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    s_raw_count.store(s_raw_count.load(kRelaxed) + 1, kRelaxed);

#if FE_IMU_ISR_INSTRUMENT
    const uint32_t epoch = s_instr_epoch.load(kRelaxed);
    if (epoch != s_isr_epoch) {
        s_isr_epoch = epoch;
        s_have_prev_raw = false;
        s_raw_iv_min.store(UINT32_MAX, kRelaxed);
        s_raw_iv_max.store(0, kRelaxed);
        s_cycles_max.store(0, kRelaxed);
    }
    if (s_have_prev_raw) {
        const uint32_t iv = now - s_prev_raw_ts;
        if (iv < s_raw_iv_min.load(kRelaxed)) s_raw_iv_min.store(iv, kRelaxed);
        if (iv > s_raw_iv_max.load(kRelaxed)) s_raw_iv_max.store(iv, kRelaxed);
    }
    s_prev_raw_ts = now;
    s_have_prev_raw = true;
    const uint32_t every = s_notify_every.load(kRelaxed);
#else
    constexpr uint32_t every = board::kImuIsrDecimation;
#endif

    if (every != 0 && ++s_div_count >= every) {
        s_div_count = 0;
        s_notify_ts.store(now, std::memory_order_release);
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &woken);
        portYIELD_FROM_ISR(woken);
    }

#if FE_IMU_ISR_INSTRUMENT
    const uint32_t cycles = esp_cpu_get_cycle_count() - c0;
    if (cycles > s_cycles_max.load(kRelaxed)) s_cycles_max.store(cycles, kRelaxed);
#endif
}

int16_t be16(const uint8_t* p) {
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

}  // namespace

InitStatus init(hal_i2c::Bus bus) {
    s_bus = bus;
    if (!s_have_dev) {
        const esp_err_t e = hal_i2c::add_device(bus, board::kMpuAddr, &s_dev);
        if (e != ESP_OK) {
            InitStatus st{};
            st.dlpf_cfg = static_cast<uint8_t>(board::kImuDlpfCfg);
            return fail(st, Step::kProbe, e, 0, 0, 0);
        }
        s_have_dev = true;
    }
    return run_sequence(bus, static_cast<uint8_t>(board::kImuDlpfCfg));
}

InitStatus reconfigure(uint8_t dlpf_cfg) {
    if (!s_have_dev || dlpf_cfg > 7) {
        InitStatus st{};
        st.dlpf_cfg = dlpf_cfg;
        return fail(st, Step::kBadArgument, ESP_ERR_INVALID_ARG, kRegConfig, 0, dlpf_cfg);
    }
    return run_sequence(s_bus, dlpf_cfg);
}

esp_err_t read_burst(uint32_t t_us, RawSample* out) {
    const esp_err_t e = hal_i2c::read_reg8(s_dev, kRegAccelXoutH, s_burst, sizeof(s_burst), kTimeoutMs);
    if (e != ESP_OK) {
        return e;
    }
    out->t_us = t_us;
    for (int i = 0; i < 3; ++i) {
        out->accel[i] = be16(&s_burst[2 * i]);
        out->gyro[i] = be16(&s_burst[8 + 2 * i]);
    }
    out->temp = be16(&s_burst[6]);
    return ESP_OK;
}

uint32_t gyro_valid_from_us() { return s_gyro_valid_from; }

float gyro_dps(int16_t raw) { return static_cast<float>(raw) * kGyroScale; }
float accel_g(int16_t raw) { return static_cast<float>(raw) * kAccelScale; }

esp_err_t int_install(TaskHandle_t task) {
    s_task = task;
    return hal_gpio::imu_int_install(&isr, nullptr);
}

uint32_t raw_pulses() { return s_raw_count.load(kRelaxed); }
uint32_t last_notify_us() { return s_notify_ts.load(std::memory_order_acquire); }

#if FE_IMU_ISR_INSTRUMENT
void isr_set_notify_every(uint32_t n) { s_notify_every.store(n, kRelaxed); }

void isr_instrument_reset() { s_instr_epoch.store(s_instr_epoch.load(kRelaxed) + 1, kRelaxed); }

IsrInstrument isr_instrument() {
    return {s_raw_iv_min.load(kRelaxed), s_raw_iv_max.load(kRelaxed), s_cycles_max.load(kRelaxed)};
}
#endif

}  // namespace drv_mpu6500
