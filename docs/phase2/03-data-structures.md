# 3. The two hot structures, two designs each

Code: [price_levels.hpp](../../cpp/include/orderflow/price_levels.hpp),
[order_map.hpp](../../cpp/include/orderflow/order_map.hpp).
Tests: [test_price_levels.cpp](../../cpp/tests/test_price_levels.cpp),
[test_order_map.cpp](../../cpp/tests/test_order_map.cpp).
Benchmarks: [04-benchmarks.md](04-benchmarks.md).

The book does two lookups per message: find the order by reference, then find
its price level. Each lookup has a textbook design and a cache-friendly
design, and the book is a template over both choices (4 combinations).

## Why memory layout matters more than big-O here

A modern CPU core does several operations per nanosecond, but fetching a cache
line from main memory (DRAM) takes ~80–100 ns. Rough costs on this machine:

| where the data is | size here | latency |
|---|---|---|
| L1 cache | 48 KiB per core | ~1 ns |
| L2 cache | 1 MiB per core (6 MiB total) | ~3–4 ns |
| L3 cache | 16 MiB shared | ~10–15 ns |
| DRAM | 7.5 GiB | ~80–100 ns |

So the number that matters is **how many dependent cache misses an operation
makes**. "Dependent" means you can't start fetching the next address until the
previous fetch returns, which is what following a pointer does. An O(log n)
tree walk of 20 levels is 20 dependent fetches. An O(1) array index is one
fetch.

## Price levels: `std::map` vs a flat tick-indexed window

**`MapLevels`** is a `std::map<Price, Level>` sorted best-first, so `begin()`
is the best price. Insert, find and erase are O(log n). It's a red-black tree:
each node is a separate heap allocation holding pointers to its children.
Finding a price means walking from the root through nodes scattered in memory.

**`FlatLevels`** relies on an observation: US stock prices at $1 and above
move in one-cent ticks (Reg NMS Rule 612), and almost all activity sits within
a few dollars of the current price. So it uses a fixed array of N = 2,048 slots,
one per cent, starting at a base price:

```
slot index = (price - base) / 100          (prices are in units of $0.0001)

base = $139.76                                                  $160.23
  [0] [1] [2] ... [1024] ... [2047]
                    ^ around the current price
```

Add and reduce are an array write: one fetch, and neighbouring prices share
cache lines. Three extra mechanisms make it complete:

1. **Finding the best level quickly.** When the best level empties, the next
   best could be any distance away. A bitmap with one bit per slot (2,048 bits
   = 32 64-bit words) marks non-empty slots. The next best bid is the highest
   set bit below, found with `std::countl_zero` (one instruction, `lzcnt`) per
   word. That's at most 32 word tests, and usually one. The best slot is cached
   between changes, so `best()` is O(1).

2. **Overflow for prices the window can't hold.** Stub quotes ($0.01 bids and
   $199,999 asks that market makers post to satisfy quoting obligations),
   prices far from the market, and sub-penny prices (legal only below $1) go
   to a small `std::map`. Nothing is ever dropped. `best()` compares the window
   with the overflow explicitly, so correctness never depends on the window
   being well placed.

3. **Re-centering.** If the best price itself ends up in the overflow (the
   market moved more than ~$10 since the window was placed), the window is
   rebuilt around it: O(N + overflow), counted, and rare.

Memory is the price: 2,048 × 16 bytes = 32 KiB per side per symbol, allocated
on first use. For ~8,700 symbols that's up to ~560 MiB, versus a few KB for a
small map. The benchmarks report peak memory next to speed.

## Order map: `std::unordered_map` vs open addressing

**`StdOrderMap`** wraps `std::unordered_map`. The C++ standard promises that an
element never moves once inserted (pointers to it stay valid), which in
practice forces **separate chaining**: an array of buckets, each pointing to a
linked list of heap-allocated nodes. A lookup is: hash → bucket array → node
pointer → node. That's typically 2 dependent misses, plus a `malloc` per add and
a `free` per delete. With 165M adds a day, that's 165M allocations.

**`OpenAddressingMap`** keeps every `{key, value}` in one flat array:

- **Linear probing.** Hash to a slot. If it's taken by another key, try the
  next slot, and so on. Collisions stay in the same or the adjacent cache line.
- **At most half full.** Probe chains grow like 1/(1−load)², so at 50% they
  stay short. That costs memory: 2× the slots of a 100%-full table.
- **Fibonacci hashing.** ITCH references are nearly sequential (5001, 5002,
  5003…). Using their low bits as the slot index would pile neighbours into
  neighbouring slots and create long probe runs. Multiplying by 2⁶⁴/φ (the
  golden ratio) and keeping the top bits spreads every input bit across the
  index.
- **Backward-shift deletion.** Simply emptying a slot would break the probe
  chain for any key stored further along that probed *past* this slot.
  Tombstones ("deleted, keep probing") are the common fix, but they accumulate
  and lengthen every probe. Instead, after emptying a slot, walk the following
  run and pull back each key that's allowed to sit in the hole. The table ends
  up exactly as if the deleted key had never been inserted.
  [order_map.hpp](../../cpp/include/orderflow/order_map.hpp) has the one-line
  condition, and the differential test catches it if it's off by one (`>` for
  `>=`).
- **What it gives up:** stable addresses. A pointer from `find()` is valid only
  until the next insert or erase (a rehash or a backward shift can move the
  element). The book copies an order before erasing it for this reason.

## How the designs are tested against each other

Each fast design has a **differential test** against the simple one, over long
random operation sequences: `FlatLevels` vs `MapLevels` (20,000 operations per
seed, with drift, stubs and sub-penny prices, asserting that re-centering
actually happened), and `OpenAddressingMap` vs `std::unordered_map` (200,000
operations per seed on a small dense table). Planted bugs were confirmed to fail
them: recentering that forgets overflow levels, a wrong-direction bitmap scan,
and an off-by-one in backward shift.
