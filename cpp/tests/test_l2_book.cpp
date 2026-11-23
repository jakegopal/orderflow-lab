#include "orderflow/l2_book.hpp"

#include <gtest/gtest.h>

namespace {

using orderflow::L2Book;

TEST(L2Book, SizesAreAbsoluteAndZeroRemoves) {
    L2Book b;
    b.apply(true, 7'700'000, 1.5);
    b.apply(true, 7'700'000, 0.25);  // replaces, does not add
    EXPECT_EQ(b.best_bid(), (L2Book::PriceSize{7'700'000, 0.25}));
    b.apply(true, 7'700'000, 0.0);
    EXPECT_FALSE(b.best_bid().has_value());
    b.apply(true, 1, 0.0);           // removing an absent level is not an error
    EXPECT_EQ(b.bid_levels(), 0u);
}

TEST(L2Book, BestIsHighestBidLowestAsk) {
    L2Book b;
    for (auto px : {100, 102, 101}) b.apply(true, px, 1.0);
    for (auto px : {105, 103, 104}) b.apply(false, px, 2.0);
    EXPECT_EQ(b.best_bid()->first, 102);
    EXPECT_EQ(b.best_ask()->first, 103);
    EXPECT_EQ(b.bids().front().first, 102);
    EXPECT_EQ(b.asks().back().first, 105);
    b.clear();
    EXPECT_EQ(b.bid_levels() + b.ask_levels(), 0u);
}

}  // namespace
