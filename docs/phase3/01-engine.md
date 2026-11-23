# The event-driven backtester

Code: [engine.hpp](../../cpp/include/orderflow/backtest/engine.hpp),
[markets.hpp](../../cpp/include/orderflow/backtest/markets.hpp),
[round_trip.hpp](../../cpp/include/orderflow/backtest/round_trip.hpp),
[types.hpp](../../cpp/include/orderflow/backtest/types.hpp).
Tests: [test_backtest.cpp](../../cpp/tests/test_backtest.cpp),
[test_engine.py](../../tests/test_engine.py).

The Phase 1 scoring bought at the ask one message after a signal and sold at
the bid a fixed number of messages later. That gets prices right but treats
time loosely, and it can't say anything about orders that rest in the book.
The engine replaces it with a simulation on a single clock, where each event
happens at a definite nanosecond and events are processed in time order.

## Events and latency

There are four kinds of event: a market message applied at the exchange, our
order or cancel arriving at the exchange, a fill or order-done report reaching
the strategy, and a strategy timer. Market messages come from a stream; the
rest wait in a priority queue keyed by (time, insertion order). The loop
always takes whichever is earliest. When an order and a market message have
the same timestamp, the market message goes first, so an order never gets to
act inside the nanosecond in which the market changed.

Three latencies are charged on every decision:

```
message m applied at the exchange               t
strategy sees m                                 t + market_data
strategy's order arrives at the exchange        t + market_data + decision + order_entry
strategy learns of the fill                     fill time + market_data
```

The order is matched against the book as it is when it arrives, which can be
several messages after m. For that reason the strategy receives a copy of the
top five levels taken right after m (`BookSnapshot`) rather than a reference
to the live book, which would already show later messages by the time the
strategy runs.

## Taking liquidity

A market buy walks the ask side from the best level down, filling at each
level's price until it is done or the book runs out; the remainder of a
market order expires. A limit order does the same up to its limit and rests
with whatever is left. Fills are always at displayed prices, never at the mid.

## Resting orders and queue position (ITCH)

NASDAQ fills orders at a price in time priority. A resting order of ours
fills only once every displayed order ahead of it at that price has traded or
been cancelled. ITCH identifies every order, so this can be tracked:

- When our order arrives, all shares displayed at its price are ahead of it.
  The market numbers every order in the order it appears in the feed, and we
  record the next number (`boundary`). A replace (`U`) gets a new number
  because the exchange sends it to the back of the queue.
- An execution or cancel of an order with a number below `boundary` at our
  price moves us up.
- An execution of an order with a number at or above `boundary` at our price
  means the aggressor reached the orders behind us, so we would have traded
  first: we fill.
- An execution at a worse price on our side means the aggressor went through
  our price: we fill.
- If the other side's displayed price reaches ours, that order would have
  traded with us. Only newly arrived crossing quantity counts, because the
  real order stays on the book after our simulated fill.

`behind_while_ahead` counts executions of orders behind us while orders
ahead were still resting, which strict FIFO rules out. It stays in the tens
per day and is reported with every run.

### Why feed order and not order reference numbers

The first version compared NASDAQ's order reference numbers, assuming they
increase through the day. Before relying on that I counted references lower
than an earlier one on 01302019:

| scope | new references | regressions | when |
|---|---|---|---|
| whole feed, one counter | 191.9 M | 145.0 M | throughout |
| per symbol, all symbols | 191.9 M | 813,636 on 8,668 symbols | mostly just after the open; the last at 16:00:00.3 |
| AAPL | 875,938 | 37,981 | 9:30:00.08 to 9:30:31.7 |
| MSFT | 867,658 | 19,799 | before 9:30:31.2 |
| QQQ | 1,812,940 | 6,502 | before 9:30:30.4 |

Orders re-entered at the open reuse older references, so references can't
be used for priority. Feed order doesn't depend on them. The remaining risk
is an order that appears after ours but holds older priority; the market
records the time of a symbol's last reference regression, and `run_itch`
refuses a run whose session starts before it. For these three symbols the
last regression is at 9:30:32 and decisions start at 9:35.

### What the queue model leaves out

- Hidden orders. They appear only as `P` trades and can't jump displayed
  orders at the same price, so ignoring them can only make our fills
  slightly late, not early.
- Our own effect on the book. A simulated order never changes the replay.
  That is reasonable only when it is small next to displayed depth. On
  01302019 the median best level held about 300 shares for AAPL, 540-600 for
  MSFT and 840-900 for QQQ, and AAPL's best level was 100 shares or fewer on
  about a quarter of messages. Results are reported for 100, 200 and 500
  shares with the fraction of decisions where the order exceeds the best level.

## Coinbase: batches and segments

Coinbase publishes level2 updates in batches roughly 50 ms apart, and the
real book moves between batches. Row m of the feature table is therefore
treated as the book for the whole interval since the previous row (in the
same segment): an order that arrives between two batches is matched against
the later one. Matching it against the earlier batch would usually mean the
very batch its signal was computed from. An order arriving exactly at a
batch's timestamp still meets that batch, so zero latency reproduces the
Phase 1 scoring.

Only the best level is known from the feature table, so a taker order larger
than the best level fills partly. A segment ends at a sequence gap or a
reconnect; an order that arrives between segments is rejected rather than
filled at a stale price. On ITCH, orders that arrive while the symbol is
halted are rejected.

## Checks

- Parity with Phase 1: with zero latency, an event-count hold and an order
  that always fits the best level, the engine reproduces Phase 1's round trips
  exactly on the real captures (12,584 trades) and on random data in
  `test_engine.py`.
- Each rule above has a C++ test built from hand-written ITCH messages fed
  through the real parser. Breaking a rule on purpose (tie order, the clock
  update, the crossing count, the queue boundary, the trade-through check,
  rejecting during halts) makes the corresponding test fail.
