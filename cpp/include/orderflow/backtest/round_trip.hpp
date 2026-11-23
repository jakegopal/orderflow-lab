// Round-trip strategies: turn a directional signal into one position at a time.
//
// RoundTripStrategy holds the execution logic, and subclasses only say what
// to do (decide()). That keeps the precomputed-signal strategy (signals from
// the Python research pipeline) and the in-engine model strategy on the
// same execution path, so their results are comparable.
//
// One trade, step by step:
//   flat      decide() says +1 or -1 on a tradable two-sided book, inside the
//             session window -> send the entry order
//               taker: a market order, which crosses the spread now
//               maker: a limit order at our own best (buy at the bid, sell at
//                      the ask), joining the back of the queue; cancelled if
//                      still unfilled after maker_timeout
//   entering  fills accumulate. On the entry's OrderDone: nothing filled ->
//             flat (counted as unfilled); otherwise hold what filled
//   holding   exit after hold_ns from learning the entry finished, or after
//             hold_events book messages from the decision (the event clock
//             reproduces Phase 1's fixed-horizon round trips exactly)
//   exiting   a market order for the whole position. If part of it can't
//             fill (thin top of book, untrusted book), the rest is retried
//             on the next tradable book
// Decisions that arrive while a trade is open are skipped and counted, like
// Phase 1's non-overlapping trades.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "orderflow/backtest/engine.hpp"
#include "orderflow/backtest/types.hpp"

namespace orderflow::bt {

enum class Entry : std::uint8_t { taker, maker };

struct RoundTripParams {
    double qty = 1;
    Entry entry = Entry::taker;
    Time hold_ns = 0;                // exactly one of hold_ns, hold_events is > 0
    std::uint64_t hold_events = 0;
    Time maker_timeout_ns = 1'000'000'000;
    Time session_start = std::numeric_limits<Time>::min();  // decisions only on books in
    Time session_end = std::numeric_limits<Time>::max();    // [session_start, session_end)
};

struct Trade {
    std::uint64_t decision_index = 0;
    Time decision_ts = 0;       // exchange time of the message decided on
    Dir dir = Dir::buy;
    Px decision_bid = 0;        // the book the decision saw, for the half-spread benchmark
    Px decision_ask = 0;
    std::int64_t decision_segment = 0;
    double qty = 0;             // filled on entry
    double entry_notional = 0;  // sum of price x qty, price units
    double exit_qty = 0;
    double exit_notional = 0;
    double fees = 0;            // dollars, both legs; negative overall means net rebates
    Time entry_ts = 0;          // first entry fill, exchange time
    Time exit_ts = 0;           // last exit fill
    bool entry_maker = false;   // entry filled passively
    std::int64_t entry_segment = 0;
    std::int64_t exit_segment = 0;
    bool complete = false;      // fully exited before the data ended

    [[nodiscard]] double entry_px() const noexcept { return qty > 0 ? entry_notional / qty : 0; }
    [[nodiscard]] double exit_px() const noexcept { return exit_qty > 0 ? exit_notional / exit_qty : 0; }
};

struct RoundTripStats {
    std::uint64_t decisions = 0;          // non-zero decide() on a book we could act on
    std::uint64_t skipped_busy = 0;       // ... that arrived while a trade was open
    std::uint64_t skipped_untradable = 0; // non-zero decide() on an untradable/one-sided book
    std::uint64_t entries_sent = 0;
    std::uint64_t entries_unfilled = 0;   // maker entries that timed out or were rejected unfilled
    std::uint64_t exit_retries = 0;
};

class RoundTripStrategy : public Strategy {
public:
    explicit RoundTripStrategy(RoundTripParams p) : p_(p) {
        if ((p_.hold_ns > 0) == (p_.hold_events > 0)) {
            throw std::invalid_argument("set exactly one of hold_ns and hold_events");
        }
        if (!(p_.qty > 0)) throw std::invalid_argument("qty must be positive");
    }

    [[nodiscard]] const std::vector<Trade>& trades() const noexcept { return trades_; }
    [[nodiscard]] const RoundTripStats& stats() const noexcept { return stats_; }

    void on_book(const BookSnapshot& book, Gateway& gw) final {
        const int d = decide(book);  // every book, so stateful signals stay current
        if (state_ == State::holding && p_.hold_events > 0 &&
            book.event_index >= cur_.decision_index + p_.hold_events) {
            send_exit(gw);
        } else if (state_ == State::exit_retry && book.tradable) {
            ++stats_.exit_retries;
            send_exit(gw);
        }
        if (d == 0) return;
        if (book.exchange_ts < p_.session_start || book.exchange_ts >= p_.session_end) return;
        if (!book.tradable || !book.two_sided()) {
            ++stats_.skipped_untradable;
            return;
        }
        ++stats_.decisions;
        if (state_ != State::flat) {
            ++stats_.skipped_busy;
            return;
        }
        enter(d > 0 ? Dir::buy : Dir::sell, book, gw);
    }

    void on_fill(const Fill& f, Gateway&) final {
        if (f.id == entry_id_) {
            if (cur_.qty == 0) {
                cur_.entry_ts = f.exchange_ts;
                cur_.entry_segment = f.segment;
            }
            cur_.qty += f.qty;
            cur_.entry_notional += static_cast<double>(f.price) * f.qty;
            cur_.entry_maker = f.liquidity == Liquidity::maker;
        } else if (f.id == exit_id_) {
            cur_.exit_qty += f.qty;
            cur_.exit_notional += static_cast<double>(f.price) * f.qty;
            cur_.exit_ts = f.exchange_ts;
            cur_.exit_segment = f.segment;
        } else {
            return;
        }
        cur_.fees += f.fee;
    }

    void on_done(const OrderDone& d, Gateway& gw) final {
        if (state_ == State::entering && d.id == entry_id_) {
            if (d.filled <= 0) {
                ++stats_.entries_unfilled;
                state_ = State::flat;
                return;
            }
            state_ = State::holding;
            if (p_.hold_ns > 0) gw.timer_at(gw.now() + p_.hold_ns, token(kExitTimer));
        } else if (state_ == State::exiting && d.id == exit_id_) {
            if (cur_.exit_qty + 1e-12 >= cur_.qty) {
                cur_.complete = true;
                trades_.push_back(cur_);
                state_ = State::flat;
            } else {
                state_ = State::exit_retry;  // try the rest on the next tradable book
            }
        }
    }

    void on_timer(std::uint64_t tok, Gateway& gw) final {
        if ((tok >> 1) != trade_no_) return;  // a timer from an earlier trade
        if ((tok & 1) == kCancelTimer && state_ == State::entering) gw.cancel(entry_id_);
        if ((tok & 1) == kExitTimer && state_ == State::holding) send_exit(gw);
    }

    void on_end(Gateway&) final {
        // A trade still open when the data ends is kept, marked incomplete, so
        // it is visible rather than silently dropped.
        if (state_ != State::flat && cur_.qty > 0) trades_.push_back(cur_);
    }

protected:
    // +1 buy, -1 sell, 0 nothing, for the book just seen. Called on every book.
    virtual int decide(const BookSnapshot& book) = 0;

private:
    enum class State : std::uint8_t { flat, entering, holding, exiting, exit_retry };
    static constexpr std::uint64_t kCancelTimer = 0, kExitTimer = 1;
    [[nodiscard]] std::uint64_t token(std::uint64_t kind) const noexcept { return (trade_no_ << 1) | kind; }

    void enter(Dir dir, const BookSnapshot& book, Gateway& gw) {
        ++trade_no_;
        cur_ = Trade{};
        cur_.decision_index = book.event_index;
        cur_.decision_ts = book.exchange_ts;
        cur_.dir = dir;
        cur_.decision_bid = book.bids[0].price;
        cur_.decision_ask = book.asks[0].price;
        cur_.decision_segment = book.segment;
        ++stats_.entries_sent;
        if (p_.entry == Entry::taker) {
            entry_id_ = gw.submit({dir, OrderType::market, 0, p_.qty});
        } else {
            const Px join = dir == Dir::buy ? book.bids[0].price : book.asks[0].price;
            entry_id_ = gw.submit({dir, OrderType::limit, join, p_.qty});
            gw.timer_at(gw.now() + p_.maker_timeout_ns, token(kCancelTimer));
        }
        state_ = State::entering;
    }

    void send_exit(Gateway& gw) {
        exit_id_ = gw.submit({opposite(cur_.dir), OrderType::market, 0, cur_.qty - cur_.exit_qty});
        state_ = State::exiting;
    }

    RoundTripParams p_;
    State state_ = State::flat;
    Trade cur_;
    OrderId entry_id_ = std::numeric_limits<OrderId>::max();
    OrderId exit_id_ = std::numeric_limits<OrderId>::max();
    std::uint64_t trade_no_ = 0;
    std::vector<Trade> trades_;
    RoundTripStats stats_;
};

// Decisions computed elsewhere (the Python research pipeline), keyed by the
// event index of the book message they were computed from.
class SignalStrategy final : public RoundTripStrategy {
public:
    SignalStrategy(RoundTripParams p, std::vector<std::pair<std::uint64_t, int>> signals)
        : RoundTripStrategy(p), signals_(std::move(signals)) {
        const auto out_of_order = std::adjacent_find(signals_.begin(), signals_.end(),
                                                     [](const auto& a, const auto& b) { return b.first <= a.first; });
        if (out_of_order != signals_.end()) {
            throw std::invalid_argument("signal event indices must be strictly increasing");
        }
    }

protected:
    int decide(const BookSnapshot& book) override {
        while (next_ < signals_.size() && signals_[next_].first < book.event_index) ++next_;
        if (next_ < signals_.size() && signals_[next_].first == book.event_index) return signals_[next_++].second;
        return 0;
    }

private:
    std::vector<std::pair<std::uint64_t, int>> signals_;
    std::size_t next_ = 0;
};

}  // namespace orderflow::bt
