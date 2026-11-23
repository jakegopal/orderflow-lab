# 2. The pool allocator

Code: [pool.hpp](../../cpp/include/orderflow/pool.hpp).
Tests: [test_pool.cpp](../../cpp/tests/test_pool.cpp).
Benchmarks: `BM_OrderMap*<PooledStdOrderMap>` in
[bench_structures.cpp](../../cpp/bench/bench_structures.cpp), and the
`flat-pool` replay variant. Results in [03-results.md](03-results.md).

## Overview

`std::unordered_map` allocates one heap node per element. On the ITCH day
that is ~165 million orders, so 165 million `malloc`s on adds and as many
`free`s on deletes. A general-purpose `malloc` must serve any size from any
thread. It finds a size class, consults a per-thread cache, keeps block
metadata, and sometimes takes a lock. A pool that serves only a few fixed
sizes to a single thread can do each allocation in a handful of
instructions.

## How it works

```
free list for 48-byte blocks:   head -> [blk 7] -> [blk 3] -> [blk 9] -> null
allocate():   take head (blk 7), head = blk 7's next      (2 loads, 1 store)
deallocate(): blk->next = head, head = blk                (1 load, 2 stores)
```

- One free list per size class (multiples of 16 bytes up to 256).
- The "next" pointer lives *inside* the free block itself (an intrusive
  list), so a free block costs no extra memory and there is no header per
  block.
- When a list is empty, a 64 KiB chunk is carved into blocks and threaded
  onto it in address order, so a burst of allocations is contiguous in memory.
- Memory goes back to the system only when the pool is destroyed. That is
  fine for a book that lives all day, and it is the usual price of a pool.
- Not thread-safe. The book is owned by one thread, and a lock
  would cost more than the pool saves.

`PoolAllocator<T>` plugs the pool into the standard Allocator interface.
`PooledStdOrderMap` is therefore *the same* `std::unordered_map` (same hash,
same chaining, same node layout) with only the allocator changed. Comparing
it with `StdOrderMap` separates "what malloc costs" from "what pointer-chasing
through nodes costs". Open addressing (Phase 2) removes both.

## The question this answers

Phase 2 found open addressing ~1.4× faster than `std::unordered_map` in the
whole-day replay. How much of that gap is the allocator, and how much the
node layout? If the pooled map closes most of the gap, the allocator was the
problem. If not, the layout's dependent cache misses were. The measured
answer is in [03-results.md](03-results.md).
