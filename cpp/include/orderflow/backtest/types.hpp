// Vocabulary of the backtester: orders, fills, fees, latency, book snapshots.
//
// Units. Time is wall-clock nanoseconds on the market data's own clock (ITCH:
// since midnight; Coinbase: since the epoch). Prices are integers in the
// market's own price units, with Market::price_scale units per dollar (ITCH
// 10,000; Coinbase cents 100), so prices compare exactly. Quantities are
// doubles because Coinbase sizes are fractional BTC; share counts up to 2^53
// are exact in a double.
#pragma once

#include <array>
#include <cstdint>

namespace orderflow::bt {

using Time = std::int64_t;
using Px = std::int64_t;
using OrderId = std::uint32_t;

enum class Dir : std::int8_t { buy = 1, sell = -1 };

[[nodiscard]] constexpr Dir opposite(Dir d) noexcept { return d == Dir::buy ? Dir::sell : Dir::buy; }
[[nodiscard]] constexpr double sign(Dir d) noexcept { return d == Dir::buy ? 1.0 : -1.0; }

// "a is a better price than b for an order resting on side d": higher for a
// buy, lower for a sell.
[[nodiscard]] constexpr bool better_for(Dir d, Px a, Px b) noexcept { return d == Dir::buy ? a > b : a < b; }

enum class OrderType : std::uint8_t {
    market,  // take whatever the opposite side offers, at any price
    limit,   // take up to the limit price; any remainder rests in the queue
};

struct OrderRequest {
    Dir dir;
    OrderType type;
    Px limit = 0;  // ignored for market orders
    double qty = 0;
};

// Taker removed resting liquidity and paid the spread; maker rested and was hit.
enum class Liquidity : std::uint8_t { taker, maker };

struct Fill {
    OrderId id;
    Time exchange_ts;  // when the exchange matched it
    Dir dir;
    Px price;
    double qty;
    Liquidity liquidity;
    double fee;            // dollars; negative is a rebate
    std::int64_t segment;  // the trusted-book segment the fill was priced in
};

enum class DoneReason : std::uint8_t {
    filled,             // completely filled
    cancelled,          // our cancel reached the exchange first
    expired,            // a market order's remainder found no more liquidity
    rejected_no_book,   // arrived while the book was untrusted or the symbol halted
    rejected_no_queue,  // a resting order on a feed without queue information (L2)
    rejected_invalid,   // zero or negative quantity
};

struct OrderDone {
    OrderId id;
    Time exchange_ts;
    DoneReason reason;
    double filled;    // total filled over the order's life
    double unfilled;  // what never filled
};

// Fees per fill: notional x bps / 10,000 + qty x per_unit. Coinbase charges in
// basis points of notional; NASDAQ charges per share. A negative maker fee
// is a rebate, which exchanges pay to attract resting liquidity.
struct Fees {
    double taker_bps = 0;
    double maker_bps = 0;
    double taker_per_unit = 0;
    double maker_per_unit = 0;

    [[nodiscard]] constexpr double fee(double notional, double qty, Liquidity l) const noexcept {
        return l == Liquidity::taker ? notional * taker_bps / 1e4 + qty * taker_per_unit
                                     : notional * maker_bps / 1e4 + qty * maker_per_unit;
    }
};

// Three delays, all charged on every decision:
//   market_data  exchange -> strategy: the feed's transit and decoding. The
//                strategy sees message m, and learns of a fill, this late.
//   decision     the strategy's own compute time (ModelStrategy measures it).
//   order_entry  strategy -> exchange: an order or cancel arrives this late.
// A decision on message m therefore reaches the exchange at
//   ts_m + market_data + decision + order_entry,
// by which time later messages may have changed the book.
struct Latency {
    Time market_data_ns = 0;
    Time decision_ns = 0;
    Time order_entry_ns = 0;
};

// What the strategy sees: a copy of the top of the book after one whole
// message, taken when that message was applied. It is a copy, not a view,
// because with market-data latency the strategy acts on it later, after the
// live book has moved on.
inline constexpr std::size_t kSnapshotDepth = 5;

struct SnapshotLevel {
    Px price = 0;
    double qty = 0;
};

struct BookSnapshot {
    Time exchange_ts = 0;          // the message's timestamp
    std::uint64_t event_index = 0; // n-th message that changed this symbol's book
    std::int64_t segment = 0;      // trusted-book segment (Coinbase); 0 all day on ITCH
    bool tradable = false;         // book trusted and symbol trading
    std::array<SnapshotLevel, kSnapshotDepth> bids{};
    std::array<SnapshotLevel, kSnapshotDepth> asks{};
    std::uint8_t n_bids = 0;
    std::uint8_t n_asks = 0;

    [[nodiscard]] bool two_sided() const noexcept { return n_bids > 0 && n_asks > 0; }
};

}  // namespace orderflow::bt
