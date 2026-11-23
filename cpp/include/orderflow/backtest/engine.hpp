// Event-driven backtester: market messages and our own orders, fills and
// timers processed in one timestamp order.
//
// Market messages come from the Market (see markets.hpp). Everything else
// waits in a priority queue keyed by (time, insertion order). The loop takes
// whichever is earliest; on a tie the market message goes first, so an order
// never acts inside the same nanosecond as a book change it couldn't have seen.
//
// - Latency: an order decided on a book seen at t reaches the exchange at
//   t + decision + order_entry and is matched against the book as it is then.
//   The strategy receives a BookSnapshot copied when the message was applied,
//   because by the time it acts the live book may have moved on.
// - Taker orders walk the opposite side level by level at displayed prices.
// - Resting orders (L3 markets only) are tracked in FIFO queue position; see
//   on_reduce() and docs/phase3/01-engine.md.
// - Our orders never change the replayed book (no market impact). Several
//   strategies can therefore share one pass over the data.
#pragma once

#include <algorithm>
#include <cstdint>
#include <queue>
#include <utility>
#include <variant>
#include <vector>

#include "orderflow/backtest/types.hpp"

namespace orderflow::bt {

// What a strategy may do. Every action takes effect at the exchange after
// decision + order-entry latency.
class Gateway {
public:
    virtual OrderId submit(const OrderRequest& request) = 0;
    virtual void cancel(OrderId id) = 0;
    virtual void timer_at(Time t, std::uint64_t token) = 0;  // strategy-side wake-up
    [[nodiscard]] virtual Time now() const = 0;              // the strategy's current time

protected:
    ~Gateway() = default;
};

// Virtual dispatch here costs a few ns per call, made once per event the
// strategy receives, not once per market message. In return, strategies
// can be written and tested independently of the engine's template type.
class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_book(const BookSnapshot& book, Gateway& gw) = 0;
    virtual void on_fill(const Fill& /*fill*/, Gateway& /*gw*/) {}
    virtual void on_done(const OrderDone& /*done*/, Gateway& /*gw*/) {}
    virtual void on_timer(std::uint64_t /*token*/, Gateway& /*gw*/) {}
    virtual void on_end(Gateway& /*gw*/) {}
};

// Diagnostics of the queue model, reported with every run.
struct QueueStats {
    std::uint64_t passive_fills = 0;
    std::uint64_t trade_through_fills = 0;  // the opposite side reached our price
    std::uint64_t behind_while_ahead = 0;   // an order behind us traded while ours still had
                                            // orders ahead: FIFO says that cannot happen
};

template <class Market>
class Engine {
public:
    Engine(Market& market, Fees fees, Latency latency) : market_(market), fees_(fees), latency_(latency) {}
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Strategies must outlive run().
    void add(Strategy& s) { accounts_.push_back(Account{this, &s, {}, {}, {}, 0}); }

    void run() {
        for (std::size_t a = 0; a < accounts_.size(); ++a) accounts_[a].index = a;
        Time next = 0;
        while (true) {
            const bool more = market_.next_ts(next);
            if (!events_.empty() && (!more || events_.top().t < next)) {
                dispatch();
                continue;
            }
            if (!more) break;
            // Set the clock first: passive fills happen inside step(), via the
            // hooks, and are stamped with this message's time.
            market_ts_ = next;
            Hooks hooks{*this};
            const bool changed = market_.step(hooks);
            if (changed) {
                if (market_.tradable()) check_trade_through();
                BookSnapshot snap;
                market_.snapshot(snap);
                // The strategy sees a message when it is published (its own
                // timestamp), which for a batched feed is later than the moment
                // its state starts to apply at the exchange (next).
                push(snap.exchange_ts + latency_.market_data_ns, kAllAccounts, Observe{snap});
            }
        }
        for (auto& acct : accounts_) {
            acct.clock = market_ts_;
            acct.strategy->on_end(acct);
        }
    }

    [[nodiscard]] const std::vector<Fill>& fills(std::size_t account) const { return accounts_[account].fills; }
    [[nodiscard]] const QueueStats& queue_stats() const noexcept { return queue_stats_; }

private:
    static constexpr std::size_t kAllAccounts = static_cast<std::size_t>(-1);

    struct OrderState {
        OrderRequest req;
        double filled = 0;
        bool done = false;
        bool resting = false;
        double ahead = 0;              // displayed shares ahead of us at our price
        std::uint64_t boundary = 0;    // feed-order number of the next order after us
        double crossing_seen = 0;      // opposite-side shares at or through our price, last check
    };

    struct Account final : Gateway {
        Engine* engine;
        Strategy* strategy;
        std::vector<OrderState> orders;
        std::vector<OrderId> resting;
        std::vector<Fill> fills;
        std::size_t index;
        Time clock = 0;  // the strategy's current time: the time of the event it is handling

        Account(Engine* e, Strategy* s, std::vector<OrderState> o, std::vector<OrderId> r, std::vector<Fill> f,
                std::size_t i)
            : engine(e), strategy(s), orders(std::move(o)), resting(std::move(r)), fills(std::move(f)), index(i) {}

        OrderId submit(const OrderRequest& request) override {
            const auto id = static_cast<OrderId>(orders.size());
            orders.push_back(OrderState{request});
            engine->push(clock + engine->latency_.decision_ns + engine->latency_.order_entry_ns, index, Arrive{id});
            return id;
        }
        void cancel(OrderId id) override {
            engine->push(clock + engine->latency_.decision_ns + engine->latency_.order_entry_ns, index,
                         CancelArrive{id});
        }
        void timer_at(Time t, std::uint64_t token) override { engine->push(std::max(t, clock), index, Timer{token}); }
        [[nodiscard]] Time now() const override { return clock; }
    };

    struct Observe { BookSnapshot snap; };
    struct Arrive { OrderId id; };
    struct CancelArrive { OrderId id; };
    struct NotifyFill { Fill fill; };
    struct NotifyDone { OrderDone done; };
    struct Timer { std::uint64_t token; };
    using Payload = std::variant<Observe, Arrive, CancelArrive, NotifyFill, NotifyDone, Timer>;

    struct Event {
        Time t;
        std::uint64_t seq;  // insertion order breaks ties, so equal-time events stay FIFO
        std::size_t account;
        Payload payload;
    };
    struct Later {
        bool operator()(const Event& a, const Event& b) const noexcept {
            return a.t != b.t ? a.t > b.t : a.seq > b.seq;
        }
    };

    // The Market calls these before it applies a message for the target symbol.
    struct Hooks {
        Engine& e;
        // Shares leave a resting order: the order with feed-order number
        // `arrival` (on side, at price) loses qty,
        // by execution (exec) or by cancel/delete.
        void on_reduce(std::uint64_t arrival, Dir side, Px price, double qty, bool exec) {
            e.on_reduce(arrival, side, price, qty, exec);
        }
    };

    template <class P>
    void push(Time t, std::size_t account, P&& payload) {
        events_.push(Event{t, seq_++, account, Payload{std::forward<P>(payload)}});
    }

    void dispatch() {
        Event ev = events_.top();
        events_.pop();
        std::visit([&](auto& p) { handle(ev, p); }, ev.payload);
    }

    // ---- strategy side -------------------------------------------------
    void handle(const Event& ev, const Observe& o) {
        for (auto& acct : accounts_) {
            acct.clock = ev.t;
            acct.strategy->on_book(o.snap, acct);
        }
    }
    void handle(const Event& ev, const NotifyFill& n) {
        Account& acct = accounts_[ev.account];
        acct.clock = ev.t;
        acct.strategy->on_fill(n.fill, acct);
    }
    void handle(const Event& ev, const NotifyDone& n) {
        Account& acct = accounts_[ev.account];
        acct.clock = ev.t;
        acct.strategy->on_done(n.done, acct);
    }
    void handle(const Event& ev, const Timer& t) {
        Account& acct = accounts_[ev.account];
        acct.clock = ev.t;
        acct.strategy->on_timer(t.token, acct);
    }

    // ---- exchange side -------------------------------------------------
    void handle(const Event& ev, const Arrive& a) {
        Account& acct = accounts_[ev.account];
        OrderState& o = acct.orders[a.id];
        if (!(o.req.qty > 0)) return finish(acct, a.id, ev.t, DoneReason::rejected_invalid);
        if (!market_.tradable()) return finish(acct, a.id, ev.t, DoneReason::rejected_no_book);

        // Take liquidity: sweep the opposite side best to worst, within the limit.
        const Dir dir = o.req.dir;
        const bool is_market = o.req.type == OrderType::market;
        market_.walk(opposite(dir), [&](Px price, double available) {
            const double remaining = o.req.qty - o.filled;
            if (remaining <= 0) return false;
            if (!is_market && better_for(opposite(dir), o.req.limit, price)) return false;  // beyond limit
            fill(acct, a.id, ev.t, price, std::min(remaining, available), Liquidity::taker);
            return true;
        });
        if (o.req.qty - o.filled <= 0) return finish(acct, a.id, ev.t, DoneReason::filled);
        if (is_market) return finish(acct, a.id, ev.t, DoneReason::expired);
        if constexpr (!Market::kHasQueue) {
            return finish(acct, a.id, ev.t, DoneReason::rejected_no_queue);
        } else {
            // Rest at the back of the queue: everything displayed at our price
            // now is ahead of us. Every live order has a feed-order number
            // below boundary; anything added later gets one at or above it.
            o.resting = true;
            o.ahead = market_.displayed(dir, o.req.limit);
            o.boundary = market_.next_arrival();
            acct.resting.push_back(a.id);
        }
    }

    void handle(const Event& ev, const CancelArrive& c) {
        Account& acct = accounts_[ev.account];
        OrderState& o = acct.orders[c.id];
        // Too late: the order already filled or was rejected, and the strategy
        // hears about that from the earlier OrderDone. (A cancel can't overtake
        // its order: both travel with the same latency, and equal times are FIFO.)
        if (o.done) return;
        if (o.resting) unrest(acct, c.id);
        finish(acct, c.id, ev.t, DoneReason::cancelled);
    }

    // FIFO queue tracking. arrival is the feed-order number of the real order
    // losing shares (stamped by the Market when the order appeared; a replace
    // gets a new one, as it loses priority). A real order at our price that
    // arrived before boundary was there when we arrived, and is ahead of us;
    // any later one is behind us.
    void on_reduce(std::uint64_t arrival, Dir side, Px price, double qty, bool exec) {
        for (auto& acct : accounts_) {
            for (std::size_t i = 0; i < acct.resting.size();) {
                const OrderId id = acct.resting[i];
                OrderState& o = acct.orders[id];
                bool removed = false;
                if (o.req.dir == side) {
                    if (price == o.req.limit) {
                        if (arrival < o.boundary) {
                            o.ahead = std::max(0.0, o.ahead - qty);  // an order ahead traded or left
                        } else if (exec) {
                            // An order behind us traded, so under FIFO we would have been
                            // hit first.
                            if (o.ahead > 0) ++queue_stats_.behind_while_ahead;
                            removed = passive_fill(acct, id, std::min(qty, o.req.qty - o.filled), false);
                        }
                    } else if (exec && better_for(side, o.req.limit, price)) {
                        // A trade at a worse price on our side: the aggressor went through
                        // our price, so it would have taken us first.
                        removed = passive_fill(acct, id, std::min(qty, o.req.qty - o.filled), false);
                    }
                }
                if (!removed) ++i;
            }
        }
    }

    // After a message: if the opposite side now reaches our resting price, a
    // real order would have traded with us. (Example: we bid 10.00 with no one
    // left ahead, and an ask at 10.00 now rests on the book.) Only crossing
    // shares that are new since the last check count: our fill doesn't remove
    // the real order (no market impact), so without this the same displayed
    // ask would fill us again after every later message.
    void check_trade_through() {
        for (auto& acct : accounts_) {
            for (std::size_t i = 0; i < acct.resting.size();) {
                const OrderId id = acct.resting[i];
                OrderState& o = acct.orders[id];
                double crossing = 0;
                market_.walk(opposite(o.req.dir), [&](Px price, double qty) {
                    if (better_for(opposite(o.req.dir), o.req.limit, price)) return false;  // not reaching us
                    crossing += qty;
                    return true;
                });
                const double fresh = std::max(0.0, crossing - o.crossing_seen);
                o.crossing_seen = crossing;
                const double take = std::min(fresh, o.req.qty - o.filled);
                if (take > 0 && passive_fill(acct, id, take, true)) continue;
                ++i;
            }
        }
    }

    // Returns true if the order left the resting list.
    bool passive_fill(Account& acct, OrderId id, double qty, bool through) {
        OrderState& o = acct.orders[id];
        if (qty <= 0) return false;
        ++queue_stats_.passive_fills;
        if (through) ++queue_stats_.trade_through_fills;
        fill(acct, id, market_ts_, o.req.limit, qty, Liquidity::maker);
        if (o.req.qty - o.filled > 0) return false;
        unrest(acct, id);
        finish(acct, id, market_ts_, DoneReason::filled);
        return true;
    }

    void fill(Account& acct, OrderId id, Time t, Px price, double qty, Liquidity liq) {
        OrderState& o = acct.orders[id];
        o.filled += qty;
        const double notional = static_cast<double>(price) / market_.price_scale() * qty;
        const Fill f{id, t, o.req.dir, price, qty, liq, fees_.fee(notional, qty, liq), market_.segment()};
        acct.fills.push_back(f);
        push(t + latency_.market_data_ns, acct.index, NotifyFill{f});
    }

    void finish(Account& acct, OrderId id, Time t, DoneReason reason) {
        OrderState& o = acct.orders[id];
        o.done = true;
        push(t + latency_.market_data_ns, acct.index,
             NotifyDone{OrderDone{id, t, reason, o.filled, o.req.qty - o.filled}});
    }

    void unrest(Account& acct, OrderId id) {
        acct.orders[id].resting = false;
        std::erase(acct.resting, id);
    }

    Market& market_;
    Fees fees_;
    Latency latency_;
    std::vector<Account> accounts_;
    std::priority_queue<Event, std::vector<Event>, Later> events_;
    std::uint64_t seq_ = 0;
    Time market_ts_ = 0;
    QueueStats queue_stats_;
};

}  // namespace orderflow::bt
