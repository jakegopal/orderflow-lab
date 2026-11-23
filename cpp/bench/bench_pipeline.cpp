// bench_pipeline: does splitting the replay into a feed thread and a book
// thread make it faster?
//
//   bench_pipeline [--mode single|threaded] [--cpus A,B] [--limit-mb N] <itch-file>
//
//   single    one thread: frame -> parse -> apply to the book
//   threaded  feed thread: frame -> parse -> push the decoded message into an
//             SPSC ring; book thread: pop -> apply to the book
//
// Time is end to end, including page faults on the memory-mapped file. With
// the whole 11 GB day the file doesn't fit in RAM, so this measures whether
// the feed thread's reading (and faulting pages in) overlaps the book work.
// With --limit-mb, only the first N MB are replayed; run it twice and the
// second run is warm, which measures the pipelining of CPU work alone.
//
// Both modes must end with the same book: the run prints a fingerprint of
// every symbol's best bid and ask at the end, and the anomaly count.
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>

#include "orderflow/book.hpp"
#include "orderflow/itch.hpp"
#include "orderflow/mapped_file.hpp"
#include "orderflow/spsc.hpp"

namespace {

using namespace orderflow;

// What crosses the queue: only messages that change the book, already decoded.
using BookMsg = std::variant<std::monostate, itch::AddOrder, itch::OrderExecuted, itch::OrderExecutedWithPrice,
                             itch::OrderCancel, itch::OrderDelete, itch::OrderReplace>;
constexpr std::size_t kSlots = 1 << 14;

void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

// Collects book messages from the parser into a BookMsg.
struct Decode {
    BookMsg out;
    void on(const itch::AddOrder& m) { out = m; }
    void on(const itch::OrderExecuted& m) { out = m; }
    void on(const itch::OrderExecutedWithPrice& m) { out = m; }
    void on(const itch::OrderCancel& m) { out = m; }
    void on(const itch::OrderDelete& m) { out = m; }
    void on(const itch::OrderReplace& m) { out = m; }
};

std::uint64_t fingerprint(const FlatOpenBook& book) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 0x100000001b3ULL;
    };
    for (std::size_t loc = 0; loc < (1u << 16); ++loc) {
        const auto& b = book.book(static_cast<itch::Locate>(loc));
        if (const auto bid = b.bids.best()) mix(bid->price ^ (bid->level.shares << 32) ^ loc);
        if (const auto ask = b.asks.best()) mix(ask->price ^ (ask->level.shares << 32) ^ (loc << 48));
    }
    return h;
}

struct Outcome {
    std::uint64_t messages = 0;
    double seconds = 0;
};

Outcome single(std::span<const std::byte> data, FlatOpenBook& book) {
    Outcome o;
    const auto t0 = std::chrono::steady_clock::now();
    itch::FrameReader reader(data);
    while (auto msg = reader.next()) {
        ++o.messages;
        (void)itch::parse(*msg, book);
    }
    o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return o;
}

Outcome threaded(std::span<const std::byte> data, FlatOpenBook& book, int feed_cpu, int book_cpu) {
    auto q = std::make_unique<SpscRing<BookMsg, kSlots>>();
    std::atomic<bool> done{false};
    Outcome o;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread feed([&] {
        pin(feed_cpu);
        itch::FrameReader reader(data);
        Decode d;
        while (auto msg = reader.next()) {
            ++o.messages;
            d.out = std::monostate{};
            (void)itch::parse(*msg, d);
            if (std::holds_alternative<std::monostate>(d.out)) continue;  // not a book message
            while (!q->try_push(d.out)) {
            }
        }
        done.store(true, std::memory_order_release);
    });
    pin(book_cpu);
    BookMsg m;
    auto apply = [&](const auto& x) {
        if constexpr (!std::is_same_v<std::decay_t<decltype(x)>, std::monostate>) book.on(x);
    };
    while (true) {
        if (q->try_pop(m)) {
            std::visit(apply, m);
        } else if (done.load(std::memory_order_acquire)) {
            // done was set after the last push, so one more drain sees everything.
            while (q->try_pop(m)) std::visit(apply, m);
            break;
        }
    }
    feed.join();
    o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "single";
    int a = 2, b = 4;
    std::size_t limit_mb = 0;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (std::strcmp(argv[i], "--cpus") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%d,%d", &a, &b);
        } else if (std::strcmp(argv[i], "--limit-mb") == 0 && i + 1 < argc) {
            limit_mb = std::stoul(argv[++i]);
        } else {
            path = argv[i];
        }
    }
    if (path == nullptr || (mode != "single" && mode != "threaded")) {
        std::fprintf(stderr, "usage: %s [--mode single|threaded] [--cpus A,B] [--limit-mb N] <file>\n", argv[0]);
        return 2;
    }
    try {
        const MappedFile file(path);
        auto data = file.bytes();
        if (limit_mb != 0) data = data.first(std::min(data.size(), limit_mb << 20));  // the last frame may be cut
        auto book = std::make_unique<FlatOpenBook>();
        Outcome o;
        if (mode == "single") {
            pin(b);
            o = single(data, *book);
        } else {
            o = threaded(data, *book, a, b);
        }
        std::printf("%-8s %llu messages in %.2f s = %.2f M msg/s; anomalies %llu; fingerprint %016llx\n",
                    mode.c_str(), static_cast<unsigned long long>(o.messages), o.seconds,
                    static_cast<double>(o.messages) / o.seconds / 1e6,
                    static_cast<unsigned long long>(book->anomalies().total()),
                    static_cast<unsigned long long>(fingerprint(*book)));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
