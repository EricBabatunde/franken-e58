// Host test for the motor-output hard latch (M7b, components/hal_mcpwm/include/output_gate.h), the
// same header the firmware compiles. A fake register layer stands in for MCPWM: four generators
// (forced low or released) and a compare value. Its mask() defers a pending "watchdog interrupt"
// until unmask(), as the hardware does on Core 1, so the test can fire the watchdog at every point
// between the flight task's gate calls and check that no output is ever enabled while latched.
//
// g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/hal_mcpwm/include
//     tools/host_test/test_output_gate.cpp -o .pio/host/test_output_gate && .pio/host/test_output_gate
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>

#include "output_gate.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

struct Fake;
Fake* g_fake = nullptr;

// Fake hardware: true = forced low.
struct Fake {
    bool forced[4] = {true, true, true, true};
    uint16_t compare = 0;
    int mask_depth = 0;
    std::function<void()> pending_isr;  // fires at the next point where interrupts are unmasked
    std::function<void()> isr_in_next_mask;  // arrives inside the next masked section
};

struct FakeOps {
    static int mask() {
        if (g_fake->isr_in_next_mask) {
            g_fake->pending_isr = g_fake->isr_in_next_mask;  // held off by the mask
            g_fake->isr_in_next_mask = nullptr;
        }
        return ++g_fake->mask_depth;
    }
    static void unmask(int) {
        --g_fake->mask_depth;
        if (g_fake->mask_depth == 0 && g_fake->pending_isr) {
            auto f = g_fake->pending_isr;
            g_fake->pending_isr = nullptr;
            f();
        }
    }
    static void force_all_low() {
        for (bool& f : g_fake->forced) f = true;
    }
    static void release_all() {
        for (bool& f : g_fake->forced) f = false;
    }
    static void set_compare(uint16_t p) { g_fake->compare = p; }
};

using Gate = hal_mcpwm::OutputGate<FakeOps>;

bool output_on(const Fake& f) {
    for (bool forced : f.forced) {
        if (!forced && f.compare > 0) return true;
    }
    return false;
}

// Interrupt arrival: immediately if unmasked, else deferred to the unmask (hardware behaviour).
void raise_isr(Fake& f, std::function<void()> isr) {
    if (f.mask_depth == 0) {
        isr();
    } else {
        f.pending_isr = std::move(isr);
    }
}

void test_basics() {
    Fake f;
    g_fake = &f;
    Gate g;
    check(g.latched() == 0, "starts clear");
    check(g.set_permille(400) == 400 && g.release_all() && output_on(f), "clear: duty set and released");
    g.hard_cut(1);
    check(g.latched() == 1 && !output_on(f), "hard_cut: forced low, latched");
    check(!g.release_all() && !output_on(f), "latched: release refused");
    check(g.set_permille(700) == 0 && f.compare == 0, "latched: set_permille writes 0");
    g.force_low_all();
    check(g.latched() == 1, "force_low_all keeps the latch");
    g.clear_hard_cut();
    check(g.latched() == 0 && !output_on(f), "clear_hard_cut alone does not release");
    check(g.set_permille(1500) == 1000, "clamped to 1000");
    check(g.release_all() && output_on(f), "after the clear: release works");
}

// The flight task's output sequence (flight.cpp): set the duty, then release. The watchdog fires
// at each point (before, between, inside each masked section, after); afterwards no output may be
// on and the latch must still be set.
void test_isr_at_every_point() {
    for (int point = 0; point < 6; ++point) {
        Fake f;
        g_fake = &f;
        Gate g;
        auto isr = [&g] { g.hard_cut(1); };
        if (point == 0) raise_isr(f, isr);
        // set_permille(), with the ISR arriving inside its masked section (deferred to the unmask)
        if (point == 1) f.isr_in_next_mask = isr;
        const uint16_t d = g.set_permille(500);
        if (point == 2) raise_isr(f, isr);
        bool released = false;
        // Arrives while release_all() holds the mask: runs at its unmask, after the release.
        if (point == 3) f.isr_in_next_mask = isr;
        released = g.release_all();
        if (point == 4) raise_isr(f, isr);
        if (point == 5) {
            // The stalled task resumes and repeats the write it was making.
            raise_isr(f, isr);
            (void)g.set_permille(500);
            (void)g.release_all();
        }
        const std::string at = "ISR at point " + std::to_string(point);
        check(g.latched() == 1, at + ": latch set");
        check(!output_on(f), at + ": no output on afterwards");
        (void)d;
        (void)released;
    }
}

// Random interleavings of flight-task operations and watchdog interrupts, the interrupt arriving
// either between operations or inside the next operation's masked section.
void test_random() {
    std::mt19937 rng(777);
    long latched_checks = 0;
    for (int run = 0; run < 20000; ++run) {
        Fake f;
        g_fake = &f;
        Gate g;
        const int steps = 1 + static_cast<int>(rng() % 30);
        for (int i = 0; i < steps; ++i) {
            auto isr = [&g] { g.hard_cut(1); };
            const uint32_t when = rng() % 6;
            if (when == 0) raise_isr(f, isr);
            if (when == 1) f.isr_in_next_mask = isr;
            switch (rng() % 5) {
                case 0: (void)g.set_permille(static_cast<uint16_t>(rng() % 1100)); break;
                case 1: (void)g.release_all(); break;
                case 2: g.force_low_all(); break;
                case 3: if (rng() % 8 == 0) g.clear_hard_cut(); break;  // only on a legitimate arm
                default: break;
            }
            if (f.isr_in_next_mask) {  // no masked section in this step: arrives now
                auto late = f.isr_in_next_mask;
                f.isr_in_next_mask = nullptr;
                raise_isr(f, late);
            }
            check(f.mask_depth == 0 && !f.pending_isr, "random: no interrupt left pending after an operation");
            if (g.latched() != 0) {
                check(!output_on(f), "random: never an output on while latched");
                latched_checks++;
            }
        }
    }
    std::printf("random: %ld latched states checked\n", latched_checks);
}

}  // namespace

int main() {
    test_basics();
    test_isr_at_every_point();
    test_random();
    std::printf("output gate host test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
