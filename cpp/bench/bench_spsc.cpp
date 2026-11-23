// bench_spsc: the SPSC ring against its own ablations and a mutex queue.
//
//   bench_spsc [--cpus A,B] [--messages N]
//
// Two threads pinned to two CPUs. Throughput: the producer pushes N sequence
// numbers, and the consumer pops them and checks each one is the next
// expected, so a lost or reordered message fails the run instead of looking
// fast. Latency: ping-pong. A sends a timestamp, B echoes it back on a second
// queue, and A records the round trip (two one-way hand-offs) in TSC ticks.
//
// Variants, all with the same 4096-slot storage:
//   spsc            padded counters + cached counters (spsc.hpp default)
//   spsc-unpadded   both counters in one cache line: false sharing
//   spsc-nocache    padded, but every operation reads the other counter
//   mutex           std::mutex around the same ring
// Every variant runs 5 times and the median is reported, with min and max,
// so run-to-run spread is visible.
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "orderflow/spsc.hpp"
#include "orderflow/tsc.hpp"

namespace {

using namespace orderflow;
constexpr std::size_t kCap = 4096;

template <class T, std::size_t Capacity>
class MutexRing {
public:
    template <class U>
    bool try_push(U&& v) {
        const std::lock_guard lock(m_);
        if (tail_ - head_ >= Capacity) return false;
        slots_[tail_++ & (Capacity - 1)] = std::forward<U>(v);
        return true;
    }
    bool try_pop(T& out) {
        const std::lock_guard lock(m_);
        if (head_ == tail_) return false;
        out = slots_[head_++ & (Capacity - 1)];
        return true;
    }

private:
    std::mutex m_;
    std::uint64_t head_ = 0, tail_ = 0;
    std::array<T, Capacity> slots_{};
};

void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) {
        std::fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
    }
}

// Returns messages per second, or 0 if the consumer saw a wrong value.
template <class Q>
double throughput(int cpu_a, int cpu_b, std::uint64_t n) {
    auto q = std::make_unique<Q>();
    std::atomic<bool> ok{true};
    std::atomic<int> ready{0};
    std::thread consumer([&] {
        pin(cpu_b);
        ready.fetch_add(1);
        while (ready.load() < 2) {
        }
        std::uint64_t expect = 0, v = 0;
        while (expect < n) {
            if (q->try_pop(v)) {
                if (v != expect) ok.store(false);
                ++expect;
            }
        }
    });
    pin(cpu_a);
    ready.fetch_add(1);
    while (ready.load() < 2) {
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < n;) {
        if (q->try_push(i)) ++i;
    }
    consumer.join();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return ok.load() ? static_cast<double>(n) / s : 0.0;
}

struct Percentiles {
    double p50, p99, p999;
};

template <class Q>
Percentiles ping_pong(int cpu_a, int cpu_b, std::size_t rounds, double ticks_per_ns) {
    auto there = std::make_unique<Q>();
    auto back = std::make_unique<Q>();
    std::thread echo([&] {
        pin(cpu_b);
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < rounds; ++i) {
            while (!there->try_pop(v)) {
            }
            while (!back->try_push(v)) {
            }
        }
    });
    pin(cpu_a);
    std::vector<std::uint64_t> rtt(rounds);
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < rounds; ++i) {
        const std::uint64_t t = __rdtsc();
        while (!there->try_push(t)) {
        }
        while (!back->try_pop(v)) {
        }
        rtt[i] = __rdtsc() - v;
    }
    echo.join();
    const std::size_t skip = rounds / 10;  // warm-up
    std::vector<std::uint64_t> s(rtt.begin() + static_cast<std::ptrdiff_t>(skip), rtt.end());
    std::sort(s.begin(), s.end());
    auto q = [&](double f) { return static_cast<double>(s[static_cast<std::size_t>(f * static_cast<double>(s.size() - 1))]) / ticks_per_ns; };
    return {q(0.50), q(0.99), q(0.999)};
}

template <class Q>
void run(const char* name, int a, int b, std::uint64_t n, double tpn) {
    std::vector<double> tput;
    for (int r = 0; r < 5; ++r) tput.push_back(throughput<Q>(a, b, n));
    std::sort(tput.begin(), tput.end());
    if (tput.front() == 0.0) {
        std::printf("%-15s FAILED: a message was lost or reordered\n", name);
        return;
    }
    std::vector<Percentiles> lat;
    for (int r = 0; r < 5; ++r) lat.push_back(ping_pong<Q>(a, b, 200'000, tpn));
    std::sort(lat.begin(), lat.end(), [](const Percentiles& x, const Percentiles& y) { return x.p50 < y.p50; });
    const auto& m = lat[2];
    std::printf("%-15s %7.1f M msg/s (min %.1f, max %.1f)   round trip p50 %6.0f  p99 %6.0f  p99.9 %7.0f ns\n",
                name, tput[2] / 1e6, tput.front() / 1e6, tput.back() / 1e6, m.p50, m.p99, m.p999);
}

}  // namespace

int main(int argc, char** argv) {
    int a = 2, b = 4;
    std::uint64_t n = 100'000'000;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--cpus") == 0) {
            std::sscanf(argv[++i], "%d,%d", &a, &b);
        } else if (std::strcmp(argv[i], "--messages") == 0) {
            n = std::stoull(argv[++i]);
        }
    }
    const double tpn = tsc_per_ns();
    std::printf("producer cpu %d, consumer cpu %d, %llu messages of 8 bytes per throughput run, %zu slots\n", a, b,
                static_cast<unsigned long long>(n), kCap);
    run<SpscRing<std::uint64_t, kCap>>("spsc", a, b, n, tpn);
    run<SpscRing<std::uint64_t, kCap, false>>("spsc-unpadded", a, b, n, tpn);
    run<SpscRingNoCache<std::uint64_t, kCap>>("spsc-nocache", a, b, n, tpn);
    run<MutexRing<std::uint64_t, kCap>>("mutex", a, b, n, tpn);
}
