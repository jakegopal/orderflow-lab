# Phase 3: the C++ backtester

1. [The event-driven engine](01-engine.md): one clock, latency, executable
   fills, FIFO queue position, Coinbase batches.
2. [Results](02-why-the-alpha-died.md): latency, size, fees, NASDAQ's
   spread, and adverse selection, with markouts.
3. [Settings](03-settings.md): fees, latencies, label horizons and order
   sizes, with sources.

Raw output in [results/](results/). Reproduce with `python scripts/phase3.py coinbase`
and `python scripts/phase3.py itch data/itch/01302019.AAPL AAPL`.
