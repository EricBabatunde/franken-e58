#pragma once
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Thin wrapper over the ESP-IDF 5.x I2C master driver (driver/i2c_master.h).
// Not thread-safe: exactly one task owns each bus.
namespace hal_i2c {

enum class Bus : uint8_t { A = 0, B = 1 };  // A = I2C0 (IMU), B = I2C1 (ToF)

// Opaque device identifier. Stays valid across recover().
struct Device {
    uint8_t bus;
    uint8_t slot;
};

// Pad levels of SDA and SCL (0 or 1; -1 = not read).
struct Lines {
    int8_t sda = -1;
    int8_t scl = -1;
};

// What init() found on the lines before creating the bus (M4 C1).
enum class LineAction : uint8_t {
    kNotRun,   // init() not called yet
    kIdle,     // both high: nothing to do
    kCleared,  // SDA low, SCL high: clocked, SDA released
    kSdaStuck, // SDA low, SCL high: clocked, SDA still low
    kSclLow,   // SCL low: not clocked (the line is held by something else)
};
struct LineCheck {
    LineAction action = LineAction::kNotRun;
    Lines before;
    Lines after;       // after the clear (kCleared and kSdaStuck only)
    uint8_t clocks = 0;
};

// Before creating the bus, reads SDA and SCL. If SDA is low while SCL is high, clocks SCL
// up to 9 times until SDA releases and issues a STOP. The outcome is kept for line_check().
// No logging: bus A is created on Core 1. Callers report the outcome from Core 0.
esp_err_t init(Bus bus, int sda_gpio, int scl_gpio, uint32_t scl_hz);
LineCheck line_check(Bus bus);
const char* line_action_name(LineAction a);  // e.g. "idle, no clear"

// Reads the pad levels without creating or disturbing a bus: only the pin inputs are
// enabled. The Bus form uses the pins given to init() (both -1 before init()).
Lines read_lines(int sda_gpio, int scl_gpio);
Lines read_lines(Bus bus);
esp_err_t add_device(Bus bus, uint8_t addr7, Device* out);
esp_err_t probe(Bus bus, uint8_t addr7, int timeout_ms);

// reg8: one-byte register address (MPU-6500). reg16: two-byte big-endian address (VL53L1X).
esp_err_t read_reg8(Device d, uint8_t reg, uint8_t* buf, size_t len, int timeout_ms);
esp_err_t read_reg16(Device d, uint16_t reg, uint8_t* buf, size_t len, int timeout_ms);
esp_err_t write_reg8(Device d, uint8_t reg, uint8_t value, int timeout_ms);

// Writes to a two-byte big-endian register address (VL53L1X). The block form writes up
// to kMaxReg16BlockLen bytes in one transfer, auto-incrementing the register address.
constexpr size_t kMaxReg16BlockLen = 8;
esp_err_t write_reg16(Device d, uint16_t reg, uint8_t value, int timeout_ms);
esp_err_t write_reg16_block(Device d, uint16_t reg, const uint8_t* data, size_t len, int timeout_ms);

// Bus recovery: removes devices and the bus, clocks SCL up to 9 times until SDA
// releases, issues a STOP, then rebuilds the bus and re-adds the same devices.
//
// Never call while the vehicle is armed (diff D-M2-01). Rebuilding the bus allocates
// from the heap inside the IDF driver. In flight, an IMU bus error is handled by the
// cut trigger; recovery is only for the disarmed state, from outside the flight loop.
// (The driver itself already resets the controller and clears the bus after a timeout.)
// The M2 soak calls this from its Core 1 loop as a bench-test exception only.
//
// Transfers and probe() return ESP_ERR_INVALID_STATE while a handle is missing,
// e.g. after a failed rebuild. Calling recover() again retries the rebuild.
esp_err_t recover(Bus bus);

}  // namespace hal_i2c
