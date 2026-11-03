#pragma once
#include <cstdint>

#include "seqlock.h"

// Setpoint hand-off from the comms handlers (Core 0) to the flight loop (Core 1), blueprint
// section 4.2. One seqlock slot per transport (M6 D-M6-01): SeqlockSlot is single-writer, and
// the UDP task and the httpd task both receive control frames. Core 1 reads both and uses the
// newer. No lock anywhere. In M6 nothing acts on the setpoint.
namespace comms::setpoint {

enum Transport : uint8_t { kUdp = 0, kWs = 1, kTransports = 2 };

struct Setpoint {
    int64_t t_rx_us = 0;  // esp_timer_get_time() when the frame arrived (same timebase on both cores)
    uint16_t seq = 0;
    int16_t roll = 0;
    int16_t pitch = 0;
    int16_t yaw = 0;
    uint16_t throttle = 0;
    uint8_t flags = 0;
    uint8_t transport = kUdp;
    uint32_t check = 0;   // check_word() of the fields above: a torn copy would not match
};

// FNV-1a over the fields. Lets the consumer prove that no torn record reaches it (M6
// acceptance 7); the seqlock should already guarantee that.
inline uint32_t check_word(const Setpoint& s) {
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            h ^= (v >> (8 * i)) & 0xFFu;
            h *= 16777619u;
        }
    };
    mix(static_cast<uint32_t>(s.t_rx_us), 4);
    mix(static_cast<uint32_t>(static_cast<uint64_t>(s.t_rx_us) >> 32), 4);
    mix(s.seq, 2);
    mix(static_cast<uint16_t>(s.roll), 2);
    mix(static_cast<uint16_t>(s.pitch), 2);
    mix(static_cast<uint16_t>(s.yaw), 2);
    mix(s.throttle, 2);
    mix(s.flags, 1);
    mix(s.transport, 1);
    return h;
}

// Written only by the transport's own task (UDP: udp_rx; WebSocket: httpd).
SeqlockSlot<Setpoint>& slot(Transport t);

}  // namespace comms::setpoint
