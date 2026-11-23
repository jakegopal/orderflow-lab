// NASDAQ TotalView-ITCH 5.0: framing and decoding of the book-relevant messages.
//
// Files are length-prefixed messages: [2-byte big-endian length][message]...
// Every message starts with an 11-byte header: type (1), stock locate (2),
// tracking number (2), timestamp in ns since midnight (6). Fields are
// big-endian; prices are uint32 with 4 implied decimals.
//
// parse() checks the length against the spec for the message type before
// reading any field, decodes into a plain struct, and calls handler.on(msg).
// The handler is a template parameter, so dispatch is resolved at compile
// time; types the handler has no on() for go to on_other() if it has one and
// are otherwise skipped.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "orderflow/endian.hpp"

namespace orderflow::itch {

using Price = std::uint32_t;  // units of $0.0001
using OrderRef = std::uint64_t;
using Locate = std::uint16_t;

inline constexpr Price kPriceScale = 10'000;  // price units per dollar

// Fields every message carries.
struct Header {
    char type;
    Locate locate;
    std::uint16_t tracking;
    std::uint64_t timestamp_ns;  // since midnight
};

// 8-byte ASCII symbol, padded on the right with spaces ("AAPL    ").
using Symbol = std::array<char, 8>;

[[nodiscard]] constexpr std::string_view trimmed(const Symbol& s) noexcept {
    std::size_t n = s.size();
    while (n > 0 && s[n - 1] == ' ') --n;
    return {s.data(), n};
}

enum class Side : char { buy = 'B', sell = 'S' };

// 'S': market-wide session events. 'O' start of messages, 'S' start of system
// hours, 'Q' start of market hours (9:30), 'M' end of market hours (16:00),
// 'E' end of system hours, 'C' end of messages.
struct SystemEvent {
    Header header;
    char event_code;
};

// 'R': one per symbol before the open; binds a locate code to a symbol.
struct StockDirectory {
    Header header;
    Symbol stock;
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
};

// 'H': a symbol's trading state changes. While a symbol is halted, paused or in
// a quotation-only period the matching engine does not execute, so orders that
// would cross can rest on the book until trading resumes with an auction.
struct StockTradingAction {
    Header header;
    Symbol stock;
    char trading_state;  // 'H' halted, 'P' paused (LULD), 'Q' quotation only, 'T' trading
    std::array<char, 4> reason;  // e.g. "T1  " news pending, "LUDP" volatility pause
};

// 'A' (no attribution) and 'F' (with a 4-letter market-participant ID).
// For the book they mean the same thing: a new resting limit order.
struct AddOrder {
    Header header;
    OrderRef order_ref;
    Side side;
    std::uint32_t shares;
    Symbol stock;
    Price price;
    std::optional<std::array<char, 4>> attribution;  // set only for 'F'
};

// 'E': a resting order was hit for executed_shares, at its own limit price.
struct OrderExecuted {
    Header header;
    OrderRef order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};

// 'C': executed at a price other than the order's limit price (for example in
// an opening or closing cross). Same effect on the book as 'E'.
struct OrderExecutedWithPrice {
    Header header;
    OrderRef order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
    bool printable;  // false: don't count toward volume (already printed by a cross)
    Price execution_price;
};

// 'X': cancelled_shares removed from an order that stays on the book.
struct OrderCancel {
    Header header;
    OrderRef order_ref;
    std::uint32_t cancelled_shares;
};

// 'D': the whole remaining order is removed.
struct OrderDelete {
    Header header;
    OrderRef order_ref;
};

// 'U': cancel original_ref and add new_ref with a new price and size. Side and
// symbol carry over. The new order goes to the back of its price's queue, which
// is why a replace is not an in-place size change.
struct OrderReplace {
    Header header;
    OrderRef original_ref;
    OrderRef new_ref;
    std::uint32_t shares;
    Price price;
};

// 'P': an execution against a non-displayed (hidden) order. Hidden orders were
// never added to the displayed book, so this does not change it.
struct Trade {
    Header header;
    OrderRef order_ref;  // always 0: hidden orders are anonymous
    Side side;
    std::uint32_t shares;
    Symbol stock;
    Price price;
    std::uint64_t match_number;
};

// Spec length of each message type, including the type byte. Returns 0 for a
// letter that is not an ITCH 5.0 message type.
[[nodiscard]] constexpr std::size_t expected_length(char type) noexcept {
    switch (type) {
        case 'S': return 12;  // system event
        case 'R': return 39;  // stock directory
        case 'H': return 25;  // stock trading action (halt / resume)
        case 'Y': return 20;  // Reg SHO short-sale restriction
        case 'L': return 26;  // market participant position
        case 'V': return 35;  // market-wide circuit breaker decline levels
        case 'W': return 12;  // market-wide circuit breaker status
        case 'K': return 28;  // IPO quoting period update
        case 'J': return 35;  // LULD auction collar
        case 'h': return 21;  // operational halt
        case 'A': return 36;  // add order
        case 'F': return 40;  // add order with MPID attribution
        case 'E': return 31;  // order executed
        case 'C': return 36;  // order executed with price
        case 'X': return 23;  // order cancel
        case 'D': return 19;  // order delete
        case 'U': return 35;  // order replace
        case 'P': return 44;  // trade (non-cross)
        case 'Q': return 40;  // cross trade
        case 'B': return 19;  // broken trade
        case 'I': return 50;  // net order imbalance indicator
        case 'N': return 20;  // retail price improvement indicator
        case 'O': return 48;  // direct listing with capital raise
        default: return 0;
    }
}

enum class ParseStatus {
    ok,            // decoded and dispatched (to on() or on_other())
    empty,         // zero-length message
    unknown_type,  // not an ITCH 5.0 letter: the framing is probably corrupt
    bad_length,    // known type, wrong length for it
};

namespace detail {

// Bounds were checked once against expected_length() before these run, so
// each read is in range by construction.
struct Cursor {
    const std::byte* p;
    template <std::unsigned_integral T>
    [[nodiscard]] T u(std::size_t offset) const noexcept { return load_be<T>(p + offset); }
    [[nodiscard]] char ch(std::size_t offset) const noexcept { return static_cast<char>(p[offset]); }
    template <std::size_t N>
    [[nodiscard]] std::array<char, N> chars(std::size_t offset) const noexcept {
        std::array<char, N> out{};
        for (std::size_t i = 0; i < N; ++i) out[i] = ch(offset + i);
        return out;
    }
    [[nodiscard]] Header header() const noexcept {
        return {ch(0), u<std::uint16_t>(1), u<std::uint16_t>(3), load_be48(p + 5)};
    }
};

template <class Handler, class Message>
void deliver(Handler& handler, const Message& m, std::span<const std::byte> raw) {
    if constexpr (requires { handler.on(m); }) {
        handler.on(m);
    } else if constexpr (requires { handler.on_other(m.header, raw); }) {
        handler.on_other(m.header, raw);
    }
}

}  // namespace detail

// Decode one message (without its length prefix) and hand it to the handler.
template <class Handler>
ParseStatus parse(std::span<const std::byte> msg, Handler& handler) {
    if (msg.empty()) return ParseStatus::empty;
    const char type = static_cast<char>(msg[0]);
    const std::size_t want = expected_length(type);
    if (want == 0) return ParseStatus::unknown_type;
    if (msg.size() != want) return ParseStatus::bad_length;

    const detail::Cursor c{msg.data()};
    const Header h = c.header();
    switch (type) {
        case 'S':
            detail::deliver(handler, SystemEvent{h, c.ch(11)}, msg);
            break;
        case 'R':
            detail::deliver(handler,
                            StockDirectory{h, c.chars<8>(11), c.ch(19), c.ch(20),
                                           c.u<std::uint32_t>(21)},
                            msg);
            break;
        case 'H':
            detail::deliver(handler,
                            StockTradingAction{h, c.chars<8>(11), c.ch(19), c.chars<4>(21)}, msg);
            break;
        case 'A':
        case 'F': {
            AddOrder m{h, c.u<std::uint64_t>(11), static_cast<Side>(c.ch(19)),
                       c.u<std::uint32_t>(20), c.chars<8>(24), c.u<std::uint32_t>(32),
                       std::nullopt};
            if (type == 'F') m.attribution = c.chars<4>(36);
            detail::deliver(handler, m, msg);
            break;
        }
        case 'E':
            detail::deliver(handler,
                            OrderExecuted{h, c.u<std::uint64_t>(11), c.u<std::uint32_t>(19),
                                          c.u<std::uint64_t>(23)},
                            msg);
            break;
        case 'C':
            detail::deliver(handler,
                            OrderExecutedWithPrice{h, c.u<std::uint64_t>(11),
                                                   c.u<std::uint32_t>(19),
                                                   c.u<std::uint64_t>(23), c.ch(31) == 'Y',
                                                   c.u<std::uint32_t>(32)},
                            msg);
            break;
        case 'X':
            detail::deliver(handler,
                            OrderCancel{h, c.u<std::uint64_t>(11), c.u<std::uint32_t>(19)}, msg);
            break;
        case 'D':
            detail::deliver(handler, OrderDelete{h, c.u<std::uint64_t>(11)}, msg);
            break;
        case 'U':
            detail::deliver(handler,
                            OrderReplace{h, c.u<std::uint64_t>(11), c.u<std::uint64_t>(19),
                                         c.u<std::uint32_t>(27), c.u<std::uint32_t>(31)},
                            msg);
            break;
        case 'P':
            detail::deliver(handler,
                            Trade{h, c.u<std::uint64_t>(11), static_cast<Side>(c.ch(19)),
                                  c.u<std::uint32_t>(20), c.chars<8>(24),
                                  c.u<std::uint32_t>(32), c.u<std::uint64_t>(36)},
                            msg);
            break;
        default:
            // A valid ITCH type this project doesn't decode (halts, imbalances, ...).
            if constexpr (requires { handler.on_other(h, msg); }) handler.on_other(h, msg);
            break;
    }
    return ParseStatus::ok;
}

// Splits a buffer of length-prefixed messages into individual messages.
// next() returns std::nullopt at the clean end of the buffer. truncated() then
// says whether the buffer ended partway through a message (a cut-off file).
class FrameReader {
public:
    explicit FrameReader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] std::optional<std::span<const std::byte>> next() noexcept {
        const std::size_t remaining = data_.size() - pos_;
        if (remaining == 0) return std::nullopt;
        if (remaining < 2) return truncate();
        const std::size_t len = load_be<std::uint16_t>(data_.data() + pos_);
        if (remaining - 2 < len) return truncate();
        auto msg = data_.subspan(pos_ + 2, len);
        pos_ += 2 + len;
        return msg;
    }

    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

private:
    std::optional<std::span<const std::byte>> truncate() noexcept {
        truncated_ = true;
        pos_ = data_.size();
        return std::nullopt;
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    bool truncated_ = false;
};

}  // namespace orderflow::itch
