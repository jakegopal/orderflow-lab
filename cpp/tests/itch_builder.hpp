// Test-only ITCH encoder. It writes each field at the byte offset the
// NASDAQ TotalView-ITCH 5.0 spec gives, independently of the parser, so a
// parser that reads a field from the wrong offset fails these tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace orderflow::test {

class Bytes {
public:
    Bytes& u8(std::uint8_t v) { return be(v, 1); }
    Bytes& u16(std::uint16_t v) { return be(v, 2); }
    Bytes& u32(std::uint32_t v) { return be(v, 4); }
    Bytes& u48(std::uint64_t v) { return be(v, 6); }
    Bytes& u64(std::uint64_t v) { return be(v, 8); }
    Bytes& ch(char c) { return u8(static_cast<std::uint8_t>(c)); }
    // Left-justified, space-padded, as ITCH sends alphanumeric fields.
    Bytes& text(std::string_view s, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) ch(i < s.size() ? s[i] : ' ');
        return *this;
    }
    [[nodiscard]] const std::vector<std::byte>& data() const { return data_; }

private:
    Bytes& be(std::uint64_t v, int width) {
        for (int i = width - 1; i >= 0; --i) {
            data_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
        }
        return *this;
    }
    std::vector<std::byte> data_;
};

// Common 11-byte header: type, locate, tracking, 6-byte timestamp.
inline Bytes header(char type, std::uint16_t locate, std::uint64_t ts = 0) {
    Bytes b;
    b.ch(type).u16(locate).u16(0).u48(ts);
    return b;
}

inline std::vector<std::byte> add(std::uint16_t locate, std::uint64_t ref, char side,
                                  std::uint32_t shares, std::uint32_t price,
                                  std::string_view stock = "TEST", std::uint64_t ts = 0) {
    return header('A', locate, ts).u64(ref).ch(side).u32(shares).text(stock, 8).u32(price).data();
}
inline std::vector<std::byte> add_mpid(std::uint16_t locate, std::uint64_t ref, char side,
                                       std::uint32_t shares, std::uint32_t price,
                                       std::string_view mpid) {
    return header('F', locate).u64(ref).ch(side).u32(shares).text("TEST", 8).u32(price)
        .text(mpid, 4).data();
}
inline std::vector<std::byte> executed(std::uint16_t locate, std::uint64_t ref,
                                       std::uint32_t shares, std::uint64_t match = 1,
                                       std::uint64_t ts = 0) {
    return header('E', locate, ts).u64(ref).u32(shares).u64(match).data();
}
inline std::vector<std::byte> executed_with_price(std::uint16_t locate, std::uint64_t ref,
                                                  std::uint32_t shares, std::uint32_t price,
                                                  bool printable, std::uint64_t match = 1,
                                                  std::uint64_t ts = 0) {
    return header('C', locate, ts).u64(ref).u32(shares).u64(match).ch(printable ? 'Y' : 'N')
        .u32(price).data();
}
inline std::vector<std::byte> cancel(std::uint16_t locate, std::uint64_t ref,
                                     std::uint32_t shares, std::uint64_t ts = 0) {
    return header('X', locate, ts).u64(ref).u32(shares).data();
}
inline std::vector<std::byte> del(std::uint16_t locate, std::uint64_t ref, std::uint64_t ts = 0) {
    return header('D', locate, ts).u64(ref).data();
}
inline std::vector<std::byte> replace(std::uint16_t locate, std::uint64_t old_ref,
                                      std::uint64_t new_ref, std::uint32_t shares,
                                      std::uint32_t price, std::uint64_t ts = 0) {
    return header('U', locate, ts).u64(old_ref).u64(new_ref).u32(shares).u32(price).data();
}
inline std::vector<std::byte> trade(std::uint16_t locate, char side, std::uint32_t shares,
                                    std::uint32_t price, std::uint64_t match) {
    return header('P', locate).u64(0).ch(side).u32(shares).text("TEST", 8).u32(price)
        .u64(match).data();
}
inline std::vector<std::byte> system_event(char code, std::uint64_t ts = 0) {
    return header('S', 0, ts).ch(code).data();
}
inline std::vector<std::byte> stock_directory(std::uint16_t locate, std::string_view stock,
                                              std::uint32_t round_lot = 100,
                                              std::uint64_t ts = 0) {
    // Fields after round-lot size are not decoded; fill them to the spec length.
    return header('R', locate, ts).text(stock, 8).ch('Q').ch('N').u32(round_lot).ch('N').ch('C')
        .text("Z", 2).ch('P').ch('N').ch('N').ch('1').ch('N').u32(0).ch('N').data();
}

// 'H': trading state 'T' trading, 'H' halted, 'P' paused, 'Q' quotation only.
inline std::vector<std::byte> trading_action(std::uint16_t locate, std::string_view stock,
                                             char state, std::uint64_t ts = 0) {
    return header('H', locate, ts).text(stock, 8).ch(state).ch(' ').text("", 4).data();
}

// Prefix each message with its 2-byte big-endian length, as the files do.
inline std::vector<std::byte> framed(const std::vector<std::vector<std::byte>>& messages) {
    std::vector<std::byte> out;
    for (const auto& m : messages) {
        out.push_back(static_cast<std::byte>(m.size() >> 8));
        out.push_back(static_cast<std::byte>(m.size() & 0xFF));
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

}  // namespace orderflow::test
