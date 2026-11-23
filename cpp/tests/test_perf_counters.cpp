// PerfCounters must degrade gracefully: CI runners and containers often
// forbid perf_event_open, and then every event reads as unavailable rather
// than as zero. Where counting works, a loop of known length must count at
// least that many instructions.
#include <gtest/gtest.h>

#include <cstdint>

#include "orderflow/perf_counters.hpp"

using orderflow::PerfCounters;

namespace {
volatile std::uint64_t g_sink = 0;
}

TEST(PerfCounters, CountsAKnownLoopOrReportsUnavailable) {
    PerfCounters c;
    c.start();
    for (std::uint64_t i = 0; i < 1'000'000; ++i) g_sink = g_sink + i;  // >= 1M instructions
    c.stop();
    const auto r = c.read();
    if (!c.any_available()) {
        for (std::size_t i = 0; i < PerfCounters::kCount; ++i) EXPECT_FALSE(r.available[i]);
        GTEST_SKIP() << "perf_event_open is not permitted here";
    }
    if (r.available[PerfCounters::instructions] && r.running[PerfCounters::instructions] > 0) {
        EXPECT_GE(r.value[PerfCounters::instructions], 1e6);
    }
    for (std::size_t i = 0; i < PerfCounters::kCount; ++i) {
        EXPECT_GE(r.running[i], 0.0);
        EXPECT_LE(r.running[i], 1.0 + 1e-9);
    }
}

TEST(PerfCounters, AccumulatesAcrossStartStop) {
    PerfCounters c;
    if (!c.any_available()) GTEST_SKIP() << "perf_event_open is not permitted here";
    c.start();
    for (std::uint64_t i = 0; i < 100'000; ++i) g_sink = g_sink + i;
    c.stop();
    const double first = c.read().value[PerfCounters::instructions];
    c.start();
    for (std::uint64_t i = 0; i < 100'000; ++i) g_sink = g_sink + i;
    c.stop();
    EXPECT_GT(c.read().value[PerfCounters::instructions], first);
}
