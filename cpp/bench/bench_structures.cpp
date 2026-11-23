// Microbenchmarks of the two hot structures in isolation (Google Benchmark).
//
// The order-map benchmark sweeps the number of live orders from 16 K to 8 M.
// Each step multiplies the table's memory, so the sweep walks the working set
// out of L1, L2 (6 MiB here), L3 (16 MiB) and into DRAM. Where the time per
// lookup jumps shows which cache level each design stops fitting in, and the
// size of each jump shows how many dependent memory accesses a lookup makes.
// Hardware counters (PerfCounters, perf_event_open) are read around each
// benchmark's timed loop and reported per operation beside the time, so the
// explanation rests on counted misses, not only on where the time jumps.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "orderflow/book.hpp"
#include "orderflow/order_map.hpp"
#include "orderflow/perf_counters.hpp"
#include "orderflow/pool.hpp"
#include "orderflow/price_levels.hpp"

namespace {

using namespace orderflow;

// Counts hardware events over one benchmark's timed loop and reports them per
// iteration as user counters (L1d/op, dTLB/op, ...). Google Benchmark can do
// this itself only when built against libpfm, which this machine lacks.
class CountedRegion {
public:
    explicit CountedRegion(benchmark::State& state) : state_(state) { counters_.start(); }
    CountedRegion(const CountedRegion&) = delete;
    CountedRegion& operator=(const CountedRegion&) = delete;
    ~CountedRegion() {
        counters_.stop();
        const auto r = counters_.read();
        const auto n = static_cast<double>(state_.iterations());
        if (n == 0) return;
        if (r.available[PerfCounters::l1d_misses]) state_.counters["L1d/op"] = r.value[PerfCounters::l1d_misses] / n;
        if (r.available[PerfCounters::dtlb_misses]) state_.counters["dTLB/op"] = r.value[PerfCounters::dtlb_misses] / n;
        if (r.available[PerfCounters::cache_misses]) state_.counters["miss/op"] = r.value[PerfCounters::cache_misses] / n;
        if (r.available[PerfCounters::instructions] && r.value[PerfCounters::cycles] > 0) {
            state_.counters["IPC"] = r.value[PerfCounters::instructions] / r.value[PerfCounters::cycles];
        }
    }

private:
    benchmark::State& state_;
    PerfCounters counters_;
};

// ITCH-like keys: increasing reference numbers with small random gaps.
std::vector<std::uint64_t> itch_like_keys(std::size_t n, std::uint32_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint64_t> keys(n);
    std::uint64_t k = 1;
    for (auto& key : keys) key = (k += 1 + rng() % 4);
    return keys;
}

template <class Map>
void BM_OrderMapLookup(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto keys = itch_like_keys(n, 1);
    Map map(n);
    for (auto k : keys) map.insert(k, RestingOrder{1'000'000, 100, 1, Side::buy});
    // Random order of lookups: real cancels and executions hit orders of all ages.
    std::vector<std::uint64_t> probes(1 << 20);
    std::mt19937_64 rng(2);
    for (auto& p : probes) p = keys[rng() % n];
    std::size_t i = 0;
    const CountedRegion counted(state);  // reports its counters when the function returns
    for (auto _ : state) {
        benchmark::DoNotOptimize(map.find(probes[i]));
        i = (i + 1) & (probes.size() - 1);
    }
    state.SetItemsProcessed(state.iterations());
}

// Steady-state churn: insert a new order and erase a random live one, as the
// day's add/delete flow does, at a fixed number of live orders.
template <class Map>
void BM_OrderMapChurn(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    auto keys = itch_like_keys(n, 3);
    Map map(n);
    for (auto k : keys) map.insert(k, RestingOrder{1'000'000, 100, 1, Side::buy});
    std::mt19937_64 rng(4);
    std::uint64_t next = keys.back() + 1;
    const CountedRegion counted(state);  // reports its counters when the function returns
    for (auto _ : state) {
        const std::size_t victim = rng() % n;
        map.erase(keys[victim]);
        keys[victim] = next;
        map.insert(next++, RestingOrder{1'000'000, 100, 1, Side::buy});
    }
    state.SetItemsProcessed(state.iterations());
}

// One side's levels under the access pattern of a real book: most activity
// within a few ticks of the best price, some further out.
template <class Levels>
void BM_LevelsNearBest(benchmark::State& state) {
    Levels levels;
    constexpr Price mid = 1'500'000;  // $150.00
    for (Price t = 1; t <= 500; ++t) levels.add(mid - t * 100, 100);  // 500 resting levels
    std::mt19937 rng(5);
    std::geometric_distribution<int> ticks_from_best(0.3);  // mostly 0-5 ticks away
    const CountedRegion counted(state);  // reports its counters when the function returns
    for (auto _ : state) {
        const auto t = static_cast<Price>(1 + std::min(ticks_from_best(rng), 499));
        const Price p = mid - t * 100;
        levels.add(p, 10);
        levels.reduce(p, 10, true);
        benchmark::DoNotOptimize(levels.best());
    }
    state.SetItemsProcessed(state.iterations());
}

// Memory latency by working-set size: a random cyclic permutation followed one
// load at a time. Each load's address comes from the previous load, so nothing
// overlaps and the hardware prefetcher cannot guess ahead: the time per step
// is the latency of whichever cache level the working set fits in. This is
// the calibration table for reading the other results.
void BM_PointerChase(benchmark::State& state) {
    const auto bytes = static_cast<std::size_t>(state.range(0));
    const std::size_t n = bytes / sizeof(std::size_t);
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::mt19937_64 rng(6);
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<std::size_t> next(n);
    for (std::size_t i = 0; i < n; ++i) next[order[i]] = order[(i + 1) % n];  // one cycle
    std::size_t p = order[0];
    const CountedRegion counted(state);  // reports its counters when the function returns
    for (auto _ : state) {
        p = next[p];
        benchmark::DoNotOptimize(p);
    }
    state.counters["bytes"] = static_cast<double>(bytes);
}
BENCHMARK(BM_PointerChase)->RangeMultiplier(4)->Range(16 << 10, 1 << 30);

constexpr std::int64_t kMinOrders = 1 << 14, kMaxOrders = 1 << 23;

BENCHMARK(BM_OrderMapLookup<StdOrderMap<RestingOrder>>)->RangeMultiplier(4)->Range(kMinOrders, kMaxOrders);
BENCHMARK(BM_OrderMapLookup<OpenAddressingMap<RestingOrder>>)->RangeMultiplier(4)->Range(kMinOrders, kMaxOrders);
BENCHMARK(BM_OrderMapChurn<StdOrderMap<RestingOrder>>)->RangeMultiplier(4)->Range(kMinOrders, kMaxOrders);
BENCHMARK(BM_OrderMapChurn<OpenAddressingMap<RestingOrder>>)->RangeMultiplier(4)->Range(kMinOrders, kMaxOrders);
// std::unordered_map's layout with its nodes drawn from a pool.
BENCHMARK(BM_OrderMapLookup<PooledStdOrderMap<RestingOrder>>)->RangeMultiplier(4)->Range(kMinOrders, kMaxOrders);
BENCHMARK(BM_OrderMapChurn<PooledStdOrderMap<RestingOrder>>)->RangeMultiplier(4)->Range(kMinOrders, kMaxOrders);
BENCHMARK(BM_LevelsNearBest<MapLevels<Side::buy>>);
BENCHMARK(BM_LevelsNearBest<FlatLevels<Side::buy, 2048>>);

}  // namespace

BENCHMARK_MAIN();
