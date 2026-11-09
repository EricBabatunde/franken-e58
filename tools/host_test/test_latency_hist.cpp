// Host test for the M7c latency histograms (components/latency/include/latency_hist.h), the same
// header the firmware compiles. Each percentile is compared with the exact nearest-rank value of
// the same samples, sorted: the histogram result must be an upper bound within one bin (or the
// exact maximum in the overflow bin).
//
// g++ -std=c++17 -O2 -Wall -Wextra -Werror -Icomponents/latency/include
//     tools/host_test/test_latency_hist.cpp -o .pio/host/test_latency_hist && .pio/host/test_latency_hist
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "latency_hist.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        if (g_failed <= 20) std::printf("FAIL: %s\n", what.c_str());
    }
}

// Exact nearest-rank quantile: the sample of rank ceil(p * n), 1-based.
uint32_t exact(std::vector<uint32_t> v, uint32_t num, uint32_t den) {
    std::sort(v.begin(), v.end());
    uint64_t rank = (static_cast<uint64_t>(v.size()) * num + den - 1) / den;
    if (rank == 0) rank = 1;
    return v[rank - 1];
}

template <class H>
void compare(const H& h, const std::vector<uint32_t>& v, const std::string& tag) {
    const uint32_t ps[][2] = {{1, 2}, {99, 100}, {999, 1000}, {1, 1}, {1, 1000}};
    for (const auto& p : ps) {
        const uint32_t got = latency::percentile(h, p[0], p[1]);
        const uint32_t ref = exact(v, p[0], p[1]);
        const bool in_overflow = ref >= H::kBins * H::kBinUs;
        const bool ok = in_overflow ? got == h.max : (got >= ref && got <= ref + H::kBinUs - 1 && got <= h.max);
        check(ok, tag + " p" + std::to_string(p[0]) + "/" + std::to_string(p[1]) + ": got " + std::to_string(got) +
                      " exact " + std::to_string(ref));
    }
    uint64_t sum = 0;
    uint32_t mx = 0;
    for (uint32_t x : v) {
        sum += x;
        mx = std::max(mx, x);
    }
    check(h.count == v.size() && h.sum == sum && h.max == mx, tag + ": count, sum and max exact");
}

void test_basics() {
    latency::Hist<10, 1> h;
    h.clear();
    check(latency::percentile(h, 999, 1000) == 0 && h.count == 0, "empty: 0");
    h.add(3);
    check(latency::percentile(h, 1, 2) == 3 && latency::percentile(h, 999, 1000) == 3, "one sample: itself");
    h.add(500);  // overflow
    check(h.overflow() == 1 && h.max == 500, "overflow bin and exact max");
    check(latency::percentile(h, 1, 1) == 500, "p100 in the overflow bin: the exact max");
    check(latency::percentile(h, 1, 2) == 3, "p50 of {3, 500}: 3");
    h.clear();
    check(h.count == 0 && h.max == 0 && h.sum == 0 && h.overflow() == 0, "clear");

    latency::Hist<150, 10> b;
    b.clear();
    b.add(455);
    b.add(459);
    check(latency::percentile(b, 1, 2) == 459, "10 us bins: bin 45 top is 459");
    b.add(1499);
    b.add(1500);  // first overflow value
    check(b.overflow() == 1 && b.bin[149] == 1, "1499 in the last bin, 1500 in overflow");
}

template <uint32_t N, uint32_t B>
void test_random(const std::string& name, uint32_t lo, uint32_t hi, int runs, uint32_t seed) {
    std::mt19937 rng(seed);
    for (int r = 0; r < runs; ++r) {
        latency::Hist<N, B> h;
        h.clear();
        std::vector<uint32_t> v;
        const int n = 1 + static_cast<int>(rng() % 5000);
        // Mostly a narrow peak (as the loop jitter), with a tail and occasional outliers.
        for (int i = 0; i < n; ++i) {
            uint32_t x;
            const uint32_t k = rng() % 1000;
            if (k < 950) {
                x = lo + rng() % ((hi - lo) / 10 + 1);
            } else if (k < 998) {
                x = lo + rng() % (hi - lo + 1);
            } else {
                x = hi + rng() % (3 * hi + 1);  // overflow region
            }
            v.push_back(x);
            h.add(x);
        }
        compare(h, v, name + " run " + std::to_string(r));
    }
}

// The firmware's counters are 32-bit and the condition runs are short (20 min soak: 1.2 M
// samples), so a count wrap cannot happen in a run; the rank uses 64-bit arithmetic regardless.
void test_large_count() {
    latency::Hist<200, 1> h;
    h.clear();
    h.bin[5] = 4000000000u;
    h.bin[150] = 8000000u;  // 0.2 % of the samples: above the p99.9 rank
    h.count = 4008000000u;
    h.max = 150;
    check(latency::percentile(h, 999, 1000) == 150, "large count: p99.9 rank in 64 bits");
    check(latency::percentile(h, 99, 100) == 5, "large count: p99");
    h.bin[150] = 2000000u;  // 0.05 %: below it
    h.count = 4002000000u;
    check(latency::percentile(h, 999, 1000) == 5, "large count: p99.9 below a 0.05 % tail");
}

}  // namespace

int main() {
    test_basics();
    test_random<200, 1>("period 1 us", 0, 60, 300, 11);
    test_random<100, 1>("wake 1 us", 5, 40, 300, 12);
    test_random<150, 10>("burst 10 us", 440, 900, 300, 13);
    test_random<100, 10>("work 10 us", 450, 950, 300, 14);
    test_large_count();
    std::printf("latency histogram host test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
