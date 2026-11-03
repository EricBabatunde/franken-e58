#include "link.h"

#include "esp_timer.h"

namespace comms {
namespace {

SeqlockSlot<setpoint::Setpoint> s_setpoint[setpoint::kTransports];
SeqlockSlot<link::Counters> s_counters[setpoint::kTransports];

}  // namespace

SeqlockSlot<setpoint::Setpoint>& setpoint::slot(Transport t) { return s_setpoint[t]; }

namespace link {

SeqlockSlot<Counters>& counters(Transport t) { return s_counters[t]; }

Result handle(Transport t, const uint8_t* buf, size_t len, int64_t t_rx_us, Counters& c, frame::Frame* ping) {
    c.rx++;
    frame::Frame f;
    frame::Error e = frame::decode(buf, len, &f);
    if (e == frame::Error::kNone && f.type == frame::Type::kPong) {
        e = frame::Error::kType;  // only the device sends pongs
    }
    switch (e) {
        case frame::Error::kNone:   break;
        case frame::Error::kLength: c.err_length++; return Result::kInvalid;
        case frame::Error::kMagic:  c.err_magic++;  return Result::kInvalid;
        case frame::Error::kCrc:    c.err_crc++;    return Result::kInvalid;
        case frame::Error::kType:   c.err_type++;   return Result::kInvalid;
    }
    if (f.type == frame::Type::kPing) {
        c.pings++;
        *ping = f;
        return Result::kPing;
    }
    setpoint::Setpoint s;
    s.t_rx_us = t_rx_us;
    s.seq = f.seq;
    s.roll = f.roll;
    s.pitch = f.pitch;
    s.yaw = f.yaw;
    s.throttle = f.throttle;
    s.flags = f.flags;
    s.transport = t;
    s.check = setpoint::check_word(s);
    s_setpoint[t].write(s);
    c.controls++;
    return Result::kControl;
}

void build_pong(const frame::Frame& ping, int64_t t_rx_us, Counters& c, uint8_t out[frame::kBytes]) {
    const int64_t ta64 = esp_timer_get_time() - t_rx_us;
    const uint32_t ta = ta64 < 0 ? 0 : ta64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ta64);
    c.ta_n++;
    c.ta_sum += ta;
    if (ta < c.ta_min) c.ta_min = ta;
    if (ta > c.ta_max) c.ta_max = ta;
    frame::encode(frame::make_pong(ping, ta), out);
}

}  // namespace link
}  // namespace comms
