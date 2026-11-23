# 1. NASDAQ ITCH 5.0, and how the parser reads it

Code: [itch.hpp](../../cpp/include/orderflow/itch.hpp),
[endian.hpp](../../cpp/include/orderflow/endian.hpp),
[itch_stats.cpp](../../cpp/src/itch_stats.cpp),
[itch_count.py](../../scripts/itch_count.py).
Tests: [test_itch.cpp](../../cpp/tests/test_itch.cpp),
[test_endian.cpp](../../cpp/tests/test_endian.cpp).

## Overview

An exchange's matching engine holds a list of resting buy and sell orders for
every stock. NASDAQ publishes every change to those lists as a stream of small
binary messages, called TotalView-ITCH. "An order was added", "an order was
executed for 100 shares", "an order was cancelled". If you start from an empty
book at the beginning of the day and apply every message in order, you have
NASDAQ's book at every moment of the day.

## L2 versus L3, concretely

Coinbase (Phase 1) sends **L2**: "price $77,001.00 now has 1.25 BTC in total."
You learn the total at a price, but not how many orders make it up or who is
first in line.

ITCH is **L3**, order by order:

```
A  ref=5001  BUY 100 AAPL @ 150.00     an order arrives
A  ref=5002  BUY 200 AAPL @ 150.00     another, behind it at the same price
E  ref=5001  executed 30               the first order trades 30 shares
X  ref=5002  cancelled 50              the second shrinks to 150
D  ref=5001                            the first order's remaining 70 leave
```

The later messages name only `ref`, not the symbol, side or price. So the book
has to remember every live order to know what `E ref=5001` refers to. That is
the order map in the book ([02-book.md](02-book.md)). It is also why one missed message is fatal:
every later message about that order becomes unknowable.

## Framing: how messages sit in the file

The file is the messages back to back, each preceded by its length as a 2-byte
big-endian integer:

```
00 0C | 53 00 00 00 00 0A 2D 3B 1F 8C 40 4F       12-byte 'S' (system event)
00 24 | 41 ...                                    36-byte 'A' (add order)
```

`FrameReader` walks this. It is deliberately defensive. If fewer bytes remain
than a length says, the file is truncated, and it reports that instead of
reading past the end.

## The header and big-endian numbers

Every message starts with 11 bytes: type (1), stock locate (2), tracking
number (2), timestamp (6). Fields are **big-endian**, most significant byte
first. The bytes `00 00 01 2C` mean 1×256 + 44 = 300. An x86 CPU stores
integers the other way round, so reading those 4 bytes straight into a
`uint32_t` gives 738,263,040.

`load_be` assembles the integer one byte at a time. Why not cast the pointer?
- The fields are **misaligned** (a 4-byte price at offset 32 of a message that
  starts anywhere). Dereferencing a misaligned `uint32_t*` is undefined
  behaviour in C++.
- It also breaks **strict aliasing**.

The byte loop is well-defined, and the compiler turns it into one load plus one
`bswap` instruction at `-O2`, so safety costs nothing.

The timestamp is 6 bytes, nanoseconds since midnight. 24 h = 86.4 × 10¹² ns,
which needs 47 bits. NASDAQ saves 2 bytes per message by not sending 8.

**Stock locate** is a small integer (1 to about 8,700) that the day's `R`
(stock directory) messages bind to symbols. The book indexes its per-symbol
books by it directly: an array index, not a string lookup.

**Prices** are 4-byte integers with 4 implied decimals: `1500000` is $150.0000.
They stay integers everywhere. That's the same reason the Coinbase loader uses integer ticks:
exact comparison, and no 0.1 + 0.2 ≠ 0.3 surprises in map keys.

## Dispatch: compile-time, not virtual

`parse(msg, handler)` decodes the message into a struct (`AddOrder`,
`OrderExecuted`, …) and calls `handler.on(struct)`. The handler is a template
parameter, so the call is resolved at compile time and can be inlined. If the
handler has no `on()` for a type, a C++20 `requires` check makes that call
disappear entirely. A counting tool and the order book use the same parser
with different handlers, and neither pays for what it ignores.

The alternative is a `virtual on(const AddOrder&)` interface. It costs an
indirect call per message and blocks inlining. At 368M messages that is
measurable, and it buys nothing here, because the handler type is known at
build time.

## Validation before trust

The parser checks every message's length against the spec table before
reading a single field. A wrong length means the framing is off. Reading
fields from it would give garbage that looks plausible, which is the most
dangerous kind.

On the full day `01302019` (11.2 GB uncompressed; gzip's CRC32 verified during
decompression):

| | |
|---|---|
| messages | **368,366,634** |
| malformed | 0 |
| truncated | no |
| A / F (adds) | 162,970,455 / 1,725,898 |
| D (deletes) | 158,273,361 |
| U (replaces) | 27,222,746 |
| E / C (executions) | 8,096,995 / 158,886 |
| X (cancels) | 4,669,874 |
| P (hidden trades) | 1,326,184 |

The counts were checked two ways:
1. **Spec-offset tests.** The test encoder writes each field at the byte
   offset the spec gives, independently of the parser. A parser reading a
   field from the wrong offset fails.
2. **A second implementation.** [itch_count.py](../../scripts/itch_count.py)
   is a separate framing counter in Python's standard library, with its own
   copy of the length table and no shared code. Its per-type counts equal the
   C++ counts exactly.

Both implementations are mine, so they could share a misreading of the
spec. A comparison against an independent published parser (MeatPy, for
example) is still to do.
