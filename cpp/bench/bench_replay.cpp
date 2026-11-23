// bench_replay: whole-day ITCH replay benchmark, throughput and per-message latency.
//
//   bench_replay [--variant V] [--latency] [--chunk-mb N] <itch-file>
//
// What is measured. Parse + book update for every message of the day, from
// memory. The file is 11 GB and this machine has 7 GB of RAM, so it cannot be
// cached whole. Instead the replay runs in chunks: before timing each chunk
// (default 512 MB), its pages are touched so they are resident, and only the
// processing of a resident chunk is timed. The number is book work, not disk
// speed. The book's state carries across chunks, so it is one continuous day.
//
// Two modes, because timing every message perturbs what it measures:
//   default    one clock read per chunk -> throughput (messages per second)
//   --latency  rdtsc around every message -> histogram -> p50/p99/p99.9/max.
//              The timer's own cost is measured and printed alongside.
//
// Honest-benchmark notes printed with every run: CPU, the core the process is
// pinned to (taskset), compiler and flags, and that the data is warm in the
// page cache but the book's own memory is whatever the replay left in cache.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <sched.h>

#include "orderflow/book.hpp"
#include "orderflow/itch.hpp"
#include "orderflow/mapped_file.hpp"
#include "orderflow/perf_counters.hpp"
#include "orderflow/tsc.hpp"

namespace {

using namespace orderflow;

// Histogram in TSC ticks: exact below 8192 ticks, then power-of-two buckets.
class Histogram {
public:
    void add(std::uint64_t ticks) noexcept {
        ++count_;
        max_ = std::max(max_, ticks);
        if (ticks < kLinear) {
            ++linear_[ticks];
        } else {
            ++log_[std::min<std::size_t>(63, static_cast<std::size_t>(std::bit_width(ticks)))];
        }
    }
    // Smallest value v with at least q of the samples <= v (log buckets report their top).
    [[nodiscard]] std::uint64_t quantile(double q) const noexcept {
        const auto want = static_cast<std::uint64_t>(q * static_cast<double>(count_));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kLinear; ++i) {
            seen += linear_[i];
            if (seen > want) return i;
        }
        for (std::size_t b = 0; b < 64; ++b) {
            seen += log_[b];
            if (seen > want) return (std::uint64_t{1} << b) - 1;
        }
        return max_;
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

private:
    static constexpr std::size_t kLinear = 8192;
    std::vector<std::uint64_t> linear_ = std::vector<std::uint64_t>(kLinear);
    std::array<std::uint64_t, 64> log_{};
    std::uint64_t count_ = 0;
    std::uint64_t max_ = 0;
};

// Written once per chunk so the compiler cannot drop the prefault reads.
volatile std::uint64_t g_prefault_sink = 0;

// Touch one byte per page so the chunk is resident before it is timed.
std::uint64_t prefault(std::span<const std::byte> chunk) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < chunk.size(); i += 4096) sum += std::to_integer<std::uint8_t>(chunk[i]);
    return sum;
}

// Anonymous resident memory in KiB: the book's own heap, excluding the pages
// of the mmapped input file (which ordinary RSS would count).
std::uint64_t rss_anon_kib() {
    std::ifstream f("/proc/self/status");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("RssAnon:", 0) == 0) return std::stoull(line.substr(8));
    }
    return 0;
}

struct Result {
    std::uint64_t peak_anon_kib = 0;  // sampled after every chunk
    std::uint64_t messages = 0;
    double seconds = 0;  // timed processing only
    std::uint64_t anomalies = 0;
    std::array<Histogram, 4> by_kind;  // 0 all, 1 add, 2 execute/cancel/delete, 3 replace
    PerfCounters::Reading counters;    // throughput mode only: timed regions, user space
};

int kind_of(char type) {
    switch (type) {
        case 'A': case 'F': return 1;
        case 'E': case 'C': case 'X': case 'D': return 2;
        case 'U': return 3;
        default: return 0;
    }
}

template <class Book>
Result run(std::span<const std::byte> data, std::size_t chunk_bytes, bool latency) {
    Book book;
    Result r;
    // Counted in throughput mode only: in latency mode the timers themselves
    // would be most of what the counters see.
    PerfCounters counters;
    itch::FrameReader reader(data);
    while (reader.offset() < data.size()) {
        // Fault in the next chunk (plus one frame of slack), untimed.
        const std::size_t from = reader.offset();
        const std::size_t chunk_end = std::min(data.size(), from + chunk_bytes);
        g_prefault_sink = prefault(data.subspan(from, std::min(data.size(), chunk_end + 65'536) - from));

        if (!latency) counters.start();
        const auto t0 = std::chrono::steady_clock::now();
        while (reader.offset() < chunk_end) {
            auto msg = reader.next();
            if (!msg) break;
            ++r.messages;
            if (latency) {
                const auto a = tsc_start();
                (void)itch::parse(*msg, book);
                const auto b = tsc_stop();
                r.by_kind[0].add(b - a);
                if (const int k = kind_of(static_cast<char>((*msg)[0])); k != 0) r.by_kind[static_cast<std::size_t>(k)].add(b - a);
            } else {
                (void)itch::parse(*msg, book);
            }
        }
        r.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!latency) counters.stop();
        r.peak_anon_kib = std::max(r.peak_anon_kib, rss_anon_kib());
    }
    r.anomalies = book.anomalies().total();
    if (!latency) r.counters = counters.read();
    return r;
}

std::string cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
    }
    return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
    std::string variant = "flat-open";
    bool latency = false;
    std::size_t chunk_mb = 512;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
            variant = argv[++i];
        } else if (std::strcmp(argv[i], "--latency") == 0) {
            latency = true;
        } else if (std::strcmp(argv[i], "--chunk-mb") == 0 && i + 1 < argc) {
            chunk_mb = std::stoul(argv[++i]);
        } else {
            path = argv[i];
        }
    }
    if (path == nullptr) {
        std::fprintf(stderr, "usage: %s [--variant V] [--latency] [--chunk-mb N] <file>\n", argv[0]);
        return 2;
    }
    try {
        const MappedFile file(path);
        const std::size_t chunk = chunk_mb << 20;
        Result r;
        if (variant == "flat-open") r = run<FlatOpenBook>(file.bytes(), chunk, latency);
        else if (variant == "flat-std") r = run<FlatStdBook>(file.bytes(), chunk, latency);
        else if (variant == "map-open") r = run<MapOpenBook>(file.bytes(), chunk, latency);
        else if (variant == "map-std") r = run<MapStdBook>(file.bytes(), chunk, latency);
        else if (variant == "flat-pool") r = run<FlatPoolBook>(file.bytes(), chunk, latency);
        else {
            std::fprintf(stderr, "unknown variant %s\n", variant.c_str());
            return 2;
        }

        std::printf("cpu: %s, pinned to cpu %d\n", cpu_model().c_str(), sched_getcpu());
        std::printf("compiler: %s, flags: %s\n", ORDERFLOW_COMPILER, ORDERFLOW_FLAGS);
        std::printf("variant %s: %llu messages in %.2f s timed = %.1f M msg/s, %.1f ns/msg mean%s\n",
                    variant.c_str(), static_cast<unsigned long long>(r.messages), r.seconds,
                    static_cast<double>(r.messages) / r.seconds / 1e6,
                    r.seconds * 1e9 / static_cast<double>(r.messages),
                    latency ? " (with per-message timers)" : "");
        std::printf("peak anonymous memory %.0f MiB (book state; the input file's pages excluded)\n",
                    static_cast<double>(r.peak_anon_kib) / 1024.0);
        if (!latency) {
            std::printf("hardware counters over the timed regions (user space):\n");
            PerfCounters::print(r.counters, static_cast<double>(r.messages), "message");
        } else {
            const double per_ns = tsc_per_ns();
            Histogram overhead;
            for (int i = 0; i < 1'000'000; ++i) {
                const auto a = tsc_start();
                const auto b = tsc_stop();
                overhead.add(b - a);
            }
            auto ns = [&](std::uint64_t ticks) { return static_cast<double>(ticks) / per_ns; };
            std::printf("timer: %.3f TSC ticks/ns; empty timed region p50 %.1f ns (included below)\n",
                        per_ns, ns(overhead.quantile(0.5)));
            constexpr const char* kNames[] = {"all", "add", "exec/cancel/delete", "replace"};
            for (std::size_t k = 0; k < 4; ++k) {
                const Histogram& h = r.by_kind[k];
                std::printf("  %-19s n=%11llu  p50 %6.1f  p99 %7.1f  p99.9 %8.1f  max %10.1f ns\n",
                            kNames[k], static_cast<unsigned long long>(h.count()), ns(h.quantile(0.5)),
                            ns(h.quantile(0.99)), ns(h.quantile(0.999)), ns(h.max()));
            }
        }
        return r.anomalies == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
