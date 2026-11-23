// One side of one symbol's book, aggregated by price: price -> (total shares, order count).
//
// Two implementations with the same interface, so the book can be built on
// either and benchmarked:
//
//   MapLevels   std::map keyed by price, ordered best-first. Every operation is
//               O(log n) and walks tree nodes that live wherever the allocator
//               put them, so each step down the tree is a likely cache miss.
//
//   FlatLevels  a fixed window of N price slots in one contiguous array, indexed
//               by (price - base) / tick. Add and reduce are O(1) array writes.
//               A bitmap with one bit per slot finds the next non-empty level
//               with a few count-leading-zeros instructions. Prices outside the
//               window (stub quotes far from the market, sub-penny prices) go to
//               an overflow std::map, so nothing is ever dropped. When the best
//               price itself leaves the window, the window re-centers on it.
//
// Interface both provide (S is the side; best means highest bid / lowest ask):
//   add(price, shares)                 a new order rests at price
//   reduce(price, shares, order_gone)  shares leave price; order_gone if the order left too
//   best() -> optional<PriceLevel>     O(1)
//   at(price) -> Level                 {0, 0} if empty
//   size()                             number of non-empty price levels
//   levels() -> vector<PriceLevel>     best to worst; for tests and debugging, not hot
//   walk(f)                            calls f(PriceLevel) best to worst until f returns
//                                      false: how a taker order sweeps the book
//
// Correctness contract: reduce() is only called for shares that add() put
// there. The book guarantees that by looking the order up first. Debug and
// sanitizer builds assert it.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "orderflow/itch.hpp"

namespace orderflow {

using itch::Price;
using itch::Side;

struct Level {
    std::uint64_t shares = 0;  // 64-bit: a level's total can exceed 2^32 on penny stocks
    std::uint32_t orders = 0;
    friend bool operator==(const Level&, const Level&) = default;
};

struct PriceLevel {
    Price price;
    Level level;
    friend bool operator==(const PriceLevel&, const PriceLevel&) = default;
};

// "a is a better price than b" for side S: higher for bids, lower for asks.
template <Side S>
[[nodiscard]] constexpr bool better(Price a, Price b) noexcept {
    if constexpr (S == Side::buy) {
        return a > b;
    } else {
        return a < b;
    }
}

// std::map comparator that sorts best-first, so begin() is the best level.
template <Side S>
using BestFirst = std::conditional_t<S == Side::buy, std::greater<Price>, std::less<Price>>;

template <Side S>
class MapLevels {
public:
    void add(Price price, std::uint32_t shares) {
        Level& l = levels_[price];
        l.shares += shares;
        ++l.orders;
    }

    void reduce(Price price, std::uint32_t shares, bool order_gone) {
        auto it = levels_.find(price);
        assert(it != levels_.end() && it->second.shares >= shares);
        it->second.shares -= shares;
        if (order_gone) --it->second.orders;
        if (it->second.orders == 0) {
            assert(it->second.shares == 0);
            levels_.erase(it);
        }
    }

    [[nodiscard]] std::optional<PriceLevel> best() const {
        if (levels_.empty()) return std::nullopt;
        return PriceLevel{levels_.begin()->first, levels_.begin()->second};
    }

    [[nodiscard]] Level at(Price price) const {
        auto it = levels_.find(price);
        return it == levels_.end() ? Level{} : it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

    [[nodiscard]] std::vector<PriceLevel> levels() const {
        std::vector<PriceLevel> out;
        out.reserve(levels_.size());
        for (const auto& [p, l] : levels_) out.push_back({p, l});
        return out;
    }

    template <class F>
    void walk(F&& f) const {
        for (const auto& [p, l] : levels_) {
            if (!f(PriceLevel{p, l})) return;
        }
    }

private:
    std::map<Price, Level, BestFirst<S>> levels_;
};

// N: slots in the window (a multiple of 64, one bitmap word per 64 slots).
// Tick: price units per slot. 100 = one cent, the minimum increment for US
// stocks quoted at $1.00 or more (Reg NMS Rule 612). Sub-penny prices, which
// only sub-$1 stocks may use, are not cent-aligned and live in the overflow map.
template <Side S, std::size_t N = 2048, Price Tick = 100>
class FlatLevels {
    static_assert(N % 64 == 0 && N > 0);
    static constexpr std::size_t kWords = N / 64;
    static constexpr std::size_t kNone = N;  // "no best slot"

public:
    void add(Price price, std::uint32_t shares) {
        if (slots_.empty()) anchor(price);
        if (const auto i = slot_of(price)) {
            Level& l = slots_[*i];
            if (l.orders == 0) mark(*i);
            l.shares += shares;
            ++l.orders;
            if (best_slot_ == kNone || better<S>(price, price_of(best_slot_))) best_slot_ = *i;
        } else {
            Level& l = overflow_[price];
            l.shares += shares;
            ++l.orders;
            maybe_recenter();
        }
    }

    void reduce(Price price, std::uint32_t shares, bool order_gone) {
        if (const auto i = slot_of(price)) {
            Level& l = slots_[*i];
            assert(l.shares >= shares && l.orders > 0);
            l.shares -= shares;
            if (order_gone) --l.orders;
            if (l.orders == 0) {
                assert(l.shares == 0);
                unmark(*i);
                if (*i == best_slot_) {
                    best_slot_ = scan_best();
                    maybe_recenter();
                }
            }
        } else {
            auto it = overflow_.find(price);
            assert(it != overflow_.end() && it->second.shares >= shares);
            it->second.shares -= shares;
            if (order_gone) --it->second.orders;
            if (it->second.orders == 0) {
                assert(it->second.shares == 0);
                overflow_.erase(it);
            }
        }
    }

    [[nodiscard]] std::optional<PriceLevel> best() const {
        // maybe_recenter() keeps this true: if the overflow holds a better price
        // than the window, it is one the window cannot hold (sub-penny).
        std::optional<PriceLevel> out;
        if (best_slot_ != kNone) out = PriceLevel{price_of(best_slot_), slots_[best_slot_]};
        if (!overflow_.empty()) {
            const auto& [p, l] = *overflow_.begin();
            if (!out || better<S>(p, out->price)) out = PriceLevel{p, l};
        }
        return out;
    }

    [[nodiscard]] Level at(Price price) const {
        if (const auto i = slot_of(price)) return slots_[*i];
        auto it = overflow_.find(price);
        return it == overflow_.end() ? Level{} : it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return window_levels_ + overflow_.size(); }

    [[nodiscard]] std::vector<PriceLevel> levels() const {
        std::vector<PriceLevel> out;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].orders != 0) out.push_back({price_of(i), slots_[i]});
        }
        for (const auto& [p, l] : overflow_) out.push_back({p, l});
        std::sort(out.begin(), out.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return better<S>(a.price, b.price); });
        return out;
    }

    // Best to worst, merging the window (scanned bit by bit away from the
    // best slot) with the overflow map (already sorted best-first). Usually
    // the first level or two satisfy the caller, so this touches one or two
    // bitmap words, not the whole window.
    template <class F>
    void walk(F&& f) const {
        auto over = overflow_.begin();
        std::size_t slot = best_slot_;
        while (slot != kNone || over != overflow_.end()) {
            const bool take_window =
                slot != kNone && (over == overflow_.end() || better<S>(price_of(slot), over->first));
            if (take_window) {
                if (!f(PriceLevel{price_of(slot), slots_[slot]})) return;
                slot = next_worse(slot);
            } else {
                if (!f(PriceLevel{over->first, over->second})) return;
                ++over;
            }
        }
    }

    // Diagnostics for the benchmark write-up.
    [[nodiscard]] std::uint64_t recenters() const noexcept { return recenters_; }
    [[nodiscard]] std::size_t overflow_levels() const noexcept { return overflow_.size(); }

private:
    // Slot index for price, or nullopt if it is outside the window or not on a tick.
    [[nodiscard]] std::optional<std::size_t> slot_of(Price price) const noexcept {
        if (slots_.empty() || price < base_ || price % Tick != 0) return std::nullopt;
        const std::size_t i = (price - base_) / Tick;
        if (i >= N) return std::nullopt;
        return i;
    }
    [[nodiscard]] Price price_of(std::size_t i) const noexcept {
        return base_ + static_cast<Price>(i) * Tick;
    }

    void mark(std::size_t i) noexcept {
        occupied_[i / 64] |= std::uint64_t{1} << (i % 64);
        ++window_levels_;
    }
    void unmark(std::size_t i) noexcept {
        occupied_[i / 64] &= ~(std::uint64_t{1} << (i % 64));
        --window_levels_;
    }

    // Best occupied slot: the highest set bit for bids, the lowest for asks.
    // At most kWords (32 for N = 2048) word tests, usually one or two.
    [[nodiscard]] std::size_t scan_best() const noexcept {
        if constexpr (S == Side::buy) {
            for (std::size_t w = kWords; w-- > 0;) {
                if (occupied_[w] != 0) {
                    return w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(occupied_[w]));
                }
            }
        } else {
            for (std::size_t w = 0; w < kWords; ++w) {
                if (occupied_[w] != 0) {
                    return w * 64 + static_cast<std::size_t>(std::countr_zero(occupied_[w]));
                }
            }
        }
        return kNone;
    }

    // The next occupied slot after i, moving away from the best price (down
    // for bids, up for asks), or kNone.
    [[nodiscard]] std::size_t next_worse(std::size_t i) const noexcept {
        if constexpr (S == Side::buy) {
            std::size_t w = i / 64;
            // Bits strictly below i in its word (none when i is the word's bit 0).
            std::uint64_t bits = occupied_[w] & ((std::uint64_t{1} << (i % 64)) - 1);
            while (true) {
                if (bits != 0) return w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(bits));
                if (w == 0) return kNone;
                bits = occupied_[--w];
            }
        } else {
            std::size_t j = i + 1;
            if (j >= N) return kNone;
            std::size_t w = j / 64;
            std::uint64_t bits = occupied_[w] & (~std::uint64_t{0} << (j % 64));
            while (true) {
                if (bits != 0) return w * 64 + static_cast<std::size_t>(std::countr_zero(bits));
                if (++w == kWords) return kNone;
                bits = occupied_[w];
            }
        }
    }

    // Allocate the window centred on the first price this side sees.
    void anchor(Price center) {
        slots_.assign(N, Level{});
        base_ = window_base(center);
    }

    [[nodiscard]] static Price window_base(Price center) noexcept {
        const Price aligned = center - center % Tick;
        constexpr Price half = static_cast<Price>(N / 2) * Tick;
        return aligned > half ? aligned - half : 0;
    }

    // If the best price is in the overflow map and could live in the window,
    // the market has moved: move the window so it is centred there.
    void maybe_recenter() {
        if (overflow_.empty()) return;
        const Price candidate = overflow_.begin()->first;
        if (candidate % Tick != 0) return;  // sub-penny: no window position helps
        if (best_slot_ != kNone && !better<S>(candidate, price_of(best_slot_))) return;
        recenter(candidate);
    }

    // O(N + overflow): rare by design, and counted so the benchmark can report it.
    void recenter(Price center) {
        ++recenters_;
        std::vector<PriceLevel> moved;
        for (std::size_t i = 0; i < N; ++i) {
            if (slots_[i].orders != 0) moved.push_back({price_of(i), slots_[i]});
        }
        std::fill(slots_.begin(), slots_.end(), Level{});
        occupied_.fill(0);
        window_levels_ = 0;
        base_ = window_base(center);

        for (auto it = overflow_.begin(); it != overflow_.end();) {
            if (slot_of(it->first)) {
                moved.push_back({it->first, it->second});
                it = overflow_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [p, l] : moved) {
            if (const auto i = slot_of(p)) {
                slots_[*i] = l;
                mark(*i);
            } else {
                overflow_.emplace(p, l);
            }
        }
        best_slot_ = scan_best();
    }

    std::vector<Level> slots_;              // N entries once anchored, else empty
    std::array<std::uint64_t, kWords> occupied_{};
    Price base_ = 0;                        // price of slot 0
    std::size_t best_slot_ = kNone;
    std::size_t window_levels_ = 0;
    std::map<Price, Level, BestFirst<S>> overflow_;
    std::uint64_t recenters_ = 0;
};

}  // namespace orderflow
