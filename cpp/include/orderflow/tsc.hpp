// Timing short code regions with the CPU's time-stamp counter (TSC).
//
// rdtsc reads a counter that ticks at a constant rate (on every CPU since
// about 2008, "invariant TSC"), at a cost of ~20 cycles. That is cheap enough
// to time one message or one model evaluation, where steady_clock's
// ~20-30 ns would be most of the measurement. On its own, rdtsc can execute
// out of order with the code around it, so:
//   start: lfence, rdtsc        nothing earlier is still running
//   stop:  rdtscp, lfence       the timed work has finished; nothing later started
// (Intel's "How to Benchmark Code Execution Times"; on AMD Zen, lfence is
// dispatch-serialising, so the same pattern holds.)
#pragma once

#include <x86intrin.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace orderflow {

inline std::uint64_t tsc_start() noexcept {
    _mm_lfence();
    return __rdtsc();
}

inline std::uint64_t tsc_stop() noexcept {
    unsigned aux = 0;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// TSC ticks per nanosecond, measured against steady_clock over `ms`.
inline double tsc_per_ns(int ms = 200) {
    const auto c0 = std::chrono::steady_clock::now();
    const auto t0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    const auto t1 = __rdtsc();
    const auto c1 = std::chrono::steady_clock::now();
    return static_cast<double>(t1 - t0) /
           static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
}

}  // namespace orderflow
