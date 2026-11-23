// Multi-symbol L3 (order-level) book built from ITCH messages.
//
// State. Two structures, kept consistent by every message:
//   orders_  order reference -> {symbol, side, price, remaining shares}
//   books_   per symbol (indexed by stock locate), per side: price -> {total
//            shares, order count}, the aggregated view a trader looks at.
// The order map is what makes this L3: an execute, cancel or delete names
// only an order reference, so the book must remember each live order to know
// which symbol, side and price the change applies to.
//
// What each message does:
//   A/F add       insert the order; add its shares at its price
//   E   execute   remove executed shares from the order (and its level);
//                 an order executed down to zero leaves the book. ITCH sends
//                 no delete afterwards.
//   C   execute   the same as E. The execution price can differ from the
//       w/ price  order's limit (crosses), but the shares come off the level the
//                 order rests at.
//   X   cancel    the same as E, for cancelled shares
//   D   delete    remove the order and all its remaining shares
//   U   replace   delete the old order, add a new one (new reference, price
//                 and size; same symbol and side). It loses time priority.
//   P   trade     nothing: it executes a hidden order that was never displayed
//
// Each message is applied whole, so the book is only ever observed between
// messages, never halfway through one. Unlike a Coinbase message, an ITCH
// message changes at most one order (a replace changes two, but they are
// applied back to back inside one on() call).
//
// Anomalies. On a complete day replayed from the first message, every
// reference an E/C/X/D/U names must be live, and no order can lose more
// shares than it has. The book counts violations instead of crashing, so a
// whole-day run reports every one of them. Zero is the expected result,
// and the replay tool fails if it isn't.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "orderflow/itch.hpp"
#include "orderflow/order_map.hpp"
#include "orderflow/pool.hpp"
#include "orderflow/price_levels.hpp"

namespace orderflow {

using itch::Locate;
using itch::OrderRef;

struct RestingOrder {
    Price price;
    std::uint32_t shares;  // remaining
    Locate locate;
    Side side;
};

struct Anomalies {
    std::uint64_t unknown_order = 0;    // E/C/X/D/U named a reference that isn't live
    std::uint64_t duplicate_order = 0;  // add reused a live reference
    std::uint64_t zero_ref = 0;         // add with reference 0 (reserved as "empty")
    std::uint64_t over_reduction = 0;   // more shares executed or cancelled than remained
    [[nodiscard]] std::uint64_t total() const noexcept {
        return unknown_order + duplicate_order + zero_ref + over_reduction;
    }
};

template <template <Side> class LevelsT, template <class> class OrderMapT>
class OrderBook {
public:
    struct SymbolBook {
        LevelsT<Side::buy> bids;
        LevelsT<Side::sell> asks;
    };

    // Stock locate is a 16-bit code, so every possible symbol gets a slot up
    // front: no bounds check or resize on the hot path. Empty books are small
    // because FlatLevels allocates its window on the first order.
    explicit OrderBook(std::size_t expected_orders = std::size_t{1} << 22)
        : books_(std::size_t{1} << 16), orders_(expected_orders) {}

    void on(const itch::AddOrder& m) {
        add(m.order_ref, m.header.locate, m.side, m.price, m.shares);
    }
    void on(const itch::OrderExecuted& m) { reduce(m.order_ref, m.executed_shares); }
    void on(const itch::OrderExecutedWithPrice& m) { reduce(m.order_ref, m.executed_shares); }
    void on(const itch::OrderCancel& m) { reduce(m.order_ref, m.cancelled_shares); }
    void on(const itch::OrderDelete& m) { remove(m.order_ref); }
    void on(const itch::OrderReplace& m) {
        if (const auto old = remove(m.original_ref)) {
            add(m.new_ref, old->locate, old->side, m.price, m.shares);
        }
    }

    [[nodiscard]] const SymbolBook& book(Locate locate) const noexcept { return books_[locate]; }
    [[nodiscard]] const RestingOrder* order(OrderRef ref) const { return orders_.find(ref); }
    [[nodiscard]] std::size_t live_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] const Anomalies& anomalies() const noexcept { return anomalies_; }

private:
    template <class F>
    void with_side(Locate locate, Side side, F&& f) {
        SymbolBook& b = books_[locate];
        if (side == Side::buy) {
            f(b.bids);
        } else {
            f(b.asks);
        }
    }

    void add(OrderRef ref, Locate locate, Side side, Price price, std::uint32_t shares) {
        if (ref == 0) {
            ++anomalies_.zero_ref;
            return;
        }
        if (!orders_.insert(ref, RestingOrder{price, shares, locate, side})) {
            ++anomalies_.duplicate_order;
            return;
        }
        with_side(locate, side, [&](auto& levels) { levels.add(price, shares); });
    }

    void reduce(OrderRef ref, std::uint32_t shares) {
        RestingOrder* o = ref == 0 ? nullptr : orders_.find(ref);
        if (o == nullptr) {
            ++anomalies_.unknown_order;
            return;
        }
        if (shares > o->shares) {
            ++anomalies_.over_reduction;
            shares = o->shares;  // take what is there; the count records the problem
        }
        const RestingOrder copy = *o;  // o is invalid after erase
        const bool gone = shares == copy.shares;
        if (gone) {
            orders_.erase(ref);
        } else {
            o->shares -= shares;
        }
        with_side(copy.locate, copy.side,
                  [&](auto& levels) { levels.reduce(copy.price, shares, gone); });
    }

    std::optional<RestingOrder> remove(OrderRef ref) {
        const RestingOrder* o = ref == 0 ? nullptr : orders_.find(ref);
        if (o == nullptr) {
            ++anomalies_.unknown_order;
            return std::nullopt;
        }
        const RestingOrder copy = *o;
        orders_.erase(ref);
        with_side(copy.locate, copy.side,
                  [&](auto& levels) { levels.reduce(copy.price, copy.shares, true); });
        return copy;
    }

    std::vector<SymbolBook> books_;
    OrderMapT<RestingOrder> orders_;
    Anomalies anomalies_;
};

// The four variants the benchmarks compare.
template <Side S>
using FlatLevels2048 = FlatLevels<S, 2048>;
using MapStdBook = OrderBook<MapLevels, StdOrderMap>;
using MapOpenBook = OrderBook<MapLevels, OpenAddressingMap>;
using FlatStdBook = OrderBook<FlatLevels2048, StdOrderMap>;
using FlatOpenBook = OrderBook<FlatLevels2048, OpenAddressingMap>;
// std::unordered_map's layout with a pool allocator for its nodes.
using FlatPoolBook = OrderBook<FlatLevels2048, PooledStdOrderMap>;

}  // namespace orderflow
