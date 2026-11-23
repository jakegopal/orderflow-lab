// Backtester tests. Each one pins one rule from engine.hpp with a hand-built
// ITCH stream (through the real parser) or a hand-built top-of-book table,
// and the expected numbers are worked out in the comments.
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include "itch_builder.hpp"
#include "orderflow/backtest/engine.hpp"
#include "orderflow/backtest/markets.hpp"
#include "orderflow/backtest/round_trip.hpp"

namespace {

using namespace orderflow;
using namespace orderflow::bt;
namespace t = orderflow::test;

constexpr std::uint16_t kLoc = 7;
constexpr std::uint32_t usd(double d) { return static_cast<std::uint32_t>(d * 10'000 + 0.5); }

// Messages for one symbol "TEST": directory, trading, then the test's own.
std::vector<std::byte> day(std::vector<std::vector<std::byte>> msgs) {
    std::vector<std::vector<std::byte>> all{t::stock_directory(kLoc, "TEST", 100, 1),
                                            t::trading_action(kLoc, "TEST", 'T', 2)};
    for (auto& m : msgs) all.push_back(std::move(m));
    return t::framed(all);
}

// Submits orders on chosen book events and records everything it hears, with
// the strategy-side time it heard it.
struct Script final : Strategy {
    std::map<std::uint64_t, std::vector<OrderRequest>> on_event;  // event index -> orders
    std::map<std::uint64_t, OrderId> cancel_on_event;             // event index -> order to cancel
    std::vector<std::pair<Time, Fill>> fills;
    std::vector<std::pair<Time, OrderDone>> dones;
    std::vector<std::pair<Time, BookSnapshot>> books;

    void on_book(const BookSnapshot& b, Gateway& gw) override {
        books.emplace_back(gw.now(), b);
        if (auto it = on_event.find(b.event_index); it != on_event.end()) {
            for (const auto& r : it->second) gw.submit(r);
        }
        if (auto it = cancel_on_event.find(b.event_index); it != cancel_on_event.end()) gw.cancel(it->second);
    }
    void on_fill(const Fill& f, Gateway& gw) override { fills.emplace_back(gw.now(), f); }
    void on_done(const OrderDone& d, Gateway& gw) override { dones.emplace_back(gw.now(), d); }
};

struct ItchRun {
    std::vector<std::byte> data;
    ItchMarket<FlatOpenBook> market;
    Engine<ItchMarket<FlatOpenBook>> engine;
    ItchRun(std::vector<std::byte> d, Fees f = {}, Latency l = {})
        : data(std::move(d)), market(data, "TEST", 64), engine(market, f, l) {}
};

const Fees kNasdaq{0, 0, 0.0030, -0.0020};  // $0.0030/share to take, $0.0020 rebate to add

// ---------------------------------------------------------------- taking

TEST(Backtest, MarketBuySweepsAsksAtExecutablePrices) {
    // Asks 100 @ $10.00 and 200 @ $10.01. A market buy of 250 takes all 100
    // at $10.00 and 150 at $10.01: average $10.006, never the mid.
    ItchRun r(day({t::add(kLoc, 1, 'S', 100, usd(10.00), "TEST", 10),
                   t::add(kLoc, 2, 'S', 200, usd(10.01), "TEST", 20),
                   t::add(kLoc, 3, 'B', 100, usd(9.99), "TEST", 30)}),
              kNasdaq);
    Script s;
    s.on_event[2] = {{Dir::buy, OrderType::market, 0, 250}};
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.fills.size(), 2u);
    EXPECT_EQ(s.fills[0].second.price, usd(10.00));
    EXPECT_EQ(s.fills[0].second.qty, 100);
    EXPECT_EQ(s.fills[1].second.price, usd(10.01));
    EXPECT_EQ(s.fills[1].second.qty, 150);
    EXPECT_EQ(s.fills[0].second.liquidity, Liquidity::taker);
    EXPECT_NEAR(s.fills[0].second.fee + s.fills[1].second.fee, 250 * 0.0030, 1e-12);
    ASSERT_EQ(s.dones.size(), 1u);
    EXPECT_EQ(s.dones[0].second.reason, DoneReason::filled);
}

TEST(Backtest, MarketOrderRemainderExpiresWhenDepthRunsOut) {
    ItchRun r(day({t::add(kLoc, 1, 'S', 100, usd(10.00), "TEST", 10)}));
    Script s;
    s.on_event[0] = {{Dir::buy, OrderType::market, 0, 300}};
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.dones.size(), 1u);
    EXPECT_EQ(s.dones[0].second.reason, DoneReason::expired);
    EXPECT_EQ(s.dones[0].second.filled, 100);
    EXPECT_EQ(s.dones[0].second.unfilled, 200);
}

TEST(Backtest, LatencyMatchesAgainstTheBookOnArrivalNotTheBookSeen) {
    // At t=10 the best ask is $10.00 (100 shares). At t=500 it is deleted.
    // A buy decided on the t=10 book arrives at 10 + 100 + 50 + 400 = 560,
    // after the delete, so it pays the next level, $10.01.
    auto msgs = [] {
        return day({t::add(kLoc, 1, 'S', 100, usd(10.00), "TEST", 10),
                    t::add(kLoc, 2, 'S', 100, usd(10.01), "TEST", 11), t::del(kLoc, 1, 500)});
    };
    for (const auto& [lat, want] :
         std::vector<std::pair<Latency, std::uint32_t>>{{{0, 0, 0}, usd(10.00)}, {{100, 50, 400}, usd(10.01)}}) {
        ItchRun r(msgs(), {}, lat);
        Script s;
        s.on_event[1] = {{Dir::buy, OrderType::market, 0, 100}};  // decided on the t=11 book
        r.engine.add(s);
        r.engine.run();
        ASSERT_EQ(s.fills.size(), 1u);
        EXPECT_EQ(s.fills[0].second.price, want);
        // The strategy hears of the fill market-data latency after it happened.
        EXPECT_EQ(s.fills[0].first, s.fills[0].second.exchange_ts + lat.market_data_ns);
    }
}

TEST(Backtest, OnATieTheMarketMessageGoesFirst) {
    // Decided at t=10 with zero latency, so the order arrives at t=10. The
    // next message is also at t=10 and deletes the $10.00 ask. Tie rule:
    // the message is applied first, so the order pays $10.01.
    ItchRun r(day({t::add(kLoc, 2, 'S', 100, usd(10.01), "TEST", 5),
                   t::add(kLoc, 1, 'S', 100, usd(10.00), "TEST", 10), t::del(kLoc, 1, 10)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::market, 0, 100}};
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.fills.size(), 1u);
    EXPECT_EQ(s.fills[0].second.price, usd(10.01));
}

TEST(Backtest, MarketableLimitTakesUpToItsLimitThenRests) {
    ItchRun r(day({t::add(kLoc, 1, 'S', 100, usd(10.00), "TEST", 10),
                   t::add(kLoc, 2, 'S', 100, usd(10.02), "TEST", 11)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::limit, usd(10.01), 150}};
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.fills.size(), 1u);  // 100 at $10.00; $10.02 is beyond the limit
    EXPECT_EQ(s.fills[0].second.qty, 100);
    EXPECT_TRUE(s.dones.empty());   // the other 50 rest at $10.01
}

// ---------------------------------------------------------------- queue position

TEST(Backtest, RestingOrderWaitsForTheQueueAheadThenFills) {
    // Bids at $9.99: ref 1 (100) and ref 2 (200) are ahead of us when we join.
    // Ref 5 (300) joins behind us. Then:
    //   E ref 1, 100  -> ahead 300 -> 200
    //   X ref 2, 200  -> ahead 0 (a cancel of an order ahead also moves us up)
    //   E ref 5, 120  -> an order behind us traded: under FIFO we trade first.
    //                    We fill 120 of our 150 at $9.99, as maker.
    ItchRun r(day({t::add(kLoc, 1, 'B', 100, usd(9.99), "TEST", 10),
                   t::add(kLoc, 2, 'B', 200, usd(9.99), "TEST", 11),
                   t::add(kLoc, 3, 'S', 100, usd(10.01), "TEST", 12),
                   t::add(kLoc, 5, 'B', 300, usd(9.99), "TEST", 100),
                   t::executed(kLoc, 1, 100, 1, 200), t::cancel(kLoc, 2, 200, 300),
                   t::executed(kLoc, 5, 120, 2, 400)}),
              kNasdaq);
    Script s;
    s.on_event[2] = {{Dir::buy, OrderType::limit, usd(9.99), 150}};  // arrives at t=12
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.fills.size(), 1u);
    const Fill& f = s.fills[0].second;
    EXPECT_EQ(f.exchange_ts, 400);
    EXPECT_EQ(f.price, usd(9.99));
    EXPECT_EQ(f.qty, 120);
    EXPECT_EQ(f.liquidity, Liquidity::maker);
    EXPECT_NEAR(f.fee, -120 * 0.0020, 1e-12);  // a rebate
    EXPECT_EQ(r.engine.queue_stats().behind_while_ahead, 0u);
}

TEST(Backtest, ExecutionsAheadOfUsDoNotFillUs) {
    ItchRun r(day({t::add(kLoc, 1, 'B', 100, usd(9.99), "TEST", 10),
                   t::add(kLoc, 3, 'S', 100, usd(10.01), "TEST", 12),
                   t::executed(kLoc, 1, 60, 1, 200)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::limit, usd(9.99), 50}};
    r.engine.add(s);
    r.engine.run();
    EXPECT_TRUE(s.fills.empty());  // 40 shares still ahead of us
}

TEST(Backtest, ATradeAtAWorsePriceOnOurSideFillsUs) {
    // We bid $10.00, the real bid is $9.99. An execution at $9.99 means the
    // seller went through $10.00, so our order would have traded first.
    ItchRun r(day({t::add(kLoc, 1, 'B', 100, usd(9.99), "TEST", 10),
                   t::add(kLoc, 3, 'S', 100, usd(10.02), "TEST", 12),
                   t::executed(kLoc, 1, 80, 1, 200)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::limit, usd(10.00), 50}};
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.fills.size(), 1u);
    EXPECT_EQ(s.fills[0].second.qty, 50);
    EXPECT_EQ(s.fills[0].second.price, usd(10.00));
}

TEST(Backtest, AnAskReachingOurBidFillsUs) {
    // We bid $10.00 alone. Then an ask is displayed at $10.00: that seller
    // would have traded with us, so we fill (a trade-through fill).
    ItchRun r(day({t::add(kLoc, 1, 'B', 100, usd(9.99), "TEST", 10),
                   t::add(kLoc, 3, 'S', 100, usd(10.02), "TEST", 12),
                   t::add(kLoc, 4, 'S', 30, usd(10.00), "TEST", 200)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::limit, usd(10.00), 50}};
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.fills.size(), 1u);
    EXPECT_EQ(s.fills[0].second.qty, 30);
    EXPECT_EQ(r.engine.queue_stats().trade_through_fills, 1u);
}

TEST(Backtest, TheSameCrossingAskFillsUsOnlyOnce) {
    // The 30-share ask at $10.00 stays displayed for two more messages. It
    // fills 30 of our 50 once; it must not fill the other 20 on the next message.
    ItchRun r(day({t::add(kLoc, 1, 'B', 100, usd(9.99), "TEST", 10),
                   t::add(kLoc, 3, 'S', 100, usd(10.02), "TEST", 12),
                   t::add(kLoc, 4, 'S', 30, usd(10.00), "TEST", 200),
                   t::add(kLoc, 5, 'B', 10, usd(9.98), "TEST", 300),
                   t::add(kLoc, 6, 'B', 10, usd(9.97), "TEST", 400)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::limit, usd(10.00), 50}};
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.fills.size(), 1u);
    EXPECT_EQ(s.fills[0].second.qty, 30);
}

TEST(Backtest, CancelThatArrivesFirstPreventsTheFill) {
    ItchRun r(day({t::add(kLoc, 1, 'B', 100, usd(9.99), "TEST", 10),
                   t::add(kLoc, 3, 'S', 100, usd(10.01), "TEST", 12),
                   t::add(kLoc, 8, 'S', 1, usd(10.01), "TEST", 50),  // event 2: we cancel here
                   t::executed(kLoc, 1, 100, 1, 200), t::add(kLoc, 9, 'B', 50, usd(9.99), "TEST", 300),
                   t::executed(kLoc, 9, 50, 2, 400)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::limit, usd(9.99), 50}};
    s.cancel_on_event[2] = 0;
    r.engine.add(s);
    r.engine.run();
    EXPECT_TRUE(s.fills.empty());
    ASSERT_EQ(s.dones.size(), 1u);
    EXPECT_EQ(s.dones[0].second.reason, DoneReason::cancelled);
}

TEST(Backtest, OrdersArrivingDuringAHaltAreRejected) {
    ItchRun r(day({t::add(kLoc, 1, 'S', 100, usd(10.00), "TEST", 10),
                   t::trading_action(kLoc, "TEST", 'H', 20), t::add(kLoc, 2, 'B', 1, usd(9.00), "TEST", 30)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::market, 0, 100}};
    r.engine.add(s);
    r.engine.run();
    EXPECT_TRUE(s.fills.empty());
    ASSERT_EQ(s.dones.size(), 1u);
    EXPECT_EQ(s.dones[0].second.reason, DoneReason::rejected_no_book);
    ASSERT_FALSE(s.books.empty());
    EXPECT_FALSE(s.books.back().second.tradable);
}

TEST(Backtest, SnapshotIsACopyOfTheMessageTheStrategySaw) {
    // With 1 ms market-data latency, the t=10 book is delivered at t=1,000,010.
    // By then the next message (t=20) is applied, but the snapshot is still t=10's.
    ItchRun r(day({t::add(kLoc, 1, 'S', 100, usd(10.00), "TEST", 10), t::del(kLoc, 1, 20)}), {},
              {1'000'000, 0, 0});
    Script s;
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.books.size(), 2u);
    EXPECT_EQ(s.books[0].first, 1'000'010);
    EXPECT_EQ(s.books[0].second.n_asks, 1);
    EXPECT_EQ(s.books[0].second.asks[0].price, usd(10.00));
    EXPECT_EQ(s.books[1].second.n_asks, 0);
}

TEST(Backtest, QueuePriorityFollowsFeedOrderNotReferenceNumbers) {
    // Ref 50 is ahead of us. Ref 10 appears in the feed *after* we join, so
    // it is behind us although its reference is lower (NASDAQ reuses older
    // references for orders re-entered at the open). Its execution fills us.
    ItchRun r(day({t::add(kLoc, 50, 'B', 100, usd(9.99), "TEST", 10),
                   t::add(kLoc, 60, 'S', 100, usd(10.01), "TEST", 12),
                   t::add(kLoc, 10, 'B', 100, usd(9.99), "TEST", 100),
                   t::executed(kLoc, 10, 40, 1, 200)}));
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::limit, usd(9.99), 50}};
    r.engine.add(s);
    r.engine.run();
    ASSERT_EQ(s.fills.size(), 1u);
    EXPECT_EQ(s.fills[0].second.qty, 40);
    EXPECT_EQ(r.market.ref_regressions(), 1u);
    EXPECT_EQ(r.market.last_regression_ts(), 100);
}

TEST(Backtest, ItchMarketCountsReferenceRegressions) {
    std::vector<std::byte> d = day({t::add(kLoc, 5, 'B', 1, usd(1), "TEST"), t::add(kLoc, 3, 'B', 1, usd(1), "TEST")});
    ItchMarket<FlatOpenBook> m(d, "TEST", 16);
    Engine<ItchMarket<FlatOpenBook>> e(m, {}, {});
    e.run();
    EXPECT_EQ(m.ref_regressions(), 1u);
}

// ---------------------------------------------------------------- top of book (L2)

struct TobTable {
    std::vector<Time> ts;
    std::vector<Px> bid, ask;
    std::vector<double> bsz, asz;
    std::vector<std::int64_t> seg;
    void row(Time t, Px b, double bs, Px a, double as, std::int64_t s) {
        ts.push_back(t), bid.push_back(b), bsz.push_back(bs), ask.push_back(a), asz.push_back(as), seg.push_back(s);
    }
    [[nodiscard]] TopOfBookMarket market() const { return TopOfBookMarket({ts, bid, bsz, ask, asz, seg}, 100); }
};

TEST(Backtest, TopOfBookRejectsRestingOrdersAndCapsTakersAtTheTop) {
    TobTable tb;
    tb.row(10, 7'700'000, 1.0, 7'700'001, 0.5, 0);
    tb.row(20, 7'700'000, 1.0, 7'700'001, 0.5, 0);
    auto m = tb.market();
    Engine<TopOfBookMarket> e(m, {5.0, 0, 0, 0}, {});
    Script s;
    s.on_event[0] = {{Dir::buy, OrderType::limit, 7'699'000, 0.1}, {Dir::buy, OrderType::market, 0, 2.0}};
    e.add(s);
    e.run();
    ASSERT_EQ(s.dones.size(), 2u);
    EXPECT_EQ(s.dones[0].second.reason, DoneReason::rejected_no_queue);
    EXPECT_EQ(s.dones[1].second.reason, DoneReason::expired);  // only 0.5 shown at the ask
    ASSERT_EQ(s.fills.size(), 1u);
    EXPECT_EQ(s.fills[0].second.qty, 0.5);
    EXPECT_EQ(s.fills[0].second.price, 7'700'001);
    // 5 bps of $77,000.01 x 0.5
    EXPECT_NEAR(s.fills[0].second.fee, 77'000.01 * 0.5 * 5e-4, 1e-9);
}

TEST(Backtest, TopOfBookRejectsOrdersArrivingInAGap) {
    // Segment 0 ends at t=20; segment 1 starts at t=1000. An order decided at
    // t=20 has no trusted book to meet, so it is rejected, not filled at a stale price.
    TobTable tb;
    tb.row(10, 100, 1, 101, 1, 0);
    tb.row(20, 100, 1, 101, 1, 0);
    tb.row(1000, 200, 1, 201, 1, 1);
    tb.row(1010, 200, 1, 201, 1, 1);
    auto m = tb.market();
    Engine<TopOfBookMarket> e(m, {}, {});
    Script s;
    s.on_event[1] = {{Dir::buy, OrderType::market, 0, 1}};
    e.add(s);
    e.run();
    ASSERT_EQ(s.dones.size(), 1u);
    EXPECT_EQ(s.dones[0].second.reason, DoneReason::rejected_no_book);
}

TEST(Backtest, BatchedFeedMatchesTheBatchCoveringTheArrival) {
    // Batches at t = 0, 50, 100. A buy decided on the t=0 batch arriving at
    // t=20 meets the t=50 batch: the real book had already moved by then, and
    // the t=0 batch is the one the signal saw. Without batch intervals it would
    // meet the t=0 batch. Zero latency (arrival at t=0) still meets t=0.
    TobTable tb;
    tb.row(0, 100, 1, 101, 1, 0);
    tb.row(50, 105, 1, 106, 1, 0);
    tb.row(100, 110, 1, 111, 1, 0);
    for (const auto& [batched, lat, want] : std::vector<std::tuple<bool, Time, Px>>{
             {true, 10, 106}, {false, 10, 101}, {true, 0, 101}}) {
        TopOfBookMarket m({tb.ts, tb.bid, tb.bsz, tb.ask, tb.asz, tb.seg}, 100, batched);
        Engine<TopOfBookMarket> e(m, {}, {lat, 0, lat});
        Script s;
        s.on_event[0] = {{Dir::buy, OrderType::market, 0, 1}};
        e.add(s);
        e.run();
        ASSERT_EQ(s.fills.size(), 1u) << batched << " " << lat;
        EXPECT_EQ(s.fills[0].second.price, want) << batched << " " << lat;
        EXPECT_EQ(s.books[0].first, lat) << "the batch is seen when published plus latency";
    }
}

// With zero latency and an event-clock hold, a round trip enters at row m's
// ask and exits at row m+h's bid: exactly Phase 1's round_trips(latency=0).
TEST(Backtest, EventClockRoundTripReproducesPhase1Pricing) {
    TobTable tb;
    // 11 rows: the last row of the data is never tradable (no row follows it
    // to prove the book stayed trusted), so the exit at row 9 needs a row 10.
    for (int i = 0; i < 11; ++i) tb.row(10 * (i + 1), 1000 + i, 5, 1001 + 2 * i, 5, 0);
    auto m = tb.market();
    Engine<TopOfBookMarket> e(m, {}, {});
    RoundTripParams p;
    p.qty = 1;
    p.hold_events = 3;
    SignalStrategy buy_at_2(p, {{2, +1}, {3, -1}, {6, -1}});  // 3 is busy and skipped
    e.add(buy_at_2);
    e.run();
    const auto& tr = buy_at_2.trades();
    ASSERT_EQ(tr.size(), 2u);
    EXPECT_EQ(tr[0].entry_px(), 1001 + 2 * 2);  // ask at row 2
    EXPECT_EQ(tr[0].exit_px(), 1000 + 5);       // bid at row 5
    EXPECT_EQ(tr[1].dir, Dir::sell);
    EXPECT_EQ(tr[1].entry_px(), 1000 + 6);      // sold at row 6's bid
    EXPECT_EQ(tr[1].exit_px(), 1001 + 2 * 9);   // bought back at row 9's ask
    EXPECT_TRUE(tr[1].complete);
    EXPECT_EQ(buy_at_2.stats().skipped_busy, 1u);
}

TEST(Backtest, MakerEntryThatNeverFillsIsCancelledAndCounted) {
    ItchRun r(day({t::add(kLoc, 1, 'B', 100, usd(9.99), "TEST", 10),
                   t::add(kLoc, 3, 'S', 100, usd(10.01), "TEST", 12),
                   t::add(kLoc, 4, 'S', 100, usd(10.02), "TEST", 5'000'000'000)}));
    RoundTripParams p;
    p.entry = Entry::maker;
    p.hold_ns = 1'000'000;
    p.maker_timeout_ns = 1'000'000'000;
    SignalStrategy s(p, {{1, +1}});
    r.engine.add(s);
    r.engine.run();
    EXPECT_TRUE(s.trades().empty());
    EXPECT_EQ(s.stats().entries_unfilled, 1u);
}

}  // namespace
