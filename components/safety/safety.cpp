#include "safety.h"

namespace safety {

namespace {
std::atomic<uint8_t> s_published{static_cast<uint8_t>(State::kBoot)};
}  // namespace

void publish_state(State s) { s_published.store(static_cast<uint8_t>(s), std::memory_order_release); }

bool flash_writes_allowed() {
    return flash_writes_allowed(static_cast<State>(s_published.load(std::memory_order_acquire)));
}

State published_state() { return static_cast<State>(s_published.load(std::memory_order_acquire)); }

void Machine::emit(Outputs* out, const Event& e) {
    if (out->n_events < 2) {
        out->events[out->n_events] = e;
        out->events[out->n_events].seq = ++seq_;
        out->n_events++;
    }
}

void Machine::begin(int64_t now_us, Outputs* out) {
    out->n_events = 0;
    if (state_ == State::kBoot) {
        Event e;
        e.kind = EventKind::kState;
        e.from = State::kBoot;
        e.to = State::kSafe;
        e.t_us = now_us;
        state_ = State::kSafe;
        emit(out, e);
    }
    out->state = state_;
    out->outputs_allowed = false;
    out->force_low = true;
    out->duty_permille = 0;
    out->latched = latched_;
}

// Every reason arming would be refused now. `fresh`: the newest setpoint is younger than the
// silence threshold.
uint16_t Machine::blocking(const Inputs& in, bool fresh) const {
    uint16_t m = 0;
    if (state_ == State::kBoot || state_ == State::kSafe) m |= bit(Refusal::kNotReady);
    if (!fresh) m |= bit(Refusal::kNoLink);
    if (fresh && (in.flags & kFlagKill)) m |= bit(Refusal::kKill);
    if (!in.imu_healthy || in.imu_missing) m |= bit(Refusal::kImuUnhealthy);
    if (!in.wd_alive) m |= bit(Refusal::kWdDead);
    if (!in.gyro_cal_done) m |= bit(Refusal::kGyroCal);
    if (!in.accel_ok) m |= bit(Refusal::kAccelBad);
    if (in.accel_ok && !in.tilt_ok) m |= bit(Refusal::kTilted);
    if (!in.at_rest) m |= bit(Refusal::kMoving);
    if (throttle_low_since_ < 0 || in.now_us - throttle_low_since_ < cfg_.throttle_min_hold_us) {
        m |= bit(Refusal::kThrottleNotMin);
    }
    if (!vbat_ok()) m |= bit(Refusal::kVbatLow);
    return m;
}

void Machine::set_outputs(const Inputs& in, Outputs* out) const {
    out->state = state_;
    out->latched = latched_;
    out->outputs_allowed = state_ == State::kArmed;
    if (state_ == State::kArmed) {
        const uint32_t thr = in.throttle > 1000 ? 1000u : in.throttle;
        out->duty_permille = static_cast<uint16_t>(thr * cfg_.duty_cap_permille / 1000u);
        // Duty 0 is held by the force, not by a compare value of 0 (no reliance on the
        // generator's event priority at the counter reset).
        out->force_low = out->duty_permille == 0;
    } else {
        out->duty_permille = 0;
        out->force_low = true;
    }
}

void Machine::step(const Inputs& in, Outputs* out) {
    out->n_events = 0;
    const int64_t age = in.have_setpoint ? in.now_us - in.sp_rx_us : INT64_MAX;
    const bool fresh = in.have_setpoint && age < cfg_.link_silence_us;
    const bool silent = !in.have_setpoint || age > cfg_.link_silence_us;

    // A reconnect: the link comes back after being stale, or the newest setpoint switches
    // transport. Either way the arm flag must be seen low again before an edge counts.
    if (fresh && (!was_fresh_ || in.sp_transport != last_transport_)) {
        first_after_reset_ = true;
        arm_prev_ = true;
    }
    was_fresh_ = fresh;
    if (fresh) last_transport_ = in.sp_transport;

    // Throttle-at-minimum timer, on fresh frames only.
    const uint16_t thr = in.throttle > 1000 ? 1000 : in.throttle;
    if (fresh && thr <= cfg_.throttle_min_permille) {
        if (throttle_low_since_ < 0) throttle_low_since_ = in.now_us;
    } else {
        throttle_low_since_ = -1;
    }

    Event base;
    base.t_us = in.now_us;
    base.sp_rx_us = in.have_setpoint ? in.sp_rx_us : 0;
    base.imu_last_us = in.imu_last_us;
    base.throttle = thr;
    base.flags = in.flags;

    // ---- ARMED: cut triggers, in order: watchdog hard latch (the gates are already low), KILL,
    // watchdog dead, missing IMU (backup tick check), link silence; then the arm flag.
    if (state_ == State::kArmed) {
        Cut c = Cut::kNone;
        if (in.hard_cut != 0) {
            c = in.hard_cut == kHardImuMissing ? Cut::kWdImuMissing : Cut::kWdHeartbeat;
        } else if (fresh && (in.flags & kFlagKill)) {
            c = Cut::kKill;
        } else if (!in.wd_alive) {
            c = Cut::kWdDead;
        } else if (in.imu_missing_cut) {
            c = Cut::kImuMissing;
        } else if (silent) {
            c = Cut::kLinkSilence;
        }
        if (c != Cut::kNone) {
            Event e = base;
            e.kind = EventKind::kCut;
            e.from = State::kArmed;
            e.to = State::kDisarmed;
            e.cut = c;
            state_ = State::kDisarmed;
            latched_ = true;
            last_cut_ = c;
            arm_prev_ = true;  // a fresh edge is needed: the flag must be seen low first
            emit(out, e);
        } else if (fresh && in.sp_rx_us != last_rx_us_ && !(in.flags & kFlagArm)) {
            Event e = base;
            e.kind = EventKind::kDisarm;
            e.from = State::kArmed;
            e.to = State::kDisarmed;
            state_ = State::kDisarmed;
            arm_prev_ = false;
            emit(out, e);
        }
    }

    // ---- SAFE -> DISARMED.
    if (state_ == State::kSafe && in.gyro_cal_done && in.imu_healthy && !in.imu_missing) {
        Event e = base;
        e.kind = EventKind::kState;
        e.from = State::kSafe;
        e.to = State::kDisarmed;
        state_ = State::kDisarmed;
        emit(out, e);
    }

    const uint16_t block = blocking(in, fresh);
    out->blocking = block;

    // ---- Arm attempts, judged once per new fresh frame: a rising arm edge, or the arm flag
    // already high on the first frame after boot or a reconnect (refused: no edge).
    if (fresh && in.sp_rx_us != last_rx_us_ && state_ != State::kArmed) {
        const bool arm = (in.flags & kFlagArm) != 0;
        if (first_after_reset_) {
            first_after_reset_ = false;
            if (arm) {
                Event e = base;
                e.kind = EventKind::kRefusal;
                e.from = e.to = state_;
                e.refusal = Refusal::kNoArmEdge;
                e.mask = static_cast<uint16_t>(block | bit(Refusal::kNoArmEdge));
                emit(out, e);
            }
        } else if (arm && !arm_prev_) {
            if (block == 0) {
                Event e = base;
                e.kind = EventKind::kState;
                e.from = state_;
                e.to = State::kArmed;
                state_ = State::kArmed;
                latched_ = false;
                emit(out, e);
            } else {
                Event e = base;
                e.kind = EventKind::kRefusal;
                e.from = e.to = state_;
                e.mask = block;
                for (unsigned r = 1; r < static_cast<unsigned>(Refusal::kCount); ++r) {
                    if (block & (1u << r)) {
                        e.refusal = static_cast<Refusal>(r);
                        break;
                    }
                }
                emit(out, e);
            }
        }
        // An attempt consumes the edge: after a refusal the flag must go low and high again.
        arm_prev_ = arm;
    }
    if (fresh) last_rx_us_ = in.sp_rx_us;

    set_outputs(in, out);
}

bool Machine::to_safe(int64_t now_us, Outputs* out) {
    out->n_events = 0;
    if (state_ == State::kArmed) return false;
    if (state_ == State::kDisarmed) {
        Event e;
        e.kind = EventKind::kState;
        e.from = State::kDisarmed;
        e.to = State::kSafe;
        e.t_us = now_us;
        state_ = State::kSafe;
        emit(out, e);
    }
    out->state = state_;
    out->outputs_allowed = false;
    out->force_low = true;
    out->duty_permille = 0;
    out->latched = latched_;
    return true;
}

bool Machine::to_boot(int64_t now_us, Outputs* out) {
    out->n_events = 0;
    if (state_ == State::kArmed) return false;
    if (state_ != State::kBoot) {
        Event e;
        e.kind = EventKind::kState;
        e.from = state_;
        e.to = State::kBoot;
        e.t_us = now_us;
        state_ = State::kBoot;
        emit(out, e);
    }
    out->state = state_;
    out->outputs_allowed = false;
    out->force_low = true;
    out->duty_permille = 0;
    out->latched = latched_;
    return true;
}

const char* name(State s) {
    switch (s) {
        case State::kBoot:     return "BOOT";
        case State::kSafe:     return "SAFE";
        case State::kDisarmed: return "DISARMED";
        case State::kArmed:    return "ARMED";
    }
    return "?";
}

const char* name(Cut c) {
    switch (c) {
        case Cut::kNone:        return "none";
        case Cut::kLinkSilence: return "link_silence";
        case Cut::kKill:        return "kill";
        case Cut::kImuMissing:  return "imu_missing_tick";
        case Cut::kWdHeartbeat: return "wd_heartbeat";
        case Cut::kWdImuMissing: return "wd_imu_missing";
        case Cut::kWdDead:      return "wd_dead";
    }
    return "?";
}

const char* name(Refusal r) {
    switch (r) {
        case Refusal::kNone:           return "none";
        case Refusal::kNotReady:       return "not_ready";
        case Refusal::kNoLink:         return "no_link";
        case Refusal::kKill:           return "kill";
        case Refusal::kNoArmEdge:      return "arm_held_no_edge";
        case Refusal::kImuUnhealthy:   return "imu_unhealthy";
        case Refusal::kWdDead:         return "wd_dead";
        case Refusal::kGyroCal:        return "gyro_cal";
        case Refusal::kAccelBad:       return "accel_bad";
        case Refusal::kTilted:         return "tilted";
        case Refusal::kMoving:         return "moving";
        case Refusal::kThrottleNotMin: return "throttle_not_min";
        case Refusal::kVbatLow:        return "vbat_low";
        case Refusal::kCount:          break;
    }
    return "?";
}

}  // namespace safety
