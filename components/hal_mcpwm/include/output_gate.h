#pragma once
#include <cstdint>

// Hard-latch rules for the motor outputs (M7b, decision D2). Header-only and free of IDF headers,
// so tools/host_test/test_output_gate.cpp runs the same code against a fake register layer.
//
// Ops supplies the hardware: mask() / unmask(token) (interrupts off and on for the calling core),
// force_all_low(), release_all() and set_compare(permille). On the device every caller of the gate
// is on Core 1 (the flight task and the GPTimer watchdog ISR), so masking interrupts makes each
// operation atomic with respect to the watchdog: no lock is shared with Core 0.
//
//   hard_cut()        set ONLY by the watchdog ISR (wdt::isr via hal_mcpwm::hard_cut)
//   clear_hard_cut()  cleared ONLY by the flight task, in the iteration where the safety state
//                     machine reports the transition into ARMED (the normal arm sequence)
//
// While the latch is set, release_all() refuses and set_permille() writes 0, so a stalled flight
// task that resumes in the middle of an output write cannot enable an output.
namespace hal_mcpwm {

template <class Ops>
class OutputGate {
public:
    // Watchdog cut: every generator forced low, then the latch set (reason != 0).
    void hard_cut(uint8_t reason) {
        const auto m = Ops::mask();
        Ops::force_all_low();
        latch_ = reason;
        Ops::unmask(m);
    }

    // Clears the latch only. The outputs stay forced until release_all().
    void clear_hard_cut() {
        const auto m = Ops::mask();
        latch_ = 0;
        Ops::unmask(m);
    }

    void force_low_all() {
        const auto m = Ops::mask();
        Ops::force_all_low();
        Ops::unmask(m);
    }

    // Returns false (and changes nothing) while the latch is set.
    bool release_all() {
        const auto m = Ops::mask();
        const bool ok = latch_ == 0;
        if (ok) Ops::release_all();
        Ops::unmask(m);
        return ok;
    }

    // Returns the value written: 0 while the latch is set.
    uint16_t set_permille(uint16_t permille) {
        const auto m = Ops::mask();
        const uint16_t p = latch_ != 0 ? 0 : (permille > 1000 ? 1000 : permille);
        Ops::set_compare(p);
        Ops::unmask(m);
        return p;
    }

    uint8_t latched() const { return latch_; }

private:
    volatile uint8_t latch_ = 0;  // 0: clear; otherwise the watchdog reason
};

}  // namespace hal_mcpwm
