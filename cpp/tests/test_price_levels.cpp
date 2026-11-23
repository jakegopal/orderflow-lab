#include "orderflow/price_levels.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace orderflow;

template <class L>
class PriceLevelsTest : public ::testing::Test {};

using Implementations = ::testing::Types<MapLevels<Side::buy>, FlatLevels<Side::buy, 64>,
                                         MapLevels<Side::sell>, FlatLevels<Side::sell, 64>>;
TYPED_TEST_SUITE(PriceLevelsTest, Implementations);

TYPED_TEST(PriceLevelsTest, EmptyHasNoBest) {
    TypeParam levels;
    EXPECT_FALSE(levels.best().has_value());
    EXPECT_EQ(levels.size(), 0u);
}

TYPED_TEST(PriceLevelsTest, AggregatesOrdersAtOnePrice) {
    TypeParam levels;
    levels.add(1'000'000, 100);
    levels.add(1'000'000, 50);
    EXPECT_EQ(levels.at(1'000'000), (Level{150, 2}));
    levels.reduce(1'000'000, 30, false);  // partial execution: order stays
    EXPECT_EQ(levels.at(1'000'000), (Level{120, 2}));
    levels.reduce(1'000'000, 70, true);   // the 100-share order's last 70 shares
    EXPECT_EQ(levels.at(1'000'000), (Level{50, 1}));
    levels.reduce(1'000'000, 50, true);
    EXPECT_EQ(levels.at(1'000'000), (Level{0, 0}));
    EXPECT_EQ(levels.size(), 0u);
}

TYPED_TEST(PriceLevelsTest, BestIsHighestBidOrLowestAsk) {
    TypeParam levels;
    for (Price p : {1'000'000u, 1'000'300u, 999'900u}) levels.add(p, 10);
    constexpr bool bids = std::is_same_v<TypeParam, MapLevels<Side::buy>> ||
                          std::is_same_v<TypeParam, FlatLevels<Side::buy, 64>>;
    const Price want = bids ? 1'000'300u : 999'900u;
    ASSERT_TRUE(levels.best().has_value());
    EXPECT_EQ(levels.best()->price, want);
    levels.reduce(want, 10, true);  // best level leaves: the next one takes over
    EXPECT_EQ(levels.best()->price, 1'000'000u);
}

TYPED_TEST(PriceLevelsTest, FarAndSubPennyPricesAreKept) {
    TypeParam levels;
    levels.add(1'000'000, 1);  // $100.00
    levels.add(100, 2);        // $0.01 stub quote, far outside any window
    levels.add(1'999'999'900, 3);  // $199,999.99 stub
    levels.add(1'000'050, 4);  // $100.005, sub-penny
    EXPECT_EQ(levels.size(), 4u);
    EXPECT_EQ(levels.at(100), (Level{2, 1}));
    EXPECT_EQ(levels.at(1'999'999'900), (Level{3, 1}));
    EXPECT_EQ(levels.at(1'000'050), (Level{4, 1}));
    EXPECT_EQ(levels.levels().size(), 4u);
}

TEST(FlatLevels, RecentersWhenTheMarketLeavesTheWindow) {
    FlatLevels<Side::buy, 64> bids;  // 64 one-cent slots: a $0.64 window
    bids.add(1'000'000, 10);         // $100.00 anchors the window
    bids.add(1'100'000, 20);         // $110.00: outside, and better, so the window moves
    EXPECT_EQ(bids.recenters(), 1u);
    EXPECT_EQ(bids.best()->price, 1'100'000u);
    EXPECT_EQ(bids.overflow_levels(), 1u);  // $100.00 is now the far level
    bids.reduce(1'100'000, 20, true);       // best gone: $100.00 is best again, recenter back
    EXPECT_EQ(bids.best()->price, 1'000'000u);
    EXPECT_EQ(bids.recenters(), 2u);
    EXPECT_EQ(bids.overflow_levels(), 0u);
}

TEST(FlatLevels, FarWorsePricesDoNotRecenter) {
    FlatLevels<Side::sell, 64> asks;
    asks.add(1'000'000, 1);
    asks.add(1'999'999'900, 1);  // stub ask far above: worse, stays in overflow
    EXPECT_EQ(asks.recenters(), 0u);
    EXPECT_EQ(asks.best()->price, 1'000'000u);
}

// Everything walk() visits, best to worst; stops after `limit` levels.
template <class L>
std::vector<PriceLevel> walked(const L& levels, std::size_t limit = SIZE_MAX) {
    std::vector<PriceLevel> out;
    levels.walk([&](const PriceLevel& l) {
        out.push_back(l);
        return out.size() < limit;
    });
    return out;
}

// Differential test: FlatLevels must agree with MapLevels after every operation
// on long random sequences that exercise drift, recentering, stubs and sub-penny.
template <Side S>
void random_agreement(std::uint32_t seed) {
    std::mt19937 rng(seed);
    MapLevels<S> ref;
    FlatLevels<S, 128> flat;
    struct Live { Price price; std::uint32_t shares; };
    std::vector<Live> live;
    Price center = 5'000'000;  // $500.00

    for (int step = 0; step < 20'000; ++step) {
        const auto roll = rng() % 100;
        if (roll < 2) {  // the market drifts by up to $5
            const auto move = static_cast<Price>(rng() % 1001) * 100;
            center = (rng() % 2 != 0) ? center + move : center - std::min(center - 100'000, move);
        }
        if (live.empty() || roll < 55) {
            Price p = center - 200 * 100 + static_cast<Price>(rng() % 400) * 100;
            if (roll % 17 == 0) p = 100;                          // far stub
            if (roll % 23 == 0) p += 37;                          // sub-penny
            const auto q = 1 + static_cast<std::uint32_t>(rng() % 500);
            ref.add(p, q);
            flat.add(p, q);
            live.push_back({p, q});
        } else {
            const std::size_t k = rng() % live.size();
            Live& o = live[k];
            const bool all = rng() % 3 != 0 || o.shares == 1;
            const std::uint32_t q = all ? o.shares : 1 + static_cast<std::uint32_t>(rng() % (o.shares - 1));
            ref.reduce(o.price, q, all);
            flat.reduce(o.price, q, all);
            if (all) {
                live[k] = live.back();
                live.pop_back();
            } else {
                o.shares -= q;
            }
        }
        ASSERT_EQ(ref.best(), flat.best()) << "seed " << seed << " step " << step;
        ASSERT_EQ(ref.size(), flat.size()) << "seed " << seed << " step " << step;
        if (step % 500 == 0) {
            ASSERT_EQ(ref.levels(), flat.levels()) << "seed " << seed;
            // walk() visits the same levels in the same order, window and
            // overflow merged, and stops when told to.
            ASSERT_EQ(walked(flat), ref.levels()) << "seed " << seed << " step " << step;
            ASSERT_EQ(walked(ref), ref.levels()) << "seed " << seed << " step " << step;
            const auto first3 = walked(flat, 3);
            ASSERT_EQ(first3.size(), std::min<std::size_t>(3, ref.size()));
            ASSERT_TRUE(std::equal(first3.begin(), first3.end(), ref.levels().begin()));
        }
    }
    EXPECT_EQ(ref.levels(), flat.levels());
    EXPECT_GT(flat.recenters(), 0u);  // the test must actually exercise recentering
}

TEST(FlatLevels, AgreesWithMapLevelsBids) {
    for (std::uint32_t seed = 1; seed <= 5; ++seed) random_agreement<Side::buy>(seed);
}
TEST(FlatLevels, AgreesWithMapLevelsAsks) {
    for (std::uint32_t seed = 1; seed <= 5; ++seed) random_agreement<Side::sell>(seed);
}

}  // namespace
