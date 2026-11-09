#pragma once
#include <cstdint>

// M7c latency histograms. Static, fixed-size, no heap, no library calls: updated on Core 1 by the
// flight task and read on Core 0 from a seqlock copy. The same header is compiled by the host
// test (tools/host_test/test_latency_hist.cpp).
//
// Values are whole microseconds. Bin i holds [i * BinUs, (i + 1) * BinUs - 1]; bin Bins is the
// overflow bin (everything from Bins * BinUs up). The exact maximum and the sum are kept too.
namespace latency {

template <uint32_t Bins, uint32_t BinUs>
struct Hist {
    static constexpr uint32_t kBins = Bins;
    static constexpr uint32_t kBinUs = BinUs;

    uint32_t bin[Bins + 1];
    uint32_t count;
    uint32_t max;
    uint64_t sum;

    void clear() {
        for (uint32_t i = 0; i <= Bins; ++i) bin[i] = 0;
        count = 0;
        max = 0;
        sum = 0;
    }

    void add(uint32_t v) {
        uint32_t i = v / BinUs;
        if (i > Bins) i = Bins;
        bin[i]++;
        count++;
        sum += v;
        if (v > max) max = v;
    }

    uint32_t overflow() const { return bin[Bins]; }
};

// Upper bound of the p-quantile (p = num / den, 0 < p <= 1), nearest-rank method: the largest
// value of the bin holding the sample of rank ceil(p * count), capped at the exact maximum. So
// the true quantile q satisfies q <= result <= q + BinUs - 1, and result == max in the overflow
// bin. Returns 0 for an empty histogram.
template <class H>
uint32_t percentile(const H& h, uint32_t num, uint32_t den) {
    if (h.count == 0 || den == 0) return 0;
    uint64_t rank = (static_cast<uint64_t>(h.count) * num + den - 1) / den;
    if (rank == 0) rank = 1;
    if (rank > h.count) rank = h.count;
    uint64_t seen = 0;
    for (uint32_t i = 0; i <= H::kBins; ++i) {
        seen += h.bin[i];
        if (seen >= rank) {
            if (i == H::kBins) return h.max;
            const uint32_t top = (i + 1) * H::kBinUs - 1;
            return top < h.max ? top : h.max;
        }
    }
    return h.max;
}

template <class H>
double mean(const H& h) {
    return h.count ? static_cast<double>(h.sum) / h.count : 0.0;
}

}  // namespace latency
