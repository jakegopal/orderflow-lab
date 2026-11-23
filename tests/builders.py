"""Hand-built feed records for tests: one place that knows the schema's shape."""

from orderflow.data.schema import BookMessage, BookUpdate, Side


def make(side: Side, price_ticks: int, size: float) -> BookUpdate:
    """A minimal BookUpdate; ts and seq are irrelevant to state logic."""
    return BookUpdate(ts=0, side=side, price_ticks=price_ticks, size=size, seq=0)


def msg(
    seq: int,
    *levels: tuple[Side, int, float],
    snap: bool = False,
    gap: bool = False,
    ts: int = 0,
) -> BookMessage:
    """A BookMessage holding one update per (side, price_ticks, size) level."""
    updates = tuple(
        BookUpdate(ts=ts, side=side, price_ticks=price, size=size, seq=seq, is_snapshot=snap)
        for side, price, size in levels
    )
    return BookMessage(seq=seq, ts=ts, is_snapshot=snap, gap_before=gap, updates=updates)
