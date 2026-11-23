# orderflow-lab

A C++ market-data and backtesting system with a Python research layer. It
parses a full day of NASDAQ TotalView-ITCH 5.0 (368 million messages) into an
order-level book, backtests against it with latency, fees and FIFO queue
position, and runs the research model inside the backtester.

The research question is whether a short-horizon order-flow signal survives
realistic execution. It doesn't. On Coinbase BTC-USD the signal earns about
+0.1 bps per round trip before costs, twelve standard errors from zero, and
loses about 3.9 bps after the cheapest taker fee. On NASDAQ the same features
don't cover the spread at any horizon from 1 to 60 seconds, and resting
orders don't fix that because they are adversely selected: a resting buy
fills when the price is falling through it.

## Layout

```
                  feed thread                       book thread
NASDAQ ITCH 5.0 ─► ITCH parser ──► [SPSC ring] ──► L3 order book ──► event-driven backtester
 (binary, L3)      itch.hpp         spsc.hpp       book.hpp           engine.hpp: latency, fees,
                                                                      executable fills, FIFO queue
                                                                      position (ITCH only)
                                                                          ▲            │
Coinbase level2 ─► Python loader ── pybind11 ─► L2 book                   │            ▼
 (JSONL, own       gap detection     _core                          ModelStrategy   trades,
  recordings)      over all channels                                (infer.hpp)     markouts
                                                                           ▲
Python: features → labels → logistic / MLP → purged walk-forward ──────────┘ (exported model)
```

| path | contents |
|---|---|
| `cpp/include/orderflow/` | parser, book and its data structures, backtester, SPSC ring, pool allocator, inference (header-only) |
| `cpp/src/`, `cpp/bench/` | whole-day tools (`itch_stats`, `itch_replay`, `itch_extract`) and benchmarks |
| `cpp/bindings/` | the `orderflow._core` Python module (pybind11) |
| `src/orderflow/` | Python research code: loader, replay, features, labels, models, validation, backtest bridge |
| `scripts/` | the Coinbase recorder and the scripts that produce every result below |
| `docs/` | design notes and raw results, by stage |

The project was built in five stages, which the docs and scripts refer to:
1, a Python research loop on Coinbase data; 2, the C++ parser, book and
bindings; 3, the backtester; 4, the ring buffer, pool allocator and threaded
replay; 5, C++ inference.

## Results

### The book (stage 2)

On 01302019 all four book variants (`std::map` or a flat price window, times
`std::unordered_map` or open addressing) give 0 anomalies over 363 M book
messages. All 8,096,995 E executions happen at the best price on their side,
no orders are left at the end of the day, and every variant produces the same
top-of-book fingerprint. All 663 locked or crossed books occur during halts or
within 0.264 ms of a resume. [docs/phase2/02-book.md](docs/phase2/02-book.md)

### Data structures (stage 2)

Whole-day replay from memory on one pinned core, two runs, ns per message:

| price levels | order map | ns/msg | cycles | L1d misses | IPC |
|---|---|---|---|---|---|
| std::map | std::unordered_map | 311 / 315 | 1,468 | 9.4 | 0.26 |
| std::map | open addressing | 221 / 241 | 1,030 | 7.0 | 0.28 |
| flat window | std::unordered_map | 249 / 246 | 1,033 | 5.9 | 0.31 |
| flat window | open addressing | 171 / 147 | 647 | 4.0 | 0.36 |

The two choices are roughly additive, and an IPC around 0.3 says the replay
waits on memory. flat-open per message: p50 180 ns, p99 912 ns.
[docs/phase2/04-benchmarks.md](docs/phase2/04-benchmarks.md)

### The signal under realistic execution (stages 1 and 3)

Coinbase BTC-USD, 13 captures, logistic regression on imbalance, OFI and
spread, five purged and embargoed walk-forward folds, 100-message horizon,
0.01 BTC:

| | gross bps per round trip | net, top-tier fee (2 bps) |
|---|---|---|
| stage 1 scoring | +0.120 ± 0.010 | −3.88 |
| engine, zero latency (12,584 trades, identical to stage 1) | same | same |
| engine, 10 ms each way (cloud) | +0.110 ± 0.010 | −3.89 |
| engine, 25 ms each way (home) | +0.095 ± 0.010 | −3.91 |
| buy on every row, 10 ms | −0.019 ± 0.011 | −4.02 |

NASDAQ, 01302019, each symbol retrained on its own data, 30-second horizon,
colocated latency, 100 shares, gross bps per round trip:

| | AAPL | MSFT | QQQ |
|---|---|---|---|
| taker, model | −1.29 ± 0.21 | −0.88 ± 0.15 | −0.41 ± 0.12 |
| taker, random side | −1.01 ± 0.21 | −0.95 ± 0.15 | −0.63 ± 0.12 |
| resting entry, model | −1.25 ± 0.24 | −0.65 ± 0.17 | −0.35 ± 0.13 |

Mid price after an AAPL entry fill, in bps in our favour: taker −0.72 at the
fill and −0.68 after 10 s; resting −0.13 at the fill and −0.61 after 10 s.
Every horizon from 1 to 60 s, three sizes and two latencies are in
[docs/phase3/02-why-the-alpha-died.md](docs/phase3/02-why-the-alpha-died.md).

### Ring buffer, pool, threads (stage 4)

| | throughput | round trip p50 / p99.9 |
|---|---|---|
| SPSC ring (padded, cached counters) | 420 M msg/s | 110 / 410 ns |
| counters sharing a cache line | 283 M msg/s | 150 / 460 ns |
| no cached counters | 247 M msg/s | 140 / 451 ns |
| `std::mutex` around the same ring | 14.4 M msg/s | 351 / 6,733 ns |

The pool allocator cuts `std::unordered_map` insert-plus-erase from 363 to
252 ns at 8 M live orders and leaves lookups unchanged. Splitting the replay
into a feed thread and a book thread is 1.94× faster on the 11 GB day, which
doesn't fit in RAM, and 20% slower once the data is cached.
[docs/phase4/03-results.md](docs/phase4/03-results.md)

### Inference (stage 5)

The exported model, evaluated inside the engine, gives scikit-learn's class
on every one of 631,515 test rows. Per decision:

| | logistic | MLP 32×32 |
|---|---|---|
| in the engine, scalar double | 40 ns | 812 ns |
| in the engine, AVX2 float | 40 ns | 130 ns |
| ONNX Runtime C API, from C++ | 1.6 µs | 3.5 µs |
| ONNX Runtime, from Python | 3.4 µs | 5.5 µs |
| scikit-learn, one row from Python | 566 µs | 652 µs |

Charging the measured p99 as decision latency doesn't change any backtest.
[docs/phase5/02-results.md](docs/phase5/02-results.md)

## Methodology

- Prices are executable: buys pay the asks they take and sells receive the
  bids. No trade is priced at the mid.
- A Coinbase book is trusted only from a snapshot with no missing message
  since. Gap detection counts every message on the connection, including
  non-book channels, and every snapshot replaces the book.
- Book state is used only at message boundaries. On Coinbase, checking for a
  crossed book after each update fails; after each message it never does.
- Validation is walk-forward with a purge and an embargo, and a test fails if
  leakage is introduced. Features and labels never cross a segment boundary.
- Label horizons on NASDAQ are a fixed clock grid (1 to 60 s) for every
  symbol; the headline horizon is chosen from label shares on the first
  training block only, before any edge is computed.
- Queue priority follows feed order. NASDAQ's order references aren't
  monotonic (813,636 regressions on the day), so they aren't used.
- Coinbase's 50 ms batches are treated as covering the interval before them,
  so an order is never filled against the batch its own signal came from.
- Each port is checked against what it replaces: the C++ L2 book against the
  Python book, the engine against stage 1's trades, C++ inference against
  scikit-learn, the threaded replay against the single-threaded one.
- Fees, latencies and sizes come with sources in
  [docs/phase3/03-settings.md](docs/phase3/03-settings.md).

## Limitations

- One NASDAQ day and three symbols; training on one day and testing on
  another hasn't been done.
- ITCH is NASDAQ's own book, not the consolidated market, and the day was the
  day after Apple's earnings.
- Simulated orders don't affect the replayed book, and hidden liquidity is
  ignored.
- Some settings are estimates: the 1 µs decision time, the home-connection
  latency, and Coinbase's post-September 2026 tiers, which are visible only
  after login.
- Measurements come from a laptop under WSL2. Runs vary by up to 15%, and by
  about 20% between sessions, so comparisons are made between runs done back
  to back. LLC miss counters aren't available on this kernel.

## Reproducing

```bash
# C++ build and tests; sanitizers; release build with tools and benchmarks
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
cmake --preset release && cmake --build --preset release
cmake --preset onnx && cmake --build --preset onnx        # optional: ONNX Runtime benchmark

# Python (pip builds the extension)
python3 -m venv .venv && source .venv/bin/activate
pip install -e ".[dev,record]" && ruff check . && mypy && pytest

# Data: Coinbase captures with scripts/record_book.py; one ITCH day (4.8 GB download)
scripts/fetch_itch.sh 01302019
for s in AAPL MSFT QQQ; do
  build/release/cpp/src/itch_extract data/itch/01302019.NASDAQ_ITCH50 data/itch/01302019.$s $s
done

# Results
build/release/cpp/src/itch_replay data/itch/01302019.NASDAQ_ITCH50 --variant flat-open
scripts/run_benchmarks.sh
build/release/cpp/bench/bench_spsc
build/release/cpp/bench/bench_pipeline --mode threaded data/itch/01302019.NASDAQ_ITCH50
python scripts/phase1.py
python scripts/phase3.py coinbase
for s in AAPL MSFT QQQ; do python scripts/phase3.py itch data/itch/01302019.$s $s; done
pip install -e ".[onnx]" && python scripts/phase5.py data/itch/01302019.AAPL AAPL
```

Developed on Ubuntu 26.04 under WSL2 with GCC 15.2 and Python 3.14. CI runs
the Python checks and C++ release, ASan/UBSan and TSan builds. Tests that need
recorded data are skipped when `data/` is absent.
