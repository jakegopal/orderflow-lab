#include "orderflow/itch.hpp"

#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "itch_builder.hpp"

namespace {

using namespace orderflow::itch;
namespace t = orderflow::test;

// Records the last message of each kind it was given.
struct Recorder {
    std::vector<std::variant<SystemEvent, StockDirectory, StockTradingAction, AddOrder, OrderExecuted,
                             OrderExecutedWithPrice, OrderCancel, OrderDelete, OrderReplace,
                             Trade>>
        got;
    std::vector<char> other;
    void on(const SystemEvent& m) { got.emplace_back(m); }
    void on(const StockDirectory& m) { got.emplace_back(m); }
    void on(const StockTradingAction& m) { got.emplace_back(m); }
    void on(const AddOrder& m) { got.emplace_back(m); }
    void on(const OrderExecuted& m) { got.emplace_back(m); }
    void on(const OrderExecutedWithPrice& m) { got.emplace_back(m); }
    void on(const OrderCancel& m) { got.emplace_back(m); }
    void on(const OrderDelete& m) { got.emplace_back(m); }
    void on(const OrderReplace& m) { got.emplace_back(m); }
    void on(const Trade& m) { got.emplace_back(m); }
    void on_other(const Header& h, std::span<const std::byte>) { other.push_back(h.type); }
};

template <class T>
T parse_one(const std::vector<std::byte>& msg) {
    Recorder r;
    EXPECT_EQ(parse(msg, r), ParseStatus::ok);
    EXPECT_EQ(r.got.size(), 1u);
    return std::get<T>(r.got.at(0));
}

TEST(Itch, BuilderMatchesSpecLengths) {
    // Guards the test encoder itself: if it drifts from the spec, every
    // field-level test below would be testing the wrong thing.
    EXPECT_EQ(t::add(1, 1, 'B', 1, 1).size(), 36u);
    EXPECT_EQ(t::add_mpid(1, 1, 'B', 1, 1, "ABCD").size(), 40u);
    EXPECT_EQ(t::executed(1, 1, 1).size(), 31u);
    EXPECT_EQ(t::executed_with_price(1, 1, 1, 1, true).size(), 36u);
    EXPECT_EQ(t::cancel(1, 1, 1).size(), 23u);
    EXPECT_EQ(t::del(1, 1).size(), 19u);
    EXPECT_EQ(t::replace(1, 1, 2, 1, 1).size(), 35u);
    EXPECT_EQ(t::trade(1, 'B', 1, 1, 1).size(), 44u);
    EXPECT_EQ(t::system_event('O').size(), 12u);
    EXPECT_EQ(t::stock_directory(1, "AAPL").size(), 39u);
}

TEST(Itch, HeaderFields) {
    const auto m = parse_one<AddOrder>(t::add(13, 7, 'B', 100, 1'234'500, "AAPL", 34'200'000'000'123));
    EXPECT_EQ(m.header.type, 'A');
    EXPECT_EQ(m.header.locate, 13);
    EXPECT_EQ(m.header.timestamp_ns, 34'200'000'000'123u);  // 9:30:00.000000123
}

TEST(Itch, AddOrder) {
    const auto m = parse_one<AddOrder>(t::add(13, 0x0102030405060708, 'S', 250, 1'234'500, "AAPL"));
    EXPECT_EQ(m.order_ref, 0x0102030405060708u);
    EXPECT_EQ(m.side, Side::sell);
    EXPECT_EQ(m.shares, 250u);
    EXPECT_EQ(trimmed(m.stock), "AAPL");
    EXPECT_EQ(m.price, 1'234'500u);  // $123.45
    EXPECT_FALSE(m.attribution.has_value());
}

TEST(Itch, AddOrderWithAttribution) {
    const auto m = parse_one<AddOrder>(t::add_mpid(2, 9, 'B', 10, 500, "GSCO"));
    EXPECT_EQ(m.header.type, 'F');
    EXPECT_EQ(m.side, Side::buy);
    ASSERT_TRUE(m.attribution.has_value());
    EXPECT_EQ(std::string(m.attribution->begin(), m.attribution->end()), "GSCO");
}

TEST(Itch, Executions) {
    const auto e = parse_one<OrderExecuted>(t::executed(3, 42, 75, 999));
    EXPECT_EQ(e.order_ref, 42u);
    EXPECT_EQ(e.executed_shares, 75u);
    EXPECT_EQ(e.match_number, 999u);

    const auto c = parse_one<OrderExecutedWithPrice>(t::executed_with_price(3, 42, 5, 1'000'100, false, 7));
    EXPECT_EQ(c.executed_shares, 5u);
    EXPECT_EQ(c.match_number, 7u);
    EXPECT_FALSE(c.printable);
    EXPECT_EQ(c.execution_price, 1'000'100u);
}

TEST(Itch, CancelDeleteReplace) {
    const auto x = parse_one<OrderCancel>(t::cancel(4, 11, 30));
    EXPECT_EQ(x.order_ref, 11u);
    EXPECT_EQ(x.cancelled_shares, 30u);

    EXPECT_EQ(parse_one<OrderDelete>(t::del(4, 12)).order_ref, 12u);

    const auto u = parse_one<OrderReplace>(t::replace(4, 13, 14, 60, 2'000'000));
    EXPECT_EQ(u.original_ref, 13u);
    EXPECT_EQ(u.new_ref, 14u);
    EXPECT_EQ(u.shares, 60u);
    EXPECT_EQ(u.price, 2'000'000u);
}

TEST(Itch, TradeSystemEventDirectory) {
    const auto p = parse_one<Trade>(t::trade(5, 'S', 300, 500'000, 77));
    EXPECT_EQ(p.order_ref, 0u);
    EXPECT_EQ(p.side, Side::sell);
    EXPECT_EQ(p.shares, 300u);
    EXPECT_EQ(p.match_number, 77u);

    EXPECT_EQ(parse_one<SystemEvent>(t::system_event('Q')).event_code, 'Q');

    const auto r = parse_one<StockDirectory>(t::stock_directory(8, "MSFT", 100));
    EXPECT_EQ(r.header.locate, 8);
    EXPECT_EQ(trimmed(r.stock), "MSFT");
    EXPECT_EQ(r.round_lot_size, 100u);
}

TEST(Itch, StockTradingAction) {
    // 25 bytes: header + stock(8) + state + reserved + reason(4).
    const auto h = t::header('H', 9).text("SXTC", 8).ch('P').ch(' ').text("LUDP", 4).data();
    ASSERT_EQ(h.size(), 25u);
    const auto m = parse_one<StockTradingAction>(h);
    EXPECT_EQ(m.header.locate, 9);
    EXPECT_EQ(trimmed(m.stock), "SXTC");
    EXPECT_EQ(m.trading_state, 'P');
    EXPECT_EQ(std::string(m.reason.begin(), m.reason.end()), "LUDP");
}

TEST(Itch, KnownButUndecodedTypeGoesToOnOther) {
    // 'Y' Reg SHO restriction, 20 bytes: header + stock(8) + action.
    const auto y = t::header('Y', 1).text("TEST", 8).ch('0').data();
    Recorder r;
    EXPECT_EQ(parse(y, r), ParseStatus::ok);
    EXPECT_TRUE(r.got.empty());
    EXPECT_EQ(r.other, std::vector<char>{'Y'});
}

TEST(Itch, HandlerWithoutOnOtherIgnoresEverythingElse) {
    struct OnlyDeletes {
        int deletes = 0;
        void on(const OrderDelete&) { ++deletes; }
    } h;
    EXPECT_EQ(parse(t::add(1, 1, 'B', 1, 1), h), ParseStatus::ok);
    EXPECT_EQ(parse(t::del(1, 1), h), ParseStatus::ok);
    EXPECT_EQ(h.deletes, 1);
}

TEST(Itch, RejectsBadLengthBeforeReadingFields) {
    auto msg = t::add(1, 1, 'B', 1, 1);
    msg.pop_back();  // 35 bytes, an 'A' must be 36
    Recorder r;
    EXPECT_EQ(parse(msg, r), ParseStatus::bad_length);
    EXPECT_TRUE(r.got.empty());
}

TEST(Itch, RejectsUnknownTypeAndEmpty) {
    Recorder r;
    const std::vector<std::byte> junk{std::byte{'z'}, std::byte{0}};
    EXPECT_EQ(parse(junk, r), ParseStatus::unknown_type);
    EXPECT_EQ(parse(std::span<const std::byte>{}, r), ParseStatus::empty);
}

TEST(FrameReader, SplitsLengthPrefixedMessages) {
    const auto data = t::framed({t::system_event('O'), t::add(1, 1, 'B', 1, 1), t::del(1, 1)});
    FrameReader reader(data);
    std::vector<std::size_t> sizes;
    while (auto m = reader.next()) sizes.push_back(m->size());
    EXPECT_EQ(sizes, (std::vector<std::size_t>{12, 36, 19}));
    EXPECT_FALSE(reader.truncated());
    EXPECT_EQ(reader.offset(), data.size());
}

TEST(FrameReader, ReportsTruncatedFile) {
    auto data = t::framed({t::system_event('O'), t::add(1, 1, 'B', 1, 1)});
    data.resize(data.size() - 5);  // cut the last message short
    FrameReader reader(data);
    int n = 0;
    while (reader.next()) ++n;
    EXPECT_EQ(n, 1);
    EXPECT_TRUE(reader.truncated());
}

TEST(FrameReader, ReportsDanglingLengthByte) {
    auto data = t::framed({t::system_event('O')});
    data.push_back(std::byte{0});
    FrameReader reader(data);
    while (reader.next()) {}
    EXPECT_TRUE(reader.truncated());
}

}  // namespace
