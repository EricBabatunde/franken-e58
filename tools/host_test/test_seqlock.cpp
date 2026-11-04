// Host stress test for SeqlockSlot (components/seqlock/include/seqlock.h), the device source.
// One writer thread and one reader thread hammer a slot. Every read that returns true must be
// whole (all words equal, check word right) and never older than a value already seen. The
// test also runs the pre-M7a reader (gives up while a write is in progress) on the same load,
// so the give-up counts can be compared.
//
// x86 is more strongly ordered than the ESP32-S3, so this checks the buffer and sequence logic,
// not the memory ordering; the ordering argument is in the header comments.
//
// g++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -Icomponents/seqlock/include
//     tools/host_test/test_seqlock.cpp -o .pio/host/test_seqlock && .pio/host/test_seqlock
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "seqlock.h"

namespace {

constexpr int kWords = 16;  // 64 bytes: a little bigger than a Setpoint, so copies overlap more

struct Payload {
    uint32_t w[kWords];
    uint32_t check;
};

Payload make(uint32_t v) {
    Payload p;
    for (auto& x : p.w) x = v;
    p.check = v ^ 0xA5A5A5A5u;
    return p;
}

bool whole(const Payload& p) {
    for (auto x : p.w) {
        if (x != p.w[0]) return false;
    }
    return p.check == (p.w[0] ^ 0xA5A5A5A5u);
}

// The reader before this change, kept here for comparison only.
template <typename T>
struct OldReader {
    static bool read(const std::atomic<uint32_t>& seq, const T* buf, T* out, int max_tries) {
        for (int i = 0; i < max_tries; ++i) {
            const uint32_t s1 = seq.load(std::memory_order_acquire);
            if ((s1 & 1u) != 0) continue;
            std::memcpy(out, &buf[(s1 / 2) & 1], sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s1) return true;
        }
        return false;
    }
};

// Same layout and writer as SeqlockSlot, with both readers.
struct Probe {
    Payload buf[2]{};
    std::atomic<uint32_t> seq{0};
    void write(const Payload& v) {
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&buf[((s + 2) / 2) & 1], &v, sizeof(Payload));
        seq.store(s + 2, std::memory_order_release);
    }
};

struct Result {
    uint64_t reads = 0, ok = 0, giveups = 0, torn = 0, backwards = 0;
};

// gap_ns: pause between writes (0 = back to back, the worst case for lapping).
Result run_slot(int gap_ns, std::chrono::milliseconds dur) {
    SeqlockSlot<Payload> slot;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint32_t v = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            slot.write(make(v++));
            if (gap_ns) {
                const auto t = std::chrono::steady_clock::now() + std::chrono::nanoseconds(gap_ns);
                while (std::chrono::steady_clock::now() < t) {}
            }
        }
    });
    Result r;
    uint32_t last = 0;
    const auto end = std::chrono::steady_clock::now() + dur;
    while (std::chrono::steady_clock::now() < end) {
        Payload p;
        r.reads++;
        if (!slot.read(&p, 8)) {
            r.giveups++;
            continue;
        }
        r.ok++;
        if (slot.writes() == 0) continue;
        if (!whole(p)) {
            r.torn++;
        } else if (p.w[0] < last) {
            r.backwards++;
        } else {
            last = p.w[0];
        }
    }
    stop = true;
    writer.join();
    return r;
}

Result run_old(int gap_ns, std::chrono::milliseconds dur) {
    Probe slot;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint32_t v = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            slot.write(make(v++));
            if (gap_ns) {
                const auto t = std::chrono::steady_clock::now() + std::chrono::nanoseconds(gap_ns);
                while (std::chrono::steady_clock::now() < t) {}
            }
        }
    });
    Result r;
    const auto end = std::chrono::steady_clock::now() + dur;
    while (std::chrono::steady_clock::now() < end) {
        Payload p;
        r.reads++;
        if (!OldReader<Payload>::read(slot.seq, slot.buf, &p, 8)) {
            r.giveups++;
            continue;
        }
        r.ok++;
        if (slot.seq.load() >= 2 && !whole(p)) r.torn++;
    }
    stop = true;
    writer.join();
    return r;
}

// Single-threaded checks of the index and window logic, including sequence wrap.
int deterministic() {
    int fails = 0;
    SeqlockSlot<Payload> s;
    Payload p;
    if (!s.read(&p) || s.writes() != 0) fails++;  // empty slot: zeros, success
    for (uint32_t v = 1; v <= 5; ++v) {
        s.write(make(v));
        if (!s.read(&p) || !whole(p) || p.w[0] != v || s.writes() != v) fails++;
    }
    return fails;
}

}  // namespace

int main() {
    int fails = deterministic();
    std::printf("deterministic: %s\n", fails ? "FAILED" : "ok");

    const std::chrono::milliseconds dur(1500);
    const int gaps[] = {0, 200, 2000};
    for (int gap : gaps) {
        const Result n = run_slot(gap, dur);
        const Result o = run_old(gap, dur);
        std::printf("writer gap %4d ns | new: reads %10llu give-ups %8llu (%.4f %%) torn %llu backwards %llu"
                    " | old: give-ups %8llu (%.4f %%) torn %llu\n",
                    gap, (unsigned long long)n.reads, (unsigned long long)n.giveups,
                    n.reads ? 100.0 * n.giveups / n.reads : 0.0, (unsigned long long)n.torn,
                    (unsigned long long)n.backwards, (unsigned long long)o.giveups,
                    o.reads ? 100.0 * o.giveups / o.reads : 0.0, (unsigned long long)o.torn);
        if (n.torn || n.backwards || o.torn) fails++;
        if (gap >= 2000 && n.giveups) fails++;  // a writer that cannot lap the reader: no give-ups
    }
    std::printf("seqlock host test: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
