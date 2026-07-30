#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <x86intrin.h>
#define LOB_HAS_RDTSC 1
#endif

namespace lob::bench {

// Reads the CPU timestamp counter where available (single-instruction, ~sub-ns
// resolution: the only clock fine-grained enough to time individual order-book
// operations, which run in tens of nanoseconds). Falls back to steady_clock ns
// on non-x86. The unit is "ticks"; calibrate() converts ticks to nanoseconds.
inline std::uint64_t now_ticks() noexcept {
#if LOB_HAS_RDTSC
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Nanoseconds per tick, measured by busy-spinning the tick counter across a known
// wall-clock interval. On non-x86 (steady_clock ns) this is 1.0.
inline double calibrate_ns_per_tick() {
#if LOB_HAS_RDTSC
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = now_ticks();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count() < 200) {
        // spin
    }
    const std::uint64_t c1 = now_ticks();
    const auto t1 = clock::now();
    const double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return ns / static_cast<double>(c1 - c0);
#else
    return 1.0;
#endif
}

// Records per-operation latency samples (in ticks) and reports percentiles. Kept
// deliberately simple: store every sample, sort once at report time. A few
// million 8-byte samples is a trivial amount of memory and keeps percentiles
// exact rather than bucket-approximated.
class Histogram {
public:
    void reserve(std::size_t n) { samples_.reserve(n); }
    void add(std::uint64_t ticks) { samples_.push_back(ticks); }
    [[nodiscard]] std::size_t count() const { return samples_.size(); }

    // Percentile in ticks (p in [0,1]); call after all samples are recorded.
    [[nodiscard]] std::uint64_t percentile_ticks(double p) {
        if (samples_.empty()) return 0;
        if (!sorted_) {
            std::sort(samples_.begin(), samples_.end());
            sorted_ = true;
        }
        const std::size_t n = samples_.size();
        auto idx = static_cast<std::size_t>(p * static_cast<double>(n - 1));
        if (idx >= n) idx = n - 1;
        return samples_[idx];
    }

    [[nodiscard]] double percentile_ns(double p, double ns_per_tick) {
        return static_cast<double>(percentile_ticks(p)) * ns_per_tick;
    }

private:
    std::vector<std::uint64_t> samples_;
    bool sorted_ = false;
};

}  // namespace lob::bench
