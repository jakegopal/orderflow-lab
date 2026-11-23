# 4. Benchmarks: method, results, and what they mean

Code: [bench_replay.cpp](../../cpp/bench/bench_replay.cpp),
[bench_structures.cpp](../../cpp/bench/bench_structures.cpp),
[perf_counters.hpp](../../cpp/include/orderflow/perf_counters.hpp),
[run_benchmarks.sh](../../scripts/run_benchmarks.sh).
Raw output: [results/](results/) (every file starts with the build and
machine it came from).

## Method

**Machine.** AMD Ryzen AI 5 340 (Zen 5, 6 cores / 12 threads, 48 KiB L1d and
1 MiB L2 per core, 16 MiB shared L3), 7.5 GiB RAM, under WSL2 (kernel
6.6.87.2-microsoft-standard). GCC 15.2, `-O3 -DNDEBUG` (CMake Release).
Every run pinned to CPU 2 with `taskset`, nothing else heavy running.

**Whole-day replay** (`bench_replay`). Parse plus book update for all
368,366,634 messages of 01302019, from memory. The 11 GB file doesn't fit in
7.5 GB of RAM, so the replay runs in 512 MB chunks. Each chunk is faulted in
(untimed) and then processed (timed). The number is book work, not disk speed.
The book's state carries over between chunks, so it is one continuous day.
It is a **warm-data, cold-book** measurement: the input is resident, but the
book's own memory is wherever the replay left it.

- **Throughput mode:** one clock read per chunk. Each variant runs twice
  (`run 1`, `run 2` in the raw file), so the run-to-run spread is visible.
- **Latency mode:** serialised TSC reads around every message, into a
  histogram. The timer's own cost (the empty region) is printed and is
  *included* in the percentiles.
- **Hardware counters** come from `perf_event_open`, counted over the timed
  regions only, user space only. The `perf` tool doesn't run on WSL2's
  kernel, but the system call does. Cycles, instructions, L1d and dTLB load
  misses, and branch misses are available. **LLC-load-misses is unsupported**
  (a known WSL2 limitation, github.com/microsoft/WSL/issues/8480). The
  generic `cache-misses` event is counted, but on AMD its definition is the
  vendor's, so it is used only for comparisons between variants.

**Microbenchmarks** (`bench_structures`, Google Benchmark, 3 repetitions,
medians). Each structure in isolation, with the same counters per operation.

## Calibration: what a cache miss costs on this machine

`BM_PointerChase` follows a random cyclic permutation, one dependent load at
a time, so no two loads overlap and the prefetcher can't guess ahead:

| working set | ns per load | where it lives |
|---|---|---|
| 16 KiB | 1.05 | L1 |
| 256 KiB | 2.83 | L2 |
| 1 MiB | 5.16 | L2, the edge |
| 4 MiB | 10.7 | L3 |
| 16 MiB | 56.8 | L3 edge / DRAM |
| 64 MiB | 124 | DRAM, plus TLB misses (0.76 dTLB misses per load) |
| 1 GiB | 159 | DRAM, every load a TLB miss too (0.99 per load) |

So, roughly: L1 1 ns, L2 3–5 ns, L3 ~11 ns, DRAM ~125–160 ns. A design that
needs one fewer *dependent* DRAM access per message saves ~100 ns, which is
more than the whole parse.

## Result 1: the order map

Lookups of random live orders (`BM_OrderMapLookup`), ns per lookup:

| live orders | `std::unordered_map` | open addressing | ratio |
|---|---|---|---|
| 16 K | 7.2 | 2.1 | 3.4× |
| 256 K | 16.7 | 4.0 | 4.2× |
| 1 M | 35.2 | 11.7 | 3.0× |
| 8 M | 41.6 | 12.7 | 3.3× |

L1d misses per lookup are flat at **~3.6 for `std::unordered_map` and ~1.24
for open addressing**, at every size. That count *is* the design:

- chained: hash → bucket array → pointer to node → node (and sometimes the
  next node in the chain). About three dependent memory touches, each in a
  different, unrelated cache line.
- open addressing: hash → slot. The key and value sit together in the slot,
  so it is one line, and linear probing usually stays in it.

Why are 8 M lookups only ~42 ns when a DRAM access is ~150 ns? Because these
lookups are *independent*: the next key doesn't depend on the previous
result, so the out-of-order core runs several at once (memory-level
parallelism). The pointer chase allows none of that. In the replay, each
message's lookup is independent of the last one too, which is why the replay
is faster per message than its miss count alone suggests.

Steady-state churn (insert a new order and erase a random one), ns per pair:
`std::unordered_map` 40 → 442 from 16 K to 8 M live orders, open addressing
24 → 135. The chained map pays a `malloc` and a `free` per order on top of
its misses (Phase 4's pool allocator measures how much of the gap that is).

## Result 2: price levels

`BM_LevelsNearBest`: add and remove 10 shares at a price drawn mostly within
a few ticks of the best, then read the best, on a side with 500 levels:
**`std::map` 35.0 ns, flat window 20.2 ns**. Both fit in L1 here (L1d misses
≈ 0), so this is instruction count and branchiness: the map walks ~9 tree
levels of pointer comparisons, and the window does one subtraction and one
array write. IPC is 2.1 vs 3.0.

## Result 3: the whole day

Timed replay of all 368,366,634 messages, mean ns per message (two runs each):

| variant | price levels | order map | run 1 | run 2 | M msg/s | peak book memory |
|---|---|---|---|---|---|---|
| map-std | `std::map` | `std::unordered_map` | 311.3 | 315.0 | 3.2 | 162 MiB |
| map-open | `std::map` | open addressing | 220.7 | 241.4 | 4.1–4.5 | 241 MiB |
| flat-std | flat window | `std::unordered_map` | 248.9 | 246.4 | 4.0–4.1 | 710 MiB |
| flat-open | flat window | open addressing | 171.4 | 147.2 | 5.8–6.8 | 790 MiB |

Per message, from the hardware counters (run 1):

| variant | cycles | instructions | IPC | L1d misses | dTLB misses | branch misses |
|---|---|---|---|---|---|---|
| map-std | 1,468 | 386 | 0.26 | 9.4 | 3.9 | 4.1 |
| map-open | 1,030 | 291 | 0.28 | 7.0 | 2.9 | 4.0 |
| flat-std | 1,033 | 321 | 0.31 | 5.9 | 1.9 | 2.1 |
| flat-open | 647 | 231 | 0.36 | 4.0 | 1.4 | 2.0 |

**What this says.**

1. **The two choices are independent and roughly additive.** Open addressing
   saves ~2.4 L1d misses and ~1.0 dTLB miss per message whichever price
   levels are used. The flat window saves ~3.5 L1d misses, ~2 dTLB misses and
   ~2 branch misses whichever order map is used. Together they take the
   replay from 311 to 147–171 ns per message (1.8–2.1×).
2. **The replay is memory-bound, not compute-bound.** IPC is 0.26–0.36 on a
   core that sustains 3+ (the levels microbenchmark). Most of each message's
   ~650–1,470 cycles are spent waiting on loads. The open-addressing table
   has 8 M slots of 24 bytes (192 MB, the 241 MiB peak of map-open), twelve
   times the L3, so a lookup of an arbitrary order is usually a DRAM access.
   That is the floor
   this design leaves, and it is the target of any future work (software
   prefetching of the next message's order slot is the obvious next step).
3. **Branch misses halve with the flat window** (4.1 → 2.0 per message): a
   tree walk's comparisons depend on the data, while an array index has no
   branch to mispredict.
4. **Memory is the price.** The flat window costs 32 KiB per side per symbol
   that trades, which is the 162 → 710 MiB jump. On this machine that is
   cheap; on a box running many feeds it is a real budget.

## Result 4: latency per message

Serialised TSC reads around every message, all 368 M messages, in ns. The
timer itself is included. Its empty region reads 10–20 ns, because this VM's
TSC advances in steps of about 10 ns, so treat every number as ±10 ns.

| variant | p50 | p99 | p99.9 | max | mean with timers |
|---|---|---|---|---|---|
| map-std | 270 | 1,824 | 2,966 | 4.1 ms | 446 |
| map-open | 220 | 1,433 | 2,274 | 10.2 ms | 360 |
| flat-std | 160 | 1,222 | 2,645 | 13.5 ms | 355 |
| flat-open | 180 | 912 | 1,924 | 116 ms | 293 |

By message kind for flat-open: adds p50 190 / p99 661 ns; executes,
cancels and deletes p50 170 / p99 1,002 ns; replaces (two order-map
operations) p50 291 / p99 1,032 ns.

**What this says.**

- **The median hides the difference; the tail shows it.** flat-std has the
  lowest p50 (160 ns), but flat-open has the best p99 and p99.9 by 25–35%.
  The tail is where the extra dependent DRAM accesses of a chained map land:
  a lookup that misses in every level costs three DRAM trips, not one.
- **Per-message timing costs ~40–60%** (mean 293 vs 171 ns for flat-open).
  The serialising `lfence`/`rdtscp` stops the core overlapping one message's
  misses with the next message's work. That is the memory-level
  parallelism that makes the throughput run faster. So throughput and
  latency are measured in separate runs, and neither is derived from the other.
- **The max is not the book.** 4–116 ms maxima are page faults, preemption by
  the Windows host, or a FlatLevels re-centre. They happen a handful of times
  in 368 M messages (they don't move p99.9). A production system would pin
  memory (`mlock`), isolate the core and pre-fault. None of that is possible
  inside WSL2, so the maxima are reported but not interpreted further.

## Caveats

- **Warm data, cold book,** single-threaded, pinned. A production feed
  handler would also see the network stack and its own threading (Phase 4).
- **Run-to-run spread** is 1–2% for map-std and flat-std, 9% for map-open
  (220.7 vs 241.4 ns) and 15% for flat-open (171.4 vs 147.2 ns). WSL2 shares the machine with Windows, so
  background activity there can't be fully controlled.
- **Not measured:** NUMA effects (one socket here), huge pages (the table's
  dTLB misses suggest they would help; not tried), other compilers.
- Every number above traces to a file in [results/](results/), produced by
  `scripts/run_benchmarks.sh` at commit `9ca8c3e`.
