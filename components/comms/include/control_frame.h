#pragma once
#include <cstddef>
#include <cstdint>

// Control frame (blueprint section 4.6): 14 bytes, little-endian.
//   0 magic 0xE5 | 1 type | 2 seq u16 | 4 roll i16 | 6 pitch i16 | 8 yaw i16 |
//   10 throttle u16 | 12 flags u8 | 13 crc8 over bytes 0 to 12
// CRC-8: polynomial 0x07, init 0x00, no reflection, no final XOR (CRC-8/SMBUS, check value
// 0xF4 for "123456789"). Ping and pong (M6 D-M6-03): the pong echoes seq, roll, pitch, yaw
// and flags, and carries the device turnaround time in microseconds (receive to send,
// saturating at 65535) in the throttle field. Pings never change the setpoint.
//
// Plain C++ with no ESP-IDF headers: the same source builds for the device and for the host
// unit test (tools/host_test). No heap use.
namespace comms::frame {

constexpr size_t kBytes = 14;
constexpr uint8_t kMagic = 0xE5;

enum class Type : uint8_t { kControl = 1, kPing = 2, kPong = 3 };

namespace flag {
constexpr uint8_t kArm = 1u << 0;
constexpr uint8_t kKill = 1u << 1;
constexpr uint8_t kAngleMode = 1u << 2;
}  // namespace flag

struct Frame {
    Type type = Type::kControl;
    uint16_t seq = 0;
    int16_t roll = 0;
    int16_t pitch = 0;
    int16_t yaw = 0;
    uint16_t throttle = 0;
    uint8_t flags = 0;
};

// Why a received buffer was rejected. Checked in this order.
enum class Error : uint8_t { kNone, kLength, kMagic, kCrc, kType };
const char* error_name(Error e);

uint8_t crc8(const uint8_t* data, size_t len);

void encode(const Frame& f, uint8_t out[kBytes]);  // fills the CRC

// Validates length (exactly kBytes), magic, CRC and type (1 to 3), then decodes into `out`.
Error decode(const uint8_t* buf, size_t len, Frame* out);

// The pong for a ping: same seq, roll, pitch, yaw and flags, throttle = turnaround.
Frame make_pong(const Frame& ping, uint32_t turnaround_us);

// Runs the known-answer vectors (components/comms/test_vectors/control_frame_vectors.txt)
// against this implementation. The text need not be NUL-terminated. Returns the number of
// failed checks (0 = pass); `checked` receives the number of checks run. On the first failure
// `first_fail` receives the 1-based line number (0 if none). Used by the firmware self-test
// and the host unit test.
int run_vectors(const char* text, size_t len, int* checked, int* first_fail);

}  // namespace comms::frame
