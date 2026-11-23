"""The running order-book state that replay maintains.

Hold resting depth per price level and apply one update
at a time. Prices are integer ticks (exact, hashable); sizes are float.

This stores levels in plain dicts, price_ticks -> size. Updating a level is O(1),
but finding the best price is an O(n) scan over every level. That scan is slow
on purpose: it is the bottleneck the Phase 2 C++ book benchmarks against. The dict
is a deliberate, replaceable choice, not the final design.
"""

from dataclasses import dataclass, field

from orderflow.data.schema import BookUpdate


@dataclass(frozen=True, slots=True)
class TopOfBook:
    """Best bid and best ask with their resting sizes, at one message boundary.

    A small immutable value. Replay mutates one BookState in place, so a feature
    that compares two moments in time (like OFI) has to keep a copy of the top
    of book rather than a reference to the book.
    """

    bid_px: int        # ticks
    bid_sz: float
    ask_px: int        # ticks
    ask_sz: float


@dataclass
class BookState:
    """Resting depth keyed by integer-tick price, one dict per side."""

    bids: dict[int, float] = field(default_factory=dict)
    asks: dict[int, float] = field(default_factory=dict)

    def apply(self, update: BookUpdate) -> None:
        """Apply one update in place. A size of 0 removes the level entirely."""
        book = self.bids if update.side == "bid" else self.asks
        if update.size == 0.0:
            book.pop(update.price_ticks, None)   # remove; no error if absent
        else:
            book[update.price_ticks] = update.size

    def best_bid(self) -> int | None:
        """Highest bid price in ticks, or None if the bid side is empty."""
        return max(self.bids) if self.bids else None

    def best_ask(self) -> int | None:
        """Lowest ask price in ticks, or None if the ask side is empty."""
        return min(self.asks) if self.asks else None

    def top_of_book(self) -> TopOfBook | None:
        """The current best bid and ask with sizes, or None if either side is empty."""
        bid, ask = self.best_bid(), self.best_ask()
        if bid is None or ask is None:
            return None
        return TopOfBook(bid_px=bid, bid_sz=self.bids[bid], ask_px=ask, ask_sz=self.asks[ask])
