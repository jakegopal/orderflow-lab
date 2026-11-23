// Extract chosen symbols from an ITCH day into a smaller, still valid ITCH file.
//
// Every ITCH message header carries a stock locate code. The day's 'R'
// (stock directory) messages bind each locate to a symbol before that
// symbol's first order, so filtering by locate is exact: an order's add,
// executions, cancels, delete and replaces all carry the same locate. The
// output keeps
//   - every message with locate 0 (market-wide: system events, circuit breakers)
//   - every message whose locate belongs to a chosen symbol,
// in the original order and framing. Replaying the extract gives the chosen
// symbols' books exactly as in the full day, which test_itch_extract.cpp checks.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "orderflow/endian.hpp"
#include "orderflow/itch.hpp"

namespace orderflow::itch {

struct ExtractStats {
    std::uint64_t read = 0;
    std::uint64_t kept = 0;
    std::vector<std::string> found;  // chosen symbols that appeared in the directory
};

// sink(std::span<const std::byte> framed_message) receives each kept message
// with its 2-byte length prefix, ready to write out.
template <class Sink>
ExtractStats extract(std::span<const std::byte> data, const std::vector<std::string>& symbols, Sink&& sink) {
    ExtractStats st;
    std::vector<bool> keep(std::size_t{1} << 16, false);
    keep[0] = true;
    FrameReader reader(data);
    while (true) {
        const std::size_t at = reader.offset();
        const auto msg = reader.next();
        if (!msg) break;
        ++st.read;
        if (msg->size() < 3) continue;  // no locate field: not a valid message
        const Locate loc = load_be<std::uint16_t>(msg->data() + 1);
        if (static_cast<char>((*msg)[0]) == 'R' && msg->size() == expected_length('R')) {
            Symbol s{};
            for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>((*msg)[11 + i]);
            for (const auto& want : symbols) {
                if (trimmed(s) == want) {
                    keep[loc] = true;
                    st.found.push_back(want);
                }
            }
        }
        if (keep[loc]) {
            ++st.kept;
            sink(data.subspan(at, msg->size() + 2));
        }
    }
    return st;
}

}  // namespace orderflow::itch
