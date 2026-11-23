// itch_extract must keep exactly the chosen symbols' messages (plus
// market-wide ones), so replaying the extract gives the same book for them.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "itch_builder.hpp"
#include "orderflow/book.hpp"
#include "orderflow/itch.hpp"
#include "orderflow/itch_extract.hpp"

namespace {

using namespace orderflow;
namespace t = orderflow::test;

TEST(ItchExtract, KeepsChosenSymbolsAndMarketWideMessages) {
    const auto full = t::framed({
        t::system_event('O'),
        t::stock_directory(1, "AAPL"),
        t::stock_directory(2, "MSFT"),
        t::add(1, 10, 'B', 100, 1'500'000, "AAPL"),
        t::add(2, 11, 'B', 50, 2'000'000, "MSFT"),
        t::executed(1, 10, 30),
        t::cancel(2, 11, 10),
        t::replace(1, 10, 12, 80, 1'500'100),
        t::system_event('C'),
    });
    std::vector<std::byte> out;
    const auto st = itch::extract(full, {"AAPL"}, [&](std::span<const std::byte> m) {
        out.insert(out.end(), m.begin(), m.end());
    });
    EXPECT_EQ(st.read, 9u);
    EXPECT_EQ(st.kept, 6u);  // 2 system events + AAPL's directory, add, execute, replace
    ASSERT_EQ(st.found, std::vector<std::string>{"AAPL"});

    FlatOpenBook from_full(16), from_extract(16);
    itch::FrameReader a(full), b(out);
    while (auto m = a.next()) (void)itch::parse(*m, from_full);
    while (auto m = b.next()) (void)itch::parse(*m, from_extract);
    EXPECT_EQ(from_full.book(1).bids.levels(), from_extract.book(1).bids.levels());
    EXPECT_EQ(from_extract.book(2).bids.size(), 0u);  // MSFT left out
    EXPECT_EQ(from_extract.anomalies().total(), 0u);
}

}  // namespace
