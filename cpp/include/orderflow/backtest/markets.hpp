// Markets: where the engine's book and messages come from.
//
// A Market is any type with this interface (the engine is a template over it,
// so there is no virtual call per message):
//
//   kHasQueue              true if resting orders can be queue-tracked (L3)
//   next_ts(t)             the next message's timestamp; false at the end
//   step(hooks) -> bool    apply the next whole message; true if it changed
//                          the target symbol's book. Before applying it,
//                          report every reduction of a resting order through
//                          hooks.on_reduce (L3 only)
//   snapshot(s)            copy the top kSnapshotDepth levels per side
//   walk(side, f)          f(price, qty) per level of `side`, best to worst,
//                          until f returns false
//   displayed(side, px)    shares displayed at px on side
//   next_arrival()         the feed-order number the next added order will get (L3 only)
//   tradable()             the book is trusted and the symbol is trading
//   segment()              trusted-book segment id of the current state
//   price_scale()          price units per dollar
//
// ItchMarket is L3: it replays a NASDAQ ITCH day and backtests one symbol,
// with queue positions. TopOfBookMarket is the Coinbase path: it replays the
// top of book that the Python pipeline produced, one row per message, for
// taker orders only, because L2 data cannot place an order in a queue.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "orderflow/backtest/types.hpp"
#include "orderflow/book.hpp"
#include "orderflow/itch.hpp"

namespace orderflow::bt {

[[nodiscard]] constexpr Dir to_dir(itch::Side s) noexcept { return s == itch::Side::buy ? Dir::buy : Dir::sell; }

template <class Book = FlatOpenBook>
class ItchMarket {
public:
    static constexpr bool kHasQueue = true;

    // data: a whole ITCH file (or an itch_extract of it); symbol: e.g. "AAPL".
    // The book is kept for every symbol in the data, so that the anomaly
    // check stays meaningful; the strategy sees only `symbol`.
    // expected_orders sizes the order map (a whole day peaks at a few million).
    ItchMarket(std::span<const std::byte> data, std::string_view symbol,
               std::size_t expected_orders = std::size_t{1} << 22)
        : reader_(data), symbol_(symbol), book_(expected_orders) {
        if (symbol.empty() || symbol.size() > 8) throw std::invalid_argument("symbol must be 1-8 characters");
    }

    bool next_ts(Time& t) {
        if (!pending_) pending_ = reader_.next();
        if (!pending_) return false;
        // Every ITCH message has the 11-byte header; a shorter one is corrupt,
        // and parse() will report it. Give it the last time so order holds.
        t = pending_->size() >= 11 ? static_cast<Time>(load_be48(pending_->data() + 5)) : last_ts_;
        return true;
    }

    template <class Hooks>
    bool step(Hooks& hooks) {
        if (!pending_ && !(pending_ = reader_.next())) return false;
        const auto msg = *pending_;
        pending_.reset();
        Handler<Hooks> h{*this, hooks};
        if (itch::parse(msg, h) != itch::ParseStatus::ok) ++malformed_;
        if (msg.size() >= 11) last_ts_ = static_cast<Time>(load_be48(msg.data() + 5));
        if (h.changed) ++event_index_;
        return h.changed;
    }

    void snapshot(BookSnapshot& s) const {
        s.exchange_ts = last_ts_;
        s.event_index = event_index_ - 1;  // the message just applied
        s.segment = 0;
        s.tradable = tradable();
        s.n_bids = s.n_asks = 0;
        if (!locate_) return;
        const auto& b = book_.book(*locate_);
        b.bids.walk([&](const PriceLevel& l) {
            s.bids[s.n_bids++] = {static_cast<Px>(l.price), static_cast<double>(l.level.shares)};
            return s.n_bids < kSnapshotDepth;
        });
        b.asks.walk([&](const PriceLevel& l) {
            s.asks[s.n_asks++] = {static_cast<Px>(l.price), static_cast<double>(l.level.shares)};
            return s.n_asks < kSnapshotDepth;
        });
    }

    template <class F>
    void walk(Dir side, F&& f) const {
        if (!locate_) return;
        const auto& b = book_.book(*locate_);
        auto g = [&](const PriceLevel& l) { return f(static_cast<Px>(l.price), static_cast<double>(l.level.shares)); };
        if (side == Dir::buy) {
            b.bids.walk(g);
        } else {
            b.asks.walk(g);
        }
    }

    [[nodiscard]] double displayed(Dir side, Px price) const {
        if (!locate_ || price < 0 || price > static_cast<Px>(UINT32_MAX)) return 0;
        const auto& b = book_.book(*locate_);
        const auto p = static_cast<itch::Price>(price);
        return static_cast<double>(side == Dir::buy ? b.bids.at(p).shares : b.asks.at(p).shares);
    }

    // The arrival number the next order will get: every live order with a
    // smaller one was in the queue before an order arriving now.
    [[nodiscard]] std::uint64_t next_arrival() const noexcept { return next_arrival_; }
    // Trusted from the start of the day, but tradable only while the symbol's
    // state is 'T' (trading): during a halt the engine does not match.
    [[nodiscard]] bool tradable() const noexcept { return locate_.has_value() && trading_; }
    [[nodiscard]] std::int64_t segment() const noexcept { return 0; }
    [[nodiscard]] static constexpr double price_scale() noexcept { return itch::kPriceScale; }

    [[nodiscard]] const Book& book() const noexcept { return book_; }
    [[nodiscard]] std::optional<itch::Locate> locate() const noexcept { return locate_; }
    [[nodiscard]] std::uint64_t malformed() const noexcept { return malformed_; }
    [[nodiscard]] std::uint64_t ref_regressions() const noexcept { return ref_regressions_; }
    [[nodiscard]] Time last_regression_ts() const noexcept { return last_regression_ts_; }

private:
    template <class Hooks>
    struct Handler {
        ItchMarket& m;
        Hooks& hooks;
        bool changed = false;

        [[nodiscard]] bool target(const itch::Header& h) const noexcept { return m.target(h); }

        void on(const itch::StockDirectory& d) {
            if (itch::trimmed(d.stock) == m.symbol_) m.locate_ = d.header.locate;
        }
        void on(const itch::StockTradingAction& a) {
            if (target(a.header)) m.trading_ = a.trading_state == 'T';
        }
        void on(const itch::AddOrder& a) {
            m.note_ref(a.header, a.order_ref);
            if (target(a.header)) {
                m.arrival_.insert(a.order_ref, m.next_arrival_++);
                changed = true;
            }
            m.book_.on(a);
        }
        void on(const itch::OrderExecuted& e) { reduce(e.header, e.order_ref, e.executed_shares, true, e); }
        void on(const itch::OrderExecutedWithPrice& e) { reduce(e.header, e.order_ref, e.executed_shares, true, e); }
        void on(const itch::OrderCancel& x) { reduce(x.header, x.order_ref, x.cancelled_shares, false, x); }
        void on(const itch::OrderDelete& d) {
            if (target(d.header)) {
                if (const auto* o = m.book_.order(d.order_ref)) {
                    hooks.on_reduce(m.arrival_of(d.order_ref), to_dir(o->side), o->price, o->shares, false);
                }
                m.arrival_.erase(d.order_ref);
                changed = true;
            }
            m.book_.on(d);
        }
        void on(const itch::OrderReplace& u) {
            m.note_ref(u.header, u.new_ref);
            if (target(u.header)) {
                if (const auto* o = m.book_.order(u.original_ref)) {
                    hooks.on_reduce(m.arrival_of(u.original_ref), to_dir(o->side), o->price, o->shares, false);
                }
                // The replacement goes to the back of the queue: a new arrival.
                m.arrival_.erase(u.original_ref);
                m.arrival_.insert(u.new_ref, m.next_arrival_++);
                changed = true;
            }
            m.book_.on(u);
        }

        template <class Msg>
        void reduce(const itch::Header& h, std::uint64_t ref, std::uint32_t shares, bool exec, const Msg& msg) {
            if (!target(h)) {
                m.book_.on(msg);
                return;
            }
            // Look the order up *before* the book applies the message:
            // afterwards a fully executed order is gone.
            if (const auto* o = m.book_.order(ref)) {
                hooks.on_reduce(m.arrival_of(ref), to_dir(o->side), o->price, static_cast<double>(shares), exec);
            }
            m.book_.on(msg);
            if (m.book_.order(ref) == nullptr) m.arrival_.erase(ref);  // fully executed or cancelled
            changed = true;
        }
    };

    // Queue priority comes from the order in which orders appear in the feed
    // (arrival_), not from NASDAQ's reference numbers. On 01302019 references
    // are *not* monotonic: orders re-entered at the open reuse older
    // references, until 16:00 on some symbols (docs/phase3). So the market
    // only counts reference regressions per symbol, and remembers the time of
    // the last one: a run is exact if that time is before our first order.
    void note_ref(const itch::Header& h, std::uint64_t ref) noexcept {
        if (!target(h)) return;
        if (ref <= max_ref_) {
            ++ref_regressions_;
            last_regression_ts_ = static_cast<Time>(h.timestamp_ns);
        } else {
            max_ref_ = ref;
        }
    }
    [[nodiscard]] bool target(const itch::Header& h) const noexcept { return locate_ && h.locate == *locate_; }
    // Feed-order arrival number of a live target order (0 if unknown, which
    // the book would also have counted as an anomaly).
    [[nodiscard]] std::uint64_t arrival_of(std::uint64_t ref) const {
        const auto* a = arrival_.find(ref);
        return a == nullptr ? 0 : *a;
    }

    itch::FrameReader reader_;
    std::optional<std::span<const std::byte>> pending_;
    std::string symbol_;
    Book book_;
    std::optional<itch::Locate> locate_;
    bool trading_ = false;  // until the symbol's first 'H' says 'T'
    OpenAddressingMap<std::uint64_t> arrival_{1 << 16};  // live target orders only
    std::uint64_t next_arrival_ = 1;
    std::uint64_t max_ref_ = 0;
    std::uint64_t ref_regressions_ = 0;
    Time last_regression_ts_ = -1;
    std::uint64_t event_index_ = 0;
    std::uint64_t malformed_ = 0;
    Time last_ts_ = 0;
};

// One row per message, as the Python pipeline's feature frame has it: the top
// of book after each whole message in a trusted segment. Row i has
// event_index i. Only the best level is known, so a taker order larger than
// it fills partly and its remainder expires (it can't see deeper levels, so
// it doesn't invent them).
class TopOfBookMarket {
public:
    static constexpr bool kHasQueue = false;

    struct Rows {
        std::span<const Time> ts;
        std::span<const Px> bid_px;
        std::span<const double> bid_sz;
        std::span<const Px> ask_px;
        std::span<const double> ask_sz;
        std::span<const std::int64_t> segment;
    };

    // Coinbase publishes the book in batches (~50 ms apart). The batch stamped
    // T(m) shows the book as of T(m), but the exchange's real book moved
    // continuously between T(m-1) and T(m). With batch_intervals, row m stands
    // for the whole interval (T(m-1), T(m)] of its segment: an order arriving between two
    // batches is matched against the later one. Without it, it would be matched
    // against the earlier one, often the very batch its signal was computed
    // from, which is lookahead by up to one batch. An order arriving exactly at
    // T(m) still meets row m, so zero latency reproduces Phase 1's round trips.
    TopOfBookMarket(Rows rows, double price_scale, bool batch_intervals = true)
        : r_(rows), scale_(price_scale), batch_intervals_(batch_intervals) {
        const std::size_t n = r_.ts.size();
        if (r_.bid_px.size() != n || r_.bid_sz.size() != n || r_.ask_px.size() != n || r_.ask_sz.size() != n ||
            r_.segment.size() != n) {
            throw std::invalid_argument("all row arrays must have the same length");
        }
        for (std::size_t i = 1; i < n; ++i) {
            if (r_.ts[i] < r_.ts[i - 1]) throw std::invalid_argument("timestamps must not decrease");
        }
    }

    // When the next row's state starts to apply at the exchange: 1 ns after the
    // previous row in the same segment, or at its own time if it opens a segment
    // (it can't speak for the gap before it).
    bool next_ts(Time& t) const {
        if (next_ >= r_.ts.size()) return false;
        const bool continues = next_ > 0 && r_.segment[next_ - 1] == r_.segment[next_];
        t = batch_intervals_ && continues ? r_.ts[next_ - 1] + 1 : r_.ts[next_];
        return true;
    }

    template <class Hooks>
    bool step(Hooks& /*hooks*/) {
        if (next_ >= r_.ts.size()) return false;
        cur_ = next_++;
        return true;
    }

    void snapshot(BookSnapshot& s) const {
        s.exchange_ts = r_.ts[cur_];
        s.event_index = cur_;
        s.segment = r_.segment[cur_];
        s.tradable = tradable();
        s.bids[0] = {r_.bid_px[cur_], r_.bid_sz[cur_]};
        s.asks[0] = {r_.ask_px[cur_], r_.ask_sz[cur_]};
        s.n_bids = s.n_asks = 1;
    }

    template <class F>
    void walk(Dir side, F&& f) const {
        if (!started()) return;
        if (side == Dir::buy) {
            (void)f(r_.bid_px[cur_], r_.bid_sz[cur_]);
        } else {
            (void)f(r_.ask_px[cur_], r_.ask_sz[cur_]);
        }
    }

    // The current state is trusted only until its segment ends. Between a
    // segment's last row and the next segment's first, the real book is
    // unknown (a gap or a reconnect), so orders arriving then are rejected
    // rather than filled at stale prices. This looks one row ahead, but only
    // at the segment id, which the loader fixed from sequence numbers alone:
    // it reveals no future prices.
    [[nodiscard]] bool tradable() const noexcept {
        return started() && cur_ + 1 < r_.ts.size() && r_.segment[cur_ + 1] == r_.segment[cur_];
    }
    [[nodiscard]] std::int64_t segment() const noexcept { return started() ? r_.segment[cur_] : -1; }
    [[nodiscard]] double price_scale() const noexcept { return scale_; }
    [[nodiscard]] double displayed(Dir, Px) const noexcept { return 0; }
    [[nodiscard]] std::uint64_t next_arrival() const noexcept { return 0; }

private:
    [[nodiscard]] bool started() const noexcept { return next_ > 0; }

    Rows r_;
    double scale_;
    bool batch_intervals_;
    std::size_t next_ = 0;
    std::size_t cur_ = 0;
};

}  // namespace orderflow::bt
