#include "orderflow/order_map.hpp"

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace orderflow;

template <class M>
class OrderMapTest : public ::testing::Test {};
using Maps = ::testing::Types<StdOrderMap<int>, OpenAddressingMap<int>>;
TYPED_TEST_SUITE(OrderMapTest, Maps);

TYPED_TEST(OrderMapTest, InsertFindErase) {
    TypeParam m(4);
    EXPECT_EQ(m.find(7), nullptr);
    EXPECT_TRUE(m.insert(7, 70));
    EXPECT_FALSE(m.insert(7, 71));  // duplicate keys are refused, value unchanged
    ASSERT_NE(m.find(7), nullptr);
    EXPECT_EQ(*m.find(7), 70);
    *m.find(7) = 72;                // find gives a mutable reference into the map
    EXPECT_EQ(*m.find(7), 72);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.erase(7));
    EXPECT_FALSE(m.erase(7));
    EXPECT_EQ(m.find(7), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TYPED_TEST(OrderMapTest, GrowsPastInitialCapacity) {
    TypeParam m(4);
    for (std::uint64_t k = 1; k <= 10'000; ++k) ASSERT_TRUE(m.insert(k, static_cast<int>(k)));
    EXPECT_EQ(m.size(), 10'000u);
    for (std::uint64_t k = 1; k <= 10'000; ++k) {
        ASSERT_NE(m.find(k), nullptr);
        ASSERT_EQ(*m.find(k), static_cast<int>(k));
    }
}

TEST(OpenAddressingMap, StaysAtMostHalfFull) {
    OpenAddressingMap<int> m(4);
    for (std::uint64_t k = 1; k <= 1000; ++k) {
        m.insert(k, 0);
        ASSERT_LE(m.size() * 2, m.capacity());
    }
}

// Differential test against std::unordered_map, sized so that the table stays
// small and dense: long probe runs, wrap-around at the end of the array, and
// many backward shifts. Keys mix ITCH-like sequential numbers and random ones.
TEST(OpenAddressingMap, AgreesWithUnorderedMapUnderChurn) {
    for (std::uint32_t seed = 1; seed <= 5; ++seed) {
        std::mt19937_64 rng(seed);
        OpenAddressingMap<int> m(8);
        std::unordered_map<std::uint64_t, int> ref;
        std::vector<std::uint64_t> keys;
        std::uint64_t next_seq = 1;
        for (int step = 0; step < 200'000; ++step) {
            const auto roll = rng() % 100;
            if (keys.empty() || roll < 50) {
                const std::uint64_t k = (roll % 2 == 0) ? next_seq++ : 1 + rng() % 5'000;
                const int v = static_cast<int>(rng() % 1000);
                const bool inserted = m.insert(k, v);
                ASSERT_EQ(inserted, ref.try_emplace(k, v).second) << "seed " << seed;
                if (inserted) keys.push_back(k);
            } else if (roll < 85) {
                const std::size_t i = rng() % keys.size();
                ASSERT_TRUE(m.erase(keys[i])) << "seed " << seed << " step " << step;
                ref.erase(keys[i]);
                keys[i] = keys.back();
                keys.pop_back();
            } else {
                const std::uint64_t k = 1 + rng() % (next_seq + 5'000);
                const int* got = m.find(k);
                auto it = ref.find(k);
                ASSERT_EQ(got == nullptr, it == ref.end()) << "seed " << seed << " key " << k;
                if (got != nullptr) {
                    ASSERT_EQ(*got, it->second);
                }
            }
            ASSERT_EQ(m.size(), ref.size());
        }
        // Every key still reachable after all the shifting.
        for (const auto& [k, v] : ref) {
            ASSERT_NE(m.find(k), nullptr) << "seed " << seed << " lost key " << k;
            ASSERT_EQ(*m.find(k), v);
        }
    }
}

}  // namespace
