// itch_replay: rebuild every symbol's book for a whole ITCH day and check it.
//
//   itch_replay [--variant flat-open|flat-std|map-open|map-std] <itch-file>
//
// There is no published end-of-day book to compare against, so correctness is
// checked through properties a correct book must have:
//
//  1. No anomalies. Replayed from the first message, every execute, cancel,
//     delete and replace must name a live order and never take more shares
//     than it has. One missed or misparsed message breaks this quickly.
//  2. Executions happen at the top of the book. NASDAQ matches by price, then
//     time, so an order hit by an 'E' during continuous trading must be at
//     the best price on its side. A book with stale or missing levels would
//     often show a better price than the executed order's.
//  3. A book can be locked or crossed (bid >= ask) only while its symbol's
//     matching is suspended (halt, volatility pause, quotation-only period,
//     announced by 'H' messages), because otherwise the engine executes
//     crossing orders. Counts are split by session phase and by trading state.
//  4. A top-of-book fingerprint: a hash of (symbol, best bid, best ask, sizes)
//     after every book message. Every variant must produce the same value,
//     which compares the four implementations across all 368M messages.
//
// Exits 1 if there is any anomaly or the file is malformed.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "orderflow/book.hpp"
#include "orderflow/itch.hpp"
#include "orderflow/mapped_file.hpp"

namespace {

using namespace orderflow;

// Session phases, from the 'S' system event codes.
enum Phase { pre_market = 0, regular = 1, after_hours = 2 };
constexpr const char* kPhaseNames[] = {"pre-market", "regular", "after-hours"};

struct CrossEvent {
    std::uint64_t timestamp_ns;
    Locate locate;
    Price bid;
    Price ask;
};

struct Checks {
    std::uint64_t messages = 0;
    std::uint64_t book_messages = 0;
    std::uint64_t exec_total[3]{};
    std::uint64_t exec_at_best[3]{};
    std::uint64_t locked[3]{};   // bid == ask after a message
    std::uint64_t crossed[3]{};  // bid > ask after a message
    // The same states split by the symbol's trading state ('H' messages): a
    // correct book can lock or cross only while matching is suspended.
    std::uint64_t locked_or_crossed_while_trading = 0;
    std::uint64_t locked_or_crossed_while_halted = 0;
    // For the states seen while trading: the longest time since that symbol's
    // last resume ('H' with state 'T'). A symbol that never resumed has
    // resumed_at 0, so a cross on it would show up here as hours, not ms.
    std::uint64_t max_ns_after_resume = 0;
    std::uint64_t fingerprint = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
    std::vector<CrossEvent> first_crosses[3];  // the first few per phase, for the write-up
    std::vector<bool> ever_crossed = std::vector<bool>(1 << 16);
    std::vector<std::uint64_t> crosses_by_minute = std::vector<std::uint64_t>(24 * 60);
};

template <class Book>
class Validator {
public:
    explicit Validator(Book& book) : book_(book) {}

    void on(const itch::StockDirectory& m) { symbols_[m.header.locate] = m.stock; }
    void on(const itch::StockTradingAction& m) {
        trading_[m.header.locate] = m.trading_state == 'T';
        if (m.trading_state == 'T') resumed_at_[m.header.locate] = m.header.timestamp_ns;
    }
    void on(const itch::SystemEvent& m) {
        if (m.event_code == 'Q') phase_ = regular;      // market open, 9:30
        if (m.event_code == 'M') phase_ = after_hours;  // market close, 16:00
    }
    void on(const itch::AddOrder& m) { apply(m, m.header.locate); }
    void on(const itch::OrderExecuted& m) {
        check_at_best(m.order_ref);
        apply(m, m.header.locate);
    }
    void on(const itch::OrderExecutedWithPrice& m) { apply(m, m.header.locate); }
    void on(const itch::OrderCancel& m) { apply(m, m.header.locate); }
    void on(const itch::OrderDelete& m) { apply(m, m.header.locate); }
    void on(const itch::OrderReplace& m) { apply(m, m.header.locate); }
    void on_other(const itch::Header&, std::span<const std::byte>) {}

    Checks checks;

    [[nodiscard]] std::string_view symbol(Locate l) const { return itch::trimmed(symbols_[l]); }

private:
    void check_at_best(OrderRef ref) {
        const RestingOrder* o = book_.order(ref);
        if (o == nullptr) return;  // counted as an anomaly by the book
        const auto& b = book_.book(o->locate);
        const auto best = o->side == Side::buy ? b.bids.best() : b.asks.best();
        ++checks.exec_total[phase_];
        if (best && best->price == o->price) ++checks.exec_at_best[phase_];
    }

    template <class M>
    void apply(const M& m, Locate locate) {
        const std::uint64_t ts = m.header.timestamp_ns;
        book_.on(m);
        ++checks.book_messages;
        // Message boundary: the only point where the book state is real.
        const auto& b = book_.book(locate);
        const auto bid = b.bids.best();
        const auto ask = b.asks.best();
        if (bid && ask) {
            if (bid->price == ask->price) ++checks.locked[phase_];
            if (bid->price > ask->price) ++checks.crossed[phase_];
            if (bid->price >= ask->price) {
                ++(trading_[locate] ? checks.locked_or_crossed_while_trading
                                    : checks.locked_or_crossed_while_halted);
                if (trading_[locate]) {
                    checks.max_ns_after_resume =
                        std::max(checks.max_ns_after_resume, ts - resumed_at_[locate]);
                }
                checks.ever_crossed[locate] = true;
                ++checks.crosses_by_minute[ts / 60'000'000'000];
                if (checks.first_crosses[phase_].size() < 6) {
                    checks.first_crosses[phase_].push_back({ts, locate, bid->price, ask->price});
                }
            }
        }
        mix(locate);
        mix(bid ? bid->price : 0);
        mix(bid ? bid->level.shares : 0);
        mix(ask ? ask->price : 0);
        mix(ask ? ask->level.shares : 0);
    }

    void mix(std::uint64_t v) {  // FNV-1a over the 8 bytes of v
        for (int i = 0; i < 8; ++i) {
            checks.fingerprint ^= (v >> (8 * i)) & 0xFF;
            checks.fingerprint *= 0x100000001b3ULL;
        }
    }

    Book& book_;
    Phase phase_ = pre_market;
    std::vector<itch::Symbol> symbols_ = std::vector<itch::Symbol>(1 << 16);
    // Every symbol gets an 'H' with state 'T' at the start of the day; until
    // one arrives, treat the symbol as trading so nothing is excused early.
    std::vector<bool> trading_ = std::vector<bool>(1 << 16, true);
    std::vector<std::uint64_t> resumed_at_ = std::vector<std::uint64_t>(1 << 16);
};

template <class Book>
int run(std::span<const std::byte> data, std::string_view variant) {
    Book book;
    Validator<Book> v(book);
    itch::FrameReader reader(data);
    std::uint64_t malformed = 0;
    const auto start = std::chrono::steady_clock::now();
    while (auto msg = reader.next()) {
        ++v.checks.messages;
        if (itch::parse(*msg, v) != itch::ParseStatus::ok) ++malformed;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const Checks& c = v.checks;
    const Anomalies& a = book.anomalies();
    std::printf("variant %.*s\n", static_cast<int>(variant.size()), variant.data());
    std::printf("messages %llu (book %llu), malformed %llu, truncated %s\n",
                static_cast<unsigned long long>(c.messages),
                static_cast<unsigned long long>(c.book_messages),
                static_cast<unsigned long long>(malformed), reader.truncated() ? "yes" : "no");
    std::printf("anomalies: unknown %llu, duplicate %llu, zero-ref %llu, over-reduction %llu\n",
                static_cast<unsigned long long>(a.unknown_order),
                static_cast<unsigned long long>(a.duplicate_order),
                static_cast<unsigned long long>(a.zero_ref),
                static_cast<unsigned long long>(a.over_reduction));
    for (int p = 0; p < 3; ++p) {
        const auto total = c.exec_total[p];
        // Exact counts, not a rounded percentage: 100.000% could hide a miss.
        std::printf("%-12s E executions %10llu, at best %10llu (not %llu); locked %6llu, crossed %6llu\n",
                    kPhaseNames[p], static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(c.exec_at_best[p]),
                    static_cast<unsigned long long>(total - c.exec_at_best[p]),
                    static_cast<unsigned long long>(c.locked[p]),
                    static_cast<unsigned long long>(c.crossed[p]));
    }
    std::size_t symbols_crossed = 0;
    for (bool b : c.ever_crossed) symbols_crossed += b ? 1 : 0;
    std::printf("symbols ever locked or crossed: %zu\n", symbols_crossed);
    std::printf("locked or crossed while the symbol was trading: %llu; "
                "while halted, paused or quotation-only: %llu\n",
                static_cast<unsigned long long>(c.locked_or_crossed_while_trading),
                static_cast<unsigned long long>(c.locked_or_crossed_while_halted));
    std::printf("  while trading, at most %.3f ms after the symbol resumed from a halt\n",
                static_cast<double>(c.max_ns_after_resume) / 1e6);
    std::vector<std::size_t> minutes(c.crosses_by_minute.size());
    for (std::size_t i = 0; i < minutes.size(); ++i) minutes[i] = i;
    std::partial_sort(minutes.begin(), minutes.begin() + 8, minutes.end(),
                      [&](std::size_t x, std::size_t y) {
                          return c.crosses_by_minute[x] > c.crosses_by_minute[y];
                      });
    std::printf("busiest minutes for locked/crossed states:");
    for (std::size_t i = 0; i < 8 && c.crosses_by_minute[minutes[i]] != 0; ++i) {
        std::printf(" %02zu:%02zu (%llu)", minutes[i] / 60, minutes[i] % 60,
                    static_cast<unsigned long long>(c.crosses_by_minute[minutes[i]]));
    }
    std::printf("\nfirst events per phase:\n");
    for (const auto& phase_events : c.first_crosses) {
        for (const CrossEvent& e : phase_events) {
            const std::uint64_t s = e.timestamp_ns / 1'000'000'000;
            const auto sym = v.symbol(e.locate);
            std::printf("  %02llu:%02llu:%02llu.%09llu %-8.*s bid %u.%04u ask %u.%04u\n",
                        static_cast<unsigned long long>(s / 3600), static_cast<unsigned long long>(s / 60 % 60),
                        static_cast<unsigned long long>(s % 60),
                        static_cast<unsigned long long>(e.timestamp_ns % 1'000'000'000),
                        static_cast<int>(sym.size()), sym.data(), e.bid / itch::kPriceScale,
                        e.bid % itch::kPriceScale, e.ask / itch::kPriceScale, e.ask % itch::kPriceScale);
        }
    }
    std::printf("live orders at end %zu\n", book.live_orders());
    std::printf("top-of-book fingerprint %016llx\n",
                static_cast<unsigned long long>(c.fingerprint));
    std::printf("time %.2f s (%.1f M msg/s, I/O included)\n", secs,
                static_cast<double>(c.messages) / secs / 1e6);
    return (a.total() == 0 && malformed == 0 && !reader.truncated()) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string variant = "flat-open";
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
            variant = argv[++i];
        } else {
            path = argv[i];
        }
    }
    if (path == nullptr) {
        std::fprintf(stderr, "usage: %s [--variant flat-open|flat-std|map-open|map-std] <file>\n",
                     argv[0]);
        return 2;
    }
    try {
        const MappedFile file(path);
        if (variant == "flat-open") return run<FlatOpenBook>(file.bytes(), variant);
        if (variant == "flat-std") return run<FlatStdBook>(file.bytes(), variant);
        if (variant == "map-open") return run<MapOpenBook>(file.bytes(), variant);
        if (variant == "map-std") return run<MapStdBook>(file.bytes(), variant);
        std::fprintf(stderr, "unknown variant %s\n", variant.c_str());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
