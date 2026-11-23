// Price-level (L2) book for Coinbase level2 data, the C++ twin of the Python
// BookState in src/orderflow/book/state.py.
//
// Coinbase sends "price P now has total size Q": absolute sizes, not changes,
// and Q = 0 removes the level. There are no orders and no order references,
// so this is a different structure from the ITCH book (book.hpp): one sorted
// map per side, price ticks -> size. Sizes are doubles (BTC quantities such
// as 0.00123); they are only ever stored and compared, never summed, so the
// C++ and Python books hold bit-identical values.
//
// Kept simple: its job is the parity test. The Python BookState
// is the oracle, and a C++ book that disagrees with it on a real capture is
// wrong. std::map makes best bid/ask O(1) (begin()) where the Python dict
// scans every level.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace orderflow {

class L2Book {
public:
    using Ticks = std::int64_t;
    using PriceSize = std::pair<Ticks, double>;

    // Same semantics as BookState.apply: size 0 removes the level (no error if
    // it was absent); any other size replaces it.
    void apply(bool is_bid, Ticks price, double size) {
        if (is_bid) {
            set(bids_, price, size);
        } else {
            set(asks_, price, size);
        }
    }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
    }

    [[nodiscard]] std::optional<PriceSize> best_bid() const { return first(bids_); }
    [[nodiscard]] std::optional<PriceSize> best_ask() const { return first(asks_); }
    [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }

    // Full depth, best first.
    [[nodiscard]] std::vector<PriceSize> bids() const { return {bids_.begin(), bids_.end()}; }
    [[nodiscard]] std::vector<PriceSize> asks() const { return {asks_.begin(), asks_.end()}; }

private:
    template <class Map>
    static void set(Map& side, Ticks price, double size) {
        if (size == 0.0) {
            side.erase(price);
        } else {
            side.insert_or_assign(price, size);
        }
    }
    template <class Map>
    static std::optional<PriceSize> first(const Map& side) {
        if (side.empty()) return std::nullopt;
        return PriceSize{side.begin()->first, side.begin()->second};
    }

    std::map<Ticks, double, std::greater<>> bids_;  // highest first
    std::map<Ticks, double> asks_;                  // lowest first
};

}  // namespace orderflow
