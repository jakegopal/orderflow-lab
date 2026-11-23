"""Canonical record types that cross module boundaries.

Define the shapes every stage agrees on, so the loader can
change venue without anything downstream noticing.
"""

from dataclasses import dataclass
from typing import Literal

Side = Literal["bid", "ask"]
Direction = Literal["buy", "sell"]


@dataclass(frozen=True, slots=True)
class BookUpdate:
    """One L2 update from a venue feed.

    ts:    exchange timestamp, UTC nanoseconds since epoch.
    side:  which side of the book the level belongs to.
    price_ticks: price level as integer ticks (e.g. USD cents).
    size:  new resting size AT that level (absolute, not a delta); 0 removes it.
    seq:   sequence number of the message this update arrived in.
    """

    ts: int
    side: Side
    price_ticks: int      # price as integer ticks (USD cents); exact, hashable
    size: float
    seq: int
    is_snapshot: bool = False


@dataclass(frozen=True, slots=True)
class BookMessage:
    """One whole venue message: the only unit at which book state is real.

    A message carries many level changes, and the states partway through it never
    existed on the exchange, so replay applies and checks a message as a whole.

    seq:         the connection's sequence number for this message.
    ts:          when the venue sent the message, UTC nanoseconds since epoch. The
                 book state it produces is knowable no earlier than this.
    is_snapshot: the message is a complete book, so replay starts fresh on it.
    gap_before:  continuity with the previous message is not proven. A message was
                 skipped, repeated, or damaged; the connection restarted (seq reset);
                 or this is the first message in the stream. Any book carried
                 across this point is untrustworthy.
    updates:     every level change in the message, in venue order.
    """

    seq: int
    ts: int
    is_snapshot: bool
    gap_before: bool
    updates: tuple[BookUpdate, ...]
