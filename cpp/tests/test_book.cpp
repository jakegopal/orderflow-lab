#include "orderflow/book.hpp"

#include <cstdint>
#include <algorithm>
#include <array>
#include <map>
#include <random>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "itch_builder.hpp"
#include "orderflow/itch.hpp"

namespace {

using namespace orderflow;
namespace t = orderflow::test;

template <class Book>
void feed(Book& book, const std::vector<std::byte>& msg) {
    ASSERT_EQ(itch::parse(msg, book), itch::ParseStatus::ok);
}

template <class B>
class BookTest : public ::testing::Test {};
using Books = ::testing::Types<MapStdBook, MapOpenBook, FlatStdBook, FlatOpenBook>;
TYPED_TEST_SUITE(BookTest, Books);

constexpr Locate kAapl = 13;
constexpr Locate kMsft = 14;

TYPED_TEST(BookTest, AddsAggregateByPriceAndSymbol) {
    TypeParam book(16);
    feed(book, t::add(kAapl, 1, 'B', 100, 1'500'000));  // bid 100 @ $150.00
    feed(book, t::add(kAapl, 2, 'B', 200, 1'500'000));  // bid 200 @ $150.00
    feed(book, t::add(kAapl, 3, 'S', 50, 1'500'100));   // ask  50 @ $150.01
    feed(book, t::add(kMsft, 4, 'B', 7, 1'000'000));    // another symbol
    const auto& aapl = book.book(kAapl);
    EXPECT_EQ(aapl.bids.best(), (PriceLevel{1'500'000, {300, 2}}));
    EXPECT_EQ(aapl.asks.best(), (PriceLevel{1'500'100, {50, 1}}));
    EXPECT_EQ(book.book(kMsft).bids.best(), (PriceLevel{1'000'000, {7, 1}}));
    EXPECT_EQ(book.live_orders(), 4u);
}

TYPED_TEST(BookTest, ExecutionsAndCancelsReduceTheOrderAndItsLevel) {
    TypeParam book(16);
    feed(book, t::add(kAapl, 1, 'B', 100, 1'500'000));
    feed(book, t::add(kAapl, 2, 'B', 200, 1'500'000));
    feed(book, t::executed(kAapl, 1, 30));                           // 70 left on order 1
    feed(book, t::executed_with_price(kAapl, 2, 50, 1'499'900, true));  // 150 left on order 2
    feed(book, t::cancel(kAapl, 2, 100));                            // 50 left on order 2
    EXPECT_EQ(book.book(kAapl).bids.at(1'500'000), (Level{120, 2}));
    EXPECT_EQ(book.order(1)->shares, 70u);
    EXPECT_EQ(book.order(2)->shares, 50u);
}

TYPED_TEST(BookTest, FullExecutionRemovesTheOrderWithoutADelete) {
    TypeParam book(16);
    feed(book, t::add(kAapl, 1, 'S', 100, 1'500'100));
    feed(book, t::executed(kAapl, 1, 100));
    EXPECT_FALSE(book.book(kAapl).asks.best().has_value());
    EXPECT_EQ(book.order(1), nullptr);
    EXPECT_EQ(book.live_orders(), 0u);
    EXPECT_EQ(book.anomalies().total(), 0u);
}

TYPED_TEST(BookTest, DeleteRemovesAllRemainingShares) {
    TypeParam book(16);
    feed(book, t::add(kAapl, 1, 'B', 100, 1'500'000));
    feed(book, t::add(kAapl, 2, 'B', 40, 1'499'900));
    feed(book, t::executed(kAapl, 1, 10));
    feed(book, t::del(kAapl, 1));
    EXPECT_EQ(book.book(kAapl).bids.best(), (PriceLevel{1'499'900, {40, 1}}));
    EXPECT_EQ(book.live_orders(), 1u);
}

TYPED_TEST(BookTest, ReplaceMovesTheOrderToANewReferencePriceAndSize) {
    TypeParam book(16);
    feed(book, t::add(kAapl, 1, 'S', 100, 1'500'100));
    feed(book, t::replace(kAapl, 1, 9, 60, 1'500'200));
    EXPECT_EQ(book.order(1), nullptr);
    ASSERT_NE(book.order(9), nullptr);
    EXPECT_EQ(book.order(9)->side, Side::sell);  // side and symbol carry over
    EXPECT_EQ(book.order(9)->locate, kAapl);
    EXPECT_EQ(book.book(kAapl).asks.levels(),
              (std::vector<PriceLevel>{{1'500'200, {60, 1}}}));
}

TYPED_TEST(BookTest, HiddenTradesDoNotTouchTheBook) {
    TypeParam book(16);
    feed(book, t::add(kAapl, 1, 'B', 100, 1'500'000));
    feed(book, t::trade(kAapl, 'B', 500, 1'500'000, 1));
    EXPECT_EQ(book.book(kAapl).bids.at(1'500'000), (Level{100, 1}));
}

TYPED_TEST(BookTest, AnomaliesAreCountedNotFatal) {
    TypeParam book(16);
    feed(book, t::add(kAapl, 1, 'B', 100, 1'500'000));
    feed(book, t::add(kAapl, 1, 'B', 100, 1'500'000));  // duplicate reference
    feed(book, t::add(kAapl, 0, 'B', 100, 1'500'000));  // reserved reference
    feed(book, t::del(kAapl, 77));                      // never added
    feed(book, t::replace(kAapl, 78, 79, 1, 1));        // never added: nothing is created
    feed(book, t::executed(kAapl, 1, 101));             // more than the 100 resting
    const Anomalies& a = book.anomalies();
    EXPECT_EQ(a.duplicate_order, 1u);
    EXPECT_EQ(a.zero_ref, 1u);
    EXPECT_EQ(a.unknown_order, 2u);
    EXPECT_EQ(a.over_reduction, 1u);
    EXPECT_EQ(book.order(79), nullptr);
    EXPECT_EQ(book.live_orders(), 0u);  // the over-executed order is gone, its level too
    EXPECT_FALSE(book.book(kAapl).bids.best().has_value());
}

// Reference model: the orders alone, with levels recomputed from scratch by
// summing them. Obviously correct and far too slow for real use.
struct ReferenceBook {
    std::map<OrderRef, RestingOrder> orders;
    [[nodiscard]] std::vector<PriceLevel> levels(Locate locate, Side side) const {
        std::map<Price, Level> by_price;
        for (const auto& [ref, o] : orders) {
            if (o.locate != locate || o.side != side) continue;
            Level& l = by_price[o.price];
            l.shares += o.shares;
            ++l.orders;
        }
        std::vector<PriceLevel> out;
        for (const auto& [p, l] : by_price) out.push_back({p, l});
        if (side == Side::buy) std::reverse(out.begin(), out.end());  // best first
        return out;
    }
};

// Random but valid ITCH streams over three symbols: every variant must equal
// the reference after every message.
TEST(OrderBook, AllVariantsMatchTheReferenceOnRandomStreams) {
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        std::mt19937 rng(seed);
        ReferenceBook ref;
        MapStdBook a(16);
        MapOpenBook b(16);
        FlatStdBook c(16);
        FlatOpenBook d(16);
        std::vector<OrderRef> live;
        OrderRef next_ref = 1;
        const std::array<Locate, 3> symbols{1, 2, 3};
        const std::array<Price, 3> mids{100'000, 1'500'000, 30'000'000};  // $10, $150, $3000

        for (int step = 0; step < 6'000; ++step) {
            std::vector<std::byte> msg;
            const auto roll = rng() % 100;
            if (live.empty() || roll < 45) {
                const std::size_t s = rng() % 3;
                const bool buy = rng() % 2 == 0;
                const auto offset = static_cast<Price>(1 + rng() % 300) * 100;
                const Price p = buy ? mids[s] - offset : mids[s] + offset;
                const auto q = 1 + static_cast<std::uint32_t>(rng() % 1000);
                const OrderRef r = next_ref++;
                msg = t::add(symbols[s], r, buy ? 'B' : 'S', q, p);
                ref.orders[r] = RestingOrder{p, q, symbols[s], buy ? Side::buy : Side::sell};
                live.push_back(r);
            } else {
                const std::size_t k = rng() % live.size();
                const OrderRef r = live[k];
                RestingOrder& o = ref.orders.at(r);
                const auto kind = rng() % 4;
                const std::uint32_t part =
                    o.shares > 1 ? 1 + static_cast<std::uint32_t>(rng() % (o.shares - 1)) : 1;
                bool gone = false;
                if (kind == 0) {
                    const std::uint32_t q = rng() % 2 == 0 ? o.shares : part;  // full or partial
                    msg = t::executed(o.locate, r, q);
                    o.shares -= q;
                    gone = o.shares == 0;
                } else if (kind == 1 && o.shares > 1) {
                    msg = t::cancel(o.locate, r, part);
                    o.shares -= part;
                } else if (kind == 2) {
                    msg = t::del(o.locate, r);
                    gone = true;
                } else {
                    const OrderRef nr = next_ref++;
                    const auto np = o.side == Side::buy ? o.price - 100 : o.price + 100;
                    const auto nq = 1 + static_cast<std::uint32_t>(rng() % 1000);
                    msg = t::replace(o.locate, r, nr, nq, np);
                    ref.orders[nr] = RestingOrder{np, nq, o.locate, o.side};
                    live.push_back(nr);
                    gone = true;
                }
                if (gone) {
                    ref.orders.erase(r);
                    live[k] = live.back();
                    live.pop_back();
                }
            }
            feed(a, msg);
            feed(b, msg);
            feed(c, msg);
            feed(d, msg);
            for (Locate s : symbols) {
                for (Side side : {Side::buy, Side::sell}) {
                    const auto want = ref.levels(s, side);
                    auto got = [&](const auto& book) {
                        return side == Side::buy ? book.book(s).bids.levels()
                                                 : book.book(s).asks.levels();
                    };
                    ASSERT_EQ(got(a), want) << "seed " << seed << " step " << step;
                    ASSERT_EQ(got(b), want) << "seed " << seed << " step " << step;
                    ASSERT_EQ(got(c), want) << "seed " << seed << " step " << step;
                    ASSERT_EQ(got(d), want) << "seed " << seed << " step " << step;
                }
            }
        }
        EXPECT_EQ(a.live_orders(), ref.orders.size());
        EXPECT_EQ(d.live_orders(), ref.orders.size());
        EXPECT_EQ(a.anomalies().total() + d.anomalies().total(), 0u);
    }
}

}  // namespace
