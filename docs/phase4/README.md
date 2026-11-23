# Phase 4: low-latency building blocks

1. [The SPSC ring buffer](01-spsc.md): memory ordering, false sharing,
   cached counters, and how ThreadSanitizer checks them.
2. [The pool allocator](02-pool.md): intrusive free lists, and the question
   it answers about `std::unordered_map`.
3. [Results](03-results.md): the ring against its ablations and a mutex, the
   pool, and whether a feed thread helps (on a cold day yes, warm no).

Raw output in [results/](results/).
