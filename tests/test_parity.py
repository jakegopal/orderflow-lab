"""Parity: the C++ L2 book must equal the Python BookState, the test oracle.

BookState is simple enough to check by reading it. The C++ book is the fast
one. If they ever disagree on the same input, the C++ book is wrong. Sizes are
compared exactly (==), not approximately: both books only store the floats
they are given, so any difference at all is a bug, not rounding.
"""

import random
import warnings
from pathlib import Path

import numpy as np
import pytest

from orderflow._core import L2Book
from orderflow.book.replay import replay
from orderflow.book.state import BookState
from orderflow.data.loader import load_messages
from orderflow.data.schema import BookMessage, BookUpdate, Side


def python_depth(book: BookState) -> tuple[list[tuple[int, float]], list[tuple[int, float]]]:
    """Both sides as (price, size) lists, best first, the C++ book's format."""
    return sorted(book.bids.items(), reverse=True), sorted(book.asks.items())


def assert_same_top(py: BookState, cpp: L2Book) -> None:
    bid, ask = py.best_bid(), py.best_ask()
    assert cpp.best_bid() == (None if bid is None else (bid, py.bids[bid]))
    assert cpp.best_ask() == (None if ask is None else (ask, py.asks[ask]))


def apply_message(cpp: L2Book, message: BookMessage) -> None:
    """One boundary crossing per message, as a C++ replay client would do it."""
    cpp.apply_many(
        np.array([u.side == "bid" for u in message.updates], dtype=np.bool_),
        np.array([u.price_ticks for u in message.updates], dtype=np.int64),
        np.array([u.size for u in message.updates], dtype=np.float64),
    )


def test_random_updates_agree_after_every_update() -> None:
    rng = random.Random(7)
    py, cpp = BookState(), L2Book()
    for step in range(20_000):
        side: Side = "bid" if rng.random() < 0.5 else "ask"
        centre = 7_700_000 if side == "bid" else 7_700_100
        price = centre + rng.randint(-50, 50) * (1 if side == "ask" else -1)
        size = 0.0 if rng.random() < 0.3 else round(rng.uniform(0.0001, 3.0), 8)
        py.apply(BookUpdate(ts=step, side=side, price_ticks=price, size=size, seq=step))
        cpp.apply(side, price, size)
        assert_same_top(py, cpp)
    assert (cpp.bids(), cpp.asks()) == python_depth(py)


def test_apply_many_rejects_mismatched_lengths() -> None:
    with pytest.raises(ValueError, match="same length"):
        L2Book().apply_many(
            np.array([True]), np.array([1, 2], dtype=np.int64), np.array([1.0])
        )


def test_apply_rejects_unknown_side() -> None:
    with pytest.raises(ValueError, match="'bid' or 'ask'"):
        L2Book().apply("buy", 1, 1.0)  # type: ignore[arg-type]


CAPTURE = Path("data/coinbase_BTC-USD_2026-08-21.jsonl")


@pytest.mark.skipif(not CAPTURE.exists(), reason="capture file not present")
def test_whole_capture_agrees_after_every_message() -> None:
    """Every message of a real capture: top of book after each, full depth often.

    Snapshots clear the C++ book exactly where replay starts a fresh BookState,
    so both books see identical input. Covers all 4 segments of the file, the
    ~43k-level opening snapshots and the live updates after them.
    """
    cpp = L2Book()
    messages = checked_depth = 0
    segment = -1
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")  # the file's damaged last line
        for message, py, seg in replay(load_messages(CAPTURE)):
            if seg != segment:
                cpp.clear()
                segment = seg
            apply_message(cpp, message)
            assert_same_top(py, cpp)
            messages += 1
            if messages % 2_000 == 0:
                assert (cpp.bids(), cpp.asks()) == python_depth(py)
                checked_depth += 1
    assert (cpp.bids(), cpp.asks()) == python_depth(py)
    assert messages == 53_474   # every message, as test_replay establishes
    assert segment == 3
    assert checked_depth == 26  # the depth comparison really ran
