# 1. The SPSC ring buffer

Code: [spsc.hpp](../../cpp/include/orderflow/spsc.hpp).
Tests: [test_spsc.cpp](../../cpp/tests/test_spsc.cpp), which run under
ThreadSanitizer in CI.
Benchmark: [bench_spsc.cpp](../../cpp/bench/bench_spsc.cpp), results in
[03-results.md](03-results.md).

## Overview

A feed handler has two jobs: decode messages off the wire, and update the
book. Put them on two cores and you need a queue between them. A queue with
a lock makes each side wait for the other and pays for the lock on every
message. With exactly **one producer and one consumer**, no lock is needed:
each index has a single writer, so plain atomic loads and stores with the
right memory ordering are enough.

## The structure

```
slots:   [ 0 ][ 1 ][ 2 ][ 3 ][ 4 ][ 5 ][ 6 ][ 7 ]      capacity 8 (a power of two)
                    ^head = 10           ^tail = 13      (counters only ever grow)
                    10 & 7 = 2           13 & 7 = 5
empty:  head == tail          full:  tail - head == capacity
```

The counters never wrap back to zero. The slot is `counter & (capacity - 1)`,
one AND instruction, which is why the capacity must be a power of two. A
64-bit counter at a billion messages per second takes 584 years to overflow.

## Memory ordering

```
producer (push v)                          consumer (pop)
  t = tail.load(relaxed)                     h = head.load(relaxed)
  if t - head_cache >= N:                    if h == tail_cache:
      head_cache = head.load(acquire)  (B)       tail_cache = tail.load(acquire)  (A')
      if still full: return false                if still empty: return false
  slots[t & mask] = v                        v = slots[h & mask]
  tail.store(t + 1, release)           (A)   head.store(h + 1, release)           (B')
```

- **(A) → (A')**: release on the producer's `tail` store, acquire on the
  consumer's `tail` load. If the consumer reads the new tail, everything the
  producer wrote before the release, including the slot, is visible to it.
  Without this, a CPU or compiler could make the tail visible before the slot
  write, and the consumer would read garbage. x86 happens not to reorder
  stores with stores, but the compiler can, and ARM's CPUs can too.
- **(B') → (B)**: the mirror image. The producer must not overwrite a slot
  until the consumer has finished reading it.
- Each side reads its **own** counter relaxed: only it writes that counter.
- **Why not `seq_cst`?** Sequential consistency adds a single global order of
  all `seq_cst` operations. Nothing here needs one: each side only cares
  about the other side's single counter. On x86 a `seq_cst` store compiles to
  `xchg` (or `mov` + `mfence`), a full barrier on every push. Release is a
  plain `mov`.

## Cache lines and false sharing

A core writes memory in 64-byte cache lines. When the producer's core writes
`tail`, any other core's copy of that **line** is invalidated, even if the
other core only reads a *different* variable on it. If `head` and `tail`
shared a line, every push would knock the line out of the consumer's cache
and every pop would do the same to the producer. Neither shares data, but
both pay as if they did. That is false sharing. So each counter gets its own
line (`alignas(64)`).

### Cached counters

The producer only needs the consumer's `head` to know whether the ring is
full. It keeps its own copy, `head_cache`, and re-reads the shared `head`
only when the copy says "full". In steady state that is once per lap of the
ring, not once per message. The consumer does the same with `tail`. The
benchmark measures both choices by turning them off:

| variant | padding | cached counters |
|---|---|---|
| `spsc` | yes | yes |
| `spsc-unpadded` | no (one line for both) | yes |
| `spsc-nocache` | yes | no |
| `mutex` | the same ring, under `std::mutex` | — |

## How it is tested

- Single thread: empty, full, FIFO order, and wrap-around over several laps.
- Two threads, 2 million values: the consumer checks every value is the next
  integer, so a lost, duplicated or reordered message fails the test.
- The same tests under **ThreadSanitizer** (CI job `tsan`). TSan tracks
  happens-before. If the release/acquire pair were weakened to relaxed, it
  would report a data race on the slot even on x86, where the values usually
  come out right anyway. That is why a passing stress test alone proves
  little.
