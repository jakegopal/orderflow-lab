# 2. The L3 order book, and how we know it is right

Code: [book.hpp](../../cpp/include/orderflow/book.hpp),
[itch_replay.cpp](../../cpp/src/itch_replay.cpp).
Tests: [test_book.cpp](../../cpp/tests/test_book.cpp).

## What the book holds

Two structures, kept consistent by every message:

```
orders_   order ref -> { symbol, side, price, remaining shares }   (see 03-data-structures.md)
books_    symbol -> bids: price -> { total shares, order count }   (see 03-data-structures.md)
                    asks: price -> { total shares, order count }
```

The **order map** exists because later messages name only the reference. To
apply `E ref=5001 executed 30`, the book must find order 5001, learn it is a
buy of AAPL at $150.00, take 30 shares off it, and take 30 shares off AAPL's
$150.00 bid level.

The **price levels** are the aggregated view a trader looks at: "best bid is
$150.00 with 300 shares in 2 orders". The best bid and ask come from here.

## What each message does

| Message | Effect |
|---|---|
| A / F add | insert the order; add its shares at its price |
| E execute | take shares off the order and its level; at 0 the order leaves. **No delete follows.** |
| C execute with price | the same as E. The trade price can differ (crosses), but shares leave the level the order rests at. |
| X cancel | the same as E, for cancelled shares (a partial cancel) |
| D delete | remove the order and all its remaining shares |
| U replace | delete the old order, then add a new one with a new reference, price and size, and the same symbol and side. It goes to the back of the queue. |
| P trade | **nothing**: it executes a *hidden* order, which was never in the displayed book |

Each message is applied whole, inside one `on()` call. The book is only ever
observed between messages, the same invariant as Phase 1. A replace changes
two orders but is still one message.

## Anomalies are counted, not fatal

If the replay ever sees an execute for an unknown reference, a duplicate add,
or an execution larger than the order, the book counts it and carries on. Why
not crash on the first one? A whole-day run should report *every* problem,
with counts. One crash tells you there's a bug; a full tally tells you its
shape (one missed message type, or random corruption).

The expected count on a complete day is zero, and `itch_replay` exits non-zero
otherwise.

## How we know it's right: four independent checks

There is no published "correct book" to diff against, so the evidence comes
from properties a correct book must have. Results on the full day `01302019`,
368,366,634 messages:

1. **Zero anomalies across 363,118,215 book messages.** No unknown or
   duplicate references, and no over-execution. One mis-parsed field (say, the
   shares read from the wrong offset) or one mishandled message type breaks
   this within seconds of the open.

2. **Every execution happened at the top of the book.** NASDAQ matches by
   price first. A resting order hit by an `E` during continuous trading must
   be at the best price on its side. The replay checks this *before* applying
   each `E`: 7,998,757 of 7,998,757 regular-hours executions, plus every
   pre-market and after-hours one, were at the book's best. A book with stale
   levels (an order that should have been removed) would show a better phantom
   price above real executions. This is a semantic check, independent of how
   the parser or book are written.

3. **The day ends empty: 0 live orders at the close of messages.** Every one
   of the ~165M orders added during the day was removed by a delete, a full
   execution or a replace. A single lost message about an order would leave
   it alive at the end.

4. **All four implementations agree, message by message.** A hash of every
   symbol's best bid, best ask and sizes after every book message (a
   "fingerprint") is identical for map vs flat levels and for std vs
   open-addressing order maps.

Plus the unit level: random multi-symbol streams through the real parser,
checked after every message against a deliberately naive reference book that
recomputes each level by summing its orders.

## Locked and crossed books

A book is **locked** when best bid = best ask and **crossed** when best bid >
best ask. A matching engine executes crossing orders, so in continuous trading
NASDAQ's own book should almost never be in either state. Counted at message
boundaries:

| phase | locked | crossed |
|---|---|---|
| pre-market | 0 | 8 |
| regular (9:30–16:00) | 65 | 590 |
| after-hours | 0 | 0 |

That is 663 of 363M message boundaries, on only **4 symbols** all day.

### Where they happen: halts

The matching engine executes crossing orders only while a symbol is trading.
When NASDAQ halts a symbol (news pending, a limit-up/limit-down volatility
pause, an IPO's quotation-only period), it announces this with an `'H'`
(stock trading action) message, and orders that would cross simply rest until
trading resumes with an auction. So a correct book *may* lock or cross during
a halt and should not otherwise. The replay tracks each symbol's state from
its `'H'` messages and splits the count:

| symbol state at the time | locked or crossed |
|---|---|
| halted, paused or quotation-only | 600 |
| trading | 63 |

All 63 "trading" states fall **at most 0.264 ms after that symbol's resume**
message. The resume message comes first, then the reopening auction's
executions clear the cross a fraction of a millisecond later. Nothing locks
or crosses during ordinary continuous trading. This is a fifth check, and the
strongest one so far: a book with stale levels would cross at random symbols
and times, not only inside halts.

Examples: EYEN, crossed from 07:27 pre-market (bid $2.95, ask $2.90), and
DLPN at 09:46 (bid $1.65, ask $1.63). Each spans seconds, so neither can be
inside the 0.264 ms post-resume window: both are halted-state crosses.
