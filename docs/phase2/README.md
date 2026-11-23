# Phase 2: the C++ core

Design notes for the parser, the book, its two data structures, the
benchmarks and the Python bindings. Each note links the code it describes and
says how its claims were checked.

1. [ITCH 5.0 and the parser](01-itch.md): the protocol from zero, framing,
   big-endian fields, compile-time dispatch, validation of 368M messages.
2. [The L3 book](02-book.md): what each message does to the book, and the four
   whole-day checks that show the book is right.
3. [The two hot structures](03-data-structures.md): price levels (`std::map`
   vs a flat window) and the order map (`std::unordered_map` vs open
   addressing), and why cache misses matter more than big-O.
4. [Benchmarks](04-benchmarks.md): method, results, and what they mean.
5. [Python bindings and parity](05-bindings.md): pybind11, the parity test
   against `BookState`, and the measured cost of crossing the language
   boundary.

## Commands

```bash
# C++: configure, build, test (Debug, with -Werror)
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
# Under AddressSanitizer + UndefinedBehaviorSanitizer
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
# Release build with tools and benchmarks
cmake --preset release && cmake --build --preset release

# Whole-day checks (the file is ~11 GB uncompressed; see scripts/fetch_itch.sh)
build/release/cpp/src/itch_stats  data/itch/01302019.NASDAQ_ITCH50
build/release/cpp/src/itch_replay data/itch/01302019.NASDAQ_ITCH50 --variant flat-open
taskset -c 0 build/release/cpp/bench/bench_replay --latency data/itch/01302019.NASDAQ_ITCH50
taskset -c 0 build/release/cpp/bench/bench_structures

# Python side (pip compiles the extension)
pip install -e ".[dev,record]" && ruff check . && mypy && pytest
```
