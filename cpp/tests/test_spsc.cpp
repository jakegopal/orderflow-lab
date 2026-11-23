// SPSC ring: single-threaded semantics (empty, full, wrap-around), then a
// two-thread stress test. The stress test is the one TSan checks in CI: with
// a wrong memory ordering it reports a data race on the slots even when the
// values happen to come out right on x86.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <thread>

#include "orderflow/spsc.hpp"

namespace {

using orderflow::SpscRing;
using orderflow::SpscRingNoCache;

TEST(Spsc, EmptyFullAndWrapAround) {
    SpscRing<int, 4> q;
    int v = 0;
    EXPECT_FALSE(q.try_pop(v));
    for (int lap = 0; lap < 3; ++lap) {  // the counters run past the capacity several times
        for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(lap * 10 + i));
        EXPECT_FALSE(q.try_push(99)) << "a full ring must refuse";
        EXPECT_EQ(q.size_approx(), 4u);
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.try_pop(v));
            EXPECT_EQ(v, lap * 10 + i);  // FIFO
        }
        EXPECT_FALSE(q.try_pop(v));
    }
}

TEST(Spsc, MovesNonTrivialValues) {
    SpscRing<std::string, 8> q;
    EXPECT_TRUE(q.try_push(std::string(100, 'x')));  // longer than the small-string buffer
    std::string out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.size(), 100u);
}

template <class Q>
void stress() {
    constexpr std::uint64_t n = 2'000'000;
    Q q;
    std::uint64_t bad = 0;
    std::thread consumer([&] {
        std::uint64_t expect = 0, v = 0;
        while (expect < n) {
            if (q.try_pop(v)) {
                bad += v != expect;
                ++expect;
            }
        }
    });
    for (std::uint64_t i = 0; i < n;) {
        if (q.try_push(i)) ++i;
    }
    consumer.join();
    EXPECT_EQ(bad, 0u);
}

TEST(Spsc, TwoThreadsEveryValueInOrder) { stress<SpscRing<std::uint64_t, 64>>(); }
TEST(Spsc, TwoThreadsUnpadded) { stress<SpscRing<std::uint64_t, 64, false>>(); }
TEST(Spsc, TwoThreadsNoCache) { stress<SpscRingNoCache<std::uint64_t, 64>>(); }

}  // namespace
