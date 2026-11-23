"""Drive BookState across a message stream, yielding only trustworthy books.

A book is trustworthy only inside a *valid segment*: a run that begins with a
snapshot (a complete book) and has had no break in the message sequence since.
Every snapshot replaces the book outright and opens a new segment. Coinbase
also re-sends full snapshots mid-connection, and merging one into the old
book would leave stale levels behind. A gap marks the segment invalid until the
next snapshot, because a delta feed cannot rebuild a complete book from a
mid-stream position, so a gapped book stays wrong forever.

The loader decides where gaps are (it is the only stage that sees every channel);
replay only acts on its gap_before flag.

Work happens per whole message. A message carries many level changes, and the
states partway through it never existed on the exchange. So the cross check and
the yield happen only after the whole message has been applied.

Path-dependent by nature -- message n needs the state from message n-1 -- which
is why this loop cannot be vectorised, and why the C++ book replaces it in Phase 2.
"""

from collections.abc import Iterable, Iterator

from orderflow.book.state import BookState
from orderflow.data.schema import BookMessage


class CrossedBookError(Exception):
    """Raised when best bid >= best ask -- the book has diverged from reality."""


def replay(
    messages: Iterable[BookMessage],
    *,
    check_crossed: bool = True,
) -> Iterator[tuple[BookMessage, BookState, int]]:
    """Yield (message, book, segment_id) after each message applied in a valid segment.

    segment_id increments on every snapshot, so downstream code can group by
    segment and never compute a feature across a discontinuity. The message comes
    along so callers know the seq and time of each state. Yields the same
    BookState object, mutated in place -- consume it, do not stash it.
    """
    book = BookState()
    segment_id = -1        # becomes 0 at the first snapshot
    valid = False          # True only inside a snapshot-anchored, gap-free run

    for message in messages:
        if message.is_snapshot:
            book = BookState()
            segment_id += 1
            valid = True
        elif message.gap_before:
            valid = False   # a gap: this segment can no longer be trusted

        if not valid:
            continue        # skip everything until the next snapshot rescues us

        for update in message.updates:
            book.apply(update)

        if check_crossed:
            bid, ask = book.best_bid(), book.best_ask()
            if bid is not None and ask is not None and bid >= ask:
                raise CrossedBookError(
                    f"seq {message.seq}: best bid {bid} >= best ask {ask}"
                )

        yield message, book, segment_id
