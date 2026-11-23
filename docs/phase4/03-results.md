# 3. Results: the ring, the pool, and two threads

Raw output: [results/](results/). Machine as in
docs/phase2/04-benchmarks.md: Ryzen AI 5 340 (6 cores, 2 threads each),
WSL2, GCC 15.2 `-O3`.

## The SPSC ring

`bench_spsc`: producer on CPU 2, consumer on CPU 4, which are different
physical cores. Throughput moves 100 M 8-byte messages, and the consumer
checks every value is the next integer. Latency is a ping-pong round trip,
two one-way hand-offs. Median of 5 runs (min, max in the raw file).

| variant | throughput | round trip p50 | p99 | p99.9 |
|---|---|---|---|---|
| `spsc` (padded, cached counters) | **420 M msg/s** | **110 ns** | 160 ns | 410 ns |
| `spsc-unpadded` (counters share a line) | 283 M msg/s | 150 ns | 260 ns | 460 ns |
| `spsc-nocache` (padded, no cached counters) | 247 M msg/s | 140 ns | 210 ns | 451 ns |
| `mutex` (same ring under `std::mutex`) | 14.4 M msg/s | 351 ns | 741 ns | 6,733 ns |

- **The lock-free ring is 29× the mutex's throughput** and a third of its
  median round trip. The mutex's p99.9 is 6.7 µs: when the lock is
  contended, the losing thread can be descheduled.
- **False sharing costs a third of the throughput** (420 → 283). Putting the
  two counters on one cache line makes every push and every pop invalidate
  the other core's copy of that line.
- **Cached counters matter as much as padding** (420 → 247). Without them,
  every push reads the consumer's line and every pop reads the producer's.
  That is one cross-core cache transfer per message instead of one per lap.
- Each of the two techniques costs about 40% when removed, and the
  benchmark shows it, rather than the README asserting it.

On SMT siblings (CPUs 2 and 3, one core, a shared L1 and L2), `spsc` reaches
484 M msg/s with a 100 ns round trip and a much tighter p99.9 (160 ns): cache
lines move within one core. That is why a production feed handler pins
cooperating threads deliberately.

## The pool allocator

Same `std::unordered_map`, nodes from `NodePool` instead of `operator new`
(`bench_structures`, medians of 3):

| live orders | lookup, std / pooled | churn (insert + erase), std / pooled / open addressing |
|---|---|---|
| 16 K | 6.9 / 6.5 ns | 42 / 31 / 24 ns |
| 256 K | 16.7 / 11.4 ns | 168 / 90 / 56 ns |
| 8 M | 40.2 / 39.8 ns | 363 / 252 / 135 ns |

- **Churn gets faster, lookups don't.** At 8 M live orders the pool cuts an
  insert-plus-erase from 363 to 252 ns. That is about half of the gap to open
  addressing (135 ns), and it is the cost of `malloc`/`free`. Lookups are
  unchanged at large sizes (40 ns): the pool doesn't change the node layout,
  so the dependent misses of chaining remain. At 256 K, pooled nodes sit
  closer together than malloc's and lookups get faster (16.7 → 11.4 ns).
- **Whole day** (`pool_replay.txt`, the two run back to back): flat-std
  204.0 ns/msg, flat-pool 189.9 ns/msg, **7% faster**, with slightly less
  peak memory (684 vs 710 MiB).
- **The answer to the doc 2 question:** the allocator explains about half
  of std's churn penalty, and the node layout explains the rest and all of
  the lookup penalty. Open addressing removes both, which is why it stays
  the default.

Caution on comparing across sessions: flat-std ran at 246–249 ns/msg in the
Phase 2 session and 204 ns/msg in this one. Machine-level drift on WSL2 is
~20% between sessions, so every comparison here is between runs made back
to back.

## Feed thread → book thread

`bench_pipeline`. In single mode one thread frames, parses and applies. In
threaded mode the feed thread (CPU 2) frames and parses, then pushes each
decoded book message through a 16 K-slot SPSC ring to the book thread
(CPU 4). Both modes must end with the same book fingerprint and 0 anomalies,
and they did.

| | single | threaded | |
|---|---|---|---|
| whole day, end to end (11 GB > RAM, page faults included) | 115.6 s, 3.19 M msg/s | **59.7 s, 6.17 M msg/s** | 1.94× faster |
| first 2 GB, warm (second run of each) | **7.0 s, 9.94 M msg/s** | 8.8 s, 7.90 M msg/s | 20% slower |

- When the input must come off disk, the feed thread takes the page faults
  (it touches each message first) while the book thread works on messages
  already in memory, so I/O overlaps compute: **1.94×**.
- When everything is warm, the work is CPU-bound, and **threading makes it
  slower.** Parsing is cheap: `itch_stats` frames and parses a warm file at
  133 M msg/s, ~7.5 ns per message. Moving its result through the ring costs
  a 64-byte copy into the slot (the decoded `std::variant` fills a whole
  cache line), that line moving to the other core, and a `std::visit` on the
  far side. That costs
  more than the parse it takes off the book thread. Pipelining helps when
  the stage you move off the critical path is expensive, and parsing isn't.
- What would help the warm case instead: batch several messages per slot
  (fewer cross-core transfers), or prefetch the next message's order slot
  on the book thread. Neither was tried.
