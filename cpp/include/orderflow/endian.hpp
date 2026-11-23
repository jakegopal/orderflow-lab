// Reading big-endian integers out of a byte buffer.
//
// ITCH is a binary protocol: every integer field is sent most-significant byte
// first ("big-endian", also called network byte order). x86 and ARM CPUs store
// integers least-significant byte first ("little-endian"). So the 4 bytes
// 00 00 01 2C mean 300 on the wire, but reinterpreting them as a uint32_t on
// this machine would give 0x2C010000 = 738,263,040.
//
// Why a byte loop and not reinterpret_cast<const uint32_t*>(p):
//   - The fields are not aligned (a 4-byte price sits at offset 32 of a message
//     whose start is arbitrary), and dereferencing a misaligned pointer is
//     undefined behaviour in C++ (UBSan flags it).
//   - It also breaks the strict-aliasing rule.
// The loop below is well-defined, and GCC and Clang at -O2 compile it to one
// load plus one `bswap` instruction, so the safe version costs nothing.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace orderflow {

// Load sizeof(T) bytes starting at p as a big-endian unsigned integer.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T load_be(const std::byte* p) noexcept {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value = static_cast<T>((value << 8) | std::to_integer<T>(p[i]));
    }
    return value;
}

// ITCH timestamps are 6 bytes: nanoseconds since midnight, which needs 47 bits
// (86,400e9 ns < 2^47), so NASDAQ saves 2 bytes per message by not sending 8.
[[nodiscard]] constexpr std::uint64_t load_be48(const std::byte* p) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        value = (value << 8) | std::to_integer<std::uint64_t>(p[i]);
    }
    return value;
}

}  // namespace orderflow
