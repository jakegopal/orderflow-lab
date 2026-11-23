#include "orderflow/endian.hpp"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

using orderflow::load_be;
using orderflow::load_be48;

template <std::size_t N>
constexpr std::array<std::byte, N> bytes(const unsigned char (&raw)[N]) {
    std::array<std::byte, N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = std::byte{raw[i]};
    return out;
}

TEST(Endian, ReadsMostSignificantByteFirst) {
    constexpr auto b = bytes({0x00, 0x00, 0x01, 0x2C});
    EXPECT_EQ(load_be<std::uint32_t>(b.data()), 300u);  // not 0x2C010000
}

TEST(Endian, AllWidths) {
    constexpr auto b = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    EXPECT_EQ(load_be<std::uint8_t>(b.data()), 0x01u);
    EXPECT_EQ(load_be<std::uint16_t>(b.data()), 0x0102u);
    EXPECT_EQ(load_be<std::uint32_t>(b.data()), 0x01020304u);
    EXPECT_EQ(load_be<std::uint64_t>(b.data()), 0x0102030405060708u);
    EXPECT_EQ(load_be48(b.data()), 0x010203040506u);
}

TEST(Endian, HighBitDoesNotSignExtend) {
    constexpr auto b = bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(load_be<std::uint32_t>(b.data()), 0xFFFFFFFFu);
    EXPECT_EQ(load_be48(b.data()), 0xFFFFFFFFFFFFu);
}

TEST(Endian, LargestItchTimestampFitsIn48Bits) {
    // 23:59:59.999999999 after midnight, in ns, encoded big-endian in 6 bytes.
    constexpr std::uint64_t last_ns = 86'400'000'000'000 - 1;
    std::array<std::byte, 6> b{};
    for (std::size_t i = 0; i < 6; ++i) b[i] = std::byte(last_ns >> (8 * (5 - i)));
    EXPECT_EQ(load_be48(b.data()), last_ns);
}

// Compile-time check: load_be is constexpr, so it can be verified by the compiler.
static_assert(load_be<std::uint16_t>(bytes({0xAB, 0xCD}).data()) == 0xABCD);

}  // namespace
