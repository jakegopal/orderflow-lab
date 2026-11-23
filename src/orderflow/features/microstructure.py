"""Microstructure features, every one computable from information available at or
before its own timestamp.

Book state in, feature values out. The lookahead guard in
tests/test_features.py proves nothing here peeks forward.
"""

import math
from collections.abc import Iterable

import pandas as pd

from orderflow.book.state import BookState, TopOfBook
from orderflow.data.schema import BookMessage


def imbalance(bid_size: float, ask_size: float) -> float:
    """Top-of-book order-book imbalance in [-1, 1].

    (bid_size - ask_size) / (bid_size + ask_size). Positive means the bid side is
    heavier (buying pressure), negative means ask-heavier (selling pressure), zero
    means balanced. Returns 0.0 when both sides are empty rather than dividing by
    zero -- an empty top of book carries no directional signal.
    """
    total = bid_size + ask_size
    if total == 0.0:
        return 0.0
    return (bid_size - ask_size) / total


def weighted_mid(bid_px: float, bid_sz: float, ask_px: float, ask_sz: float) -> float:
    """(bid_px * ask_sz + ask_px * bid_sz) / (bid_sz + ask_sz).

    Each price is weighted by the OPPOSITE side's size, so heavy bid depth pulls
    the value up toward the ask.

    Both sizes zero raises ZeroDivisionError. That can't happen on a real top of
    book (a level of size 0 is removed), so it means the caller passed an empty side.
    """
    return (bid_px * ask_sz + ask_px * bid_sz) / (bid_sz + ask_sz)


def order_flow_imbalance(prev: TopOfBook, curr: TopOfBook) -> float:
    """Signed change in top-of-book depth between two states.

    Cont, Kukanov & Stoikov (2014). Measures the flow of events, not resting state.
    With P, q the best price and its size, and ' marking the previous state:

        e = q_b * [P_b >= P_b'] - q_b' * [P_b <= P_b']
            - q_a * [P_a <= P_a'] + q_a' * [P_a >= P_a']

    Bid side: if the bid price is unchanged, the term is the change in size there.
    If it rose, all of the new size is fresh demand. If it fell, the old size was
    consumed or pulled. The ask terms mirror this with the sign flipped, because
    supply arriving at the ask is selling pressure. Positive e is buying pressure.
    """
    return (
        curr.bid_sz * (curr.bid_px >= prev.bid_px)
        - prev.bid_sz * (curr.bid_px <= prev.bid_px)
        - curr.ask_sz * (curr.ask_px <= prev.ask_px)
        + prev.ask_sz * (curr.ask_px >= prev.ask_px)
    )


FEATURE_COLUMNS = (
    "segment", "seq", "ts",
    "bid_px", "bid_sz", "ask_px", "ask_sz", "mid", "spread",
    "imbalance", "ofi",
)


def build_features(replayed: Iterable[tuple[BookMessage, BookState, int]]) -> pd.DataFrame:
    """One row per message boundary in a valid segment: the feature frame.

    Takes replay's output, so each row reflects a whole message and a trustworthy
    book. Prices and spread are in ticks; mid is in ticks too, possibly half-tick.

    ofi compares each row with the previous row of the same segment. It is NaN on
    a segment's first row, because the previous state belongs to a different book
    and no feature crosses a segment boundary. A book with an empty side has no top
    of book, so it gets no row, and the next row's ofi is NaN too.

    The weighted-mid deviation (wmid - mid) / spread is deliberately not a
    column. At the top of book it equals imbalance / 2 exactly, so as a model
    input it would be perfectly collinear with imbalance.
    """
    rows = []
    prev: tuple[int, TopOfBook] | None = None   # (segment, top) of the previous row
    for message, book, segment in replayed:
        top = book.top_of_book()
        if top is None:
            prev = None
            continue
        ofi = (
            order_flow_imbalance(prev[1], top)
            if prev is not None and prev[0] == segment
            else math.nan
        )
        rows.append((
            segment, message.seq, message.ts,
            top.bid_px, top.bid_sz, top.ask_px, top.ask_sz,
            (top.bid_px + top.ask_px) / 2, top.ask_px - top.bid_px,
            imbalance(top.bid_sz, top.ask_sz), ofi,
        ))
        prev = (segment, top)
    return pd.DataFrame.from_records(rows, columns=FEATURE_COLUMNS)


def features_from_tops(tops: pd.DataFrame) -> pd.DataFrame:
    """The same feature columns as build_features, from a table of tops of book.

    For venues whose book is rebuilt outside Python (ITCH, in C++): tops has one
    row per message boundary with segment, ts, bid_px, bid_sz, ask_px, ask_sz,
    in time order, every row two-sided. A one-sided or untrusted state between
    two rows must start a new segment, because build_features resets OFI there
    too. Other columns (an event index, say) are
    kept. This is not the replay loop, which stays sequential: the book states
    already exist, so each feature is a column operation. OFI compares each row
    with the previous row of the same segment and is NaN on a segment's first
    row, exactly as in build_features, and a test holds the two equal.
    """
    out = tops.copy()
    out["mid"] = (out["bid_px"] + out["ask_px"]) / 2
    out["spread"] = out["ask_px"] - out["bid_px"]
    total = out["bid_sz"] + out["ask_sz"]
    out["imbalance"] = ((out["bid_sz"] - out["ask_sz"]) / total).where(total != 0, 0.0)
    prev = out.groupby("segment")[["bid_px", "bid_sz", "ask_px", "ask_sz"]].shift(1)
    out["ofi"] = (
        out["bid_sz"] * (out["bid_px"] >= prev["bid_px"])
        - prev["bid_sz"] * (out["bid_px"] <= prev["bid_px"])
        - out["ask_sz"] * (out["ask_px"] <= prev["ask_px"])
        + prev["ask_sz"] * (out["ask_px"] >= prev["ask_px"])
    ).where(prev["bid_px"].notna())
    return out
