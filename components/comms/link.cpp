#include "link.h"

#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace comms {
namespace {

SeqlockSlot<setpoint::Setpoint> s_setpoint[setpoint::kTransports];
// B-M6-10: each setpoint write runs in a Core 0 critical section, so the writer cannot be
// preempted or interrupted with its slot half written (M6 saw Core 1 reads give up after 8 tries).
// One spinlock per slot, taken only by that slot's writer task on Core 0; Core 1 never takes it.
// portENTER_CRITICAL masks interrupts on this core up to the FreeRTOS critical level for the
// copy only (verify against IDF docs: Critical Sections, SMP).
portMUX_TYPE s_setpoint_mux[setpoint::kTransports] = {portMUX_INITIALIZER_UNLOCKED, portMUX_INITIALIZER_UNLOCKED};
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
    const uint32_t c0 = esp_cpu_get_cycle_count();
    portENTER_CRITICAL(&s_setpoint_mux[t]);
    const uint32_t c1 = esp_cpu_get_cycle_count();
    s_setpoint[t].write(s);
    const uint32_t c2 = esp_cpu_get_cycle_count();
    portEXIT_CRITICAL(&s_setpoint_mux[t]);
    const uint32_t c3 = esp_cpu_get_cycle_count();
    const uint32_t total = c3 - c0;
    const uint32_t inner = c2 - c1;
    c.wr_cyc_sum += total;
    if (total > c.wr_cyc_max) c.wr_cyc_max = total;
    c.wr_in_cyc_sum += inner;
    if (inner > c.wr_in_cyc_max) c.wr_in_cyc_max = inner;
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
