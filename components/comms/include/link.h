#pragma once
#include <cstddef>
#include <cstdint>

#include "control_frame.h"
#include "seqlock.h"
#include "setpoint.h"

// Shared receive path for the UDP and WebSocket transports (M6): validation, setpoint update,
// pong building and per-transport statistics. Core 0 only. No heap use per frame.
namespace comms::link {

using setpoint::Transport;

// Per-transport counters. Written only by the transport's own task (single writer) and
// published through a seqlock after every frame; the test reads them from Core 0.
struct Counters {
    uint32_t rx = 0;              // frames (or datagrams) received, valid or not
    uint32_t tx = 0;              // pongs sent
    uint32_t pings = 0;           // valid pings
    uint32_t controls = 0;        // valid control frames (setpoint written)
    uint32_t err_length = 0;
    uint32_t err_magic = 0;
    uint32_t err_crc = 0;
    uint32_t err_type = 0;        // type outside 1 to 3, or a pong sent to the device
    uint32_t send_errors = 0;
    uint32_t ta_n = 0;            // device turnaround, receive to send (us)
    uint32_t ta_min = UINT32_MAX;
    uint32_t ta_max = 0;
    uint64_t ta_sum = 0;
    // WebSocket only.
    uint32_t sessions = 0;        // handshakes completed
    uint32_t closed_bad = 0;      // sessions closed by us on a malformed frame
    uint32_t plain_get = 0;       // GET /ws without an upgrade (400)
};

SeqlockSlot<Counters>& counters(Transport t);

enum class Result : uint8_t { kInvalid, kControl, kPing };

// Validates one received buffer and counts it in `c`. Control: writes the transport's setpoint
// slot. Ping: fills `ping` for the caller to answer. Invalid: counted by cause, nothing else.
Result handle(Transport t, const uint8_t* buf, size_t len, int64_t t_rx_us, Counters& c, frame::Frame* ping);

// Records one pong's turnaround in `c` (call just before sending) and encodes the pong.
void build_pong(const frame::Frame& ping, int64_t t_rx_us, Counters& c, uint8_t out[frame::kBytes]);

}  // namespace comms::link
