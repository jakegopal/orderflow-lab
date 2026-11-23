"""Tests for microstructure features."""

import math
import random
from itertools import islice
from pathlib import Path

import pandas as pd
import pytest

from builders import make, msg
from orderflow.book.replay import replay
from orderflow.book.state import BookState, TopOfBook
from orderflow.data.loader import load_messages
from orderflow.data.schema import Side
from orderflow.features.microstructure import (
    FEATURE_COLUMNS,
    build_features,
    features_from_tops,
    imbalance,
    order_flow_imbalance,
    weighted_mid,
)


def test_imbalance_balanced() -> None:
    assert imbalance(1.0, 1.0) == 0.0


def test_imbalance_all_bid() -> None:
    assert imbalance(2.0, 0.0) == 1.0        # only bids -> +1


def test_imbalance_all_ask() -> None:
    assert imbalance(0.0, 2.0) == -1.0       # only asks -> -1


def test_imbalance_bid_heavy() -> None:
    # bid 2.1, ask 0.3 -> 1.8 / 2.4 = 0.75
    assert imbalance(2.1, 0.3) == 0.75


def test_imbalance_empty_is_zero() -> None:
    assert imbalance(0.0, 0.0) == 0.0        # no divide-by-zero


def test_imbalance_from_book() -> None:
    """The feature reads correctly off a reconstructed book."""
    book = BookState()
    book.apply(make("bid", 76290_00, 2.1))
    book.apply(make("ask", 76291_00, 0.3))
    top = book.top_of_book()
    assert top is not None
    result = imbalance(top.bid_sz, top.ask_sz)
    assert result == 0.75


def test_weighted_mid_balanced_is_mid() -> None:
    assert weighted_mid(100.0, 1.0, 102.0, 1.0) == 101.0


def test_weighted_mid_leans_toward_the_thin_side() -> None:
    # Bid 3, ask 1: (100*1 + 102*3) / 4 = 101.5. Heavy bids pull it toward the ask.
    assert weighted_mid(100.0, 3.0, 102.0, 1.0) == 101.5
    # Mirror image: heavy asks pull it toward the bid.
    assert weighted_mid(100.0, 1.0, 102.0, 3.0) == 100.5


def test_weighted_mid_stays_inside_the_spread() -> None:
    for bid_sz, ask_sz in [(0.001, 50.0), (50.0, 0.001), (2.5, 7.25)]:
        assert 100.0 < weighted_mid(100.0, bid_sz, 102.0, ask_sz) < 102.0


PREV = TopOfBook(bid_px=10000, bid_sz=1.0, ask_px=10002, ask_sz=1.0)


def top(bid: tuple[int, float], ask: tuple[int, float]) -> TopOfBook:
    return TopOfBook(bid_px=bid[0], bid_sz=bid[1], ask_px=ask[0], ask_sz=ask[1])


def test_ofi_unchanged_book_is_zero() -> None:
    assert order_flow_imbalance(PREV, top((10000, 1.0), (10002, 1.0))) == 0.0


def test_ofi_bid_size_grows_at_same_price() -> None:
    # Same price: the bid term is the size change, 3 - 1 = +2.
    assert order_flow_imbalance(PREV, top((10000, 3.0), (10002, 1.0))) == 2.0


def test_ofi_bid_price_rises() -> None:
    # A better bid: all of its size (2) is new demand; the old bid term drops out.
    assert order_flow_imbalance(PREV, top((10001, 2.0), (10002, 1.0))) == 2.0


def test_ofi_bid_price_falls() -> None:
    # The old best bid (size 1) was consumed or pulled: -1, whatever rests below.
    assert order_flow_imbalance(PREV, top((9999, 4.0), (10002, 1.0))) == -1.0


def test_ofi_ask_size_grows_at_same_price() -> None:
    # More supply at the same ask is selling pressure: -(3 - 1) = -2.
    assert order_flow_imbalance(PREV, top((10000, 1.0), (10002, 3.0))) == -2.0


def test_ofi_ask_price_falls() -> None:
    # A lower ask: all of its size (2) is new supply.
    assert order_flow_imbalance(PREV, top((10000, 1.0), (10001, 2.0))) == -2.0


def test_ofi_ask_price_rises() -> None:
    # The old best ask (size 1) was lifted or pulled: buying pressure, +1.
    assert order_flow_imbalance(PREV, top((10000, 1.0), (10003, 5.0))) == 1.0



def test_weighted_mid_deviation_is_half_the_imbalance() -> None:
    """Why build_features has no (wmid - mid) / spread column.

    Algebra: wmid - mid = (ask - bid)(q_b - q_a) / (2(q_b + q_a)), and dividing by
    the spread (ask - bid) leaves imbalance / 2. A model given both columns sees one
    feature twice, which makes the coefficients unidentifiable.
    """
    for bid, bid_sz, ask, ask_sz in [(100, 3.0, 102, 1.0), (7629000, 0.02, 7629013, 4.7)]:
        mid, spread = (bid + ask) / 2, ask - bid
        deviation = (weighted_mid(bid, bid_sz, ask, ask_sz) - mid) / spread
        assert deviation == pytest.approx(imbalance(bid_sz, ask_sz) / 2)


STREAM = [
    msg(0, ("bid", 10000, 1.0), ("ask", 10002, 1.0), snap=True, gap=True, ts=5),
    msg(1, ("bid", 10000, 3.0), ts=6),                 # bid grows: ofi +2
    msg(0, ("bid", 10000, 1.0), ("ask", 10004, 2.0), snap=True, gap=True, ts=9),
    msg(1, ("ask", 10003, 1.0), ts=10),                # lower ask: ofi -1
]


def test_build_features_one_row_per_message() -> None:
    frame = build_features(replay(STREAM))
    assert list(frame.columns) == list(FEATURE_COLUMNS)
    assert frame["segment"].tolist() == [0, 0, 1, 1]
    assert frame["seq"].tolist() == [0, 1, 0, 1]
    assert frame["ts"].tolist() == [5, 6, 9, 10]
    assert frame["mid"].tolist() == [10001.0, 10001.0, 10002.0, 10001.5]
    assert frame["spread"].tolist() == [2, 2, 4, 3]
    assert frame["imbalance"].tolist() == [0.0, 0.5, -1 / 3, 0.0]   # (1 - 2) / 3 in row 2


def test_ofi_never_crosses_a_segment_boundary() -> None:
    """Each segment's first row has no previous state of its own, so ofi is NaN.

    Without the boundary, row 2 would compare the new snapshot with segment 0's
    last top of book, which is a different book.
    """
    ofi = build_features(replay(STREAM))["ofi"].tolist()
    assert math.isnan(ofi[0]) and math.isnan(ofi[2])
    assert (ofi[1], ofi[3]) == (2.0, -1.0)


def test_one_sided_book_gets_no_row() -> None:
    stream = [
        msg(0, ("bid", 10000, 1.0), snap=True, gap=True),   # no asks yet: no top
        msg(1, ("ask", 10002, 1.0)),
        msg(2, ("ask", 10002, 2.0)),
    ]
    frame = build_features(replay(stream))
    assert frame["seq"].tolist() == [1, 2]
    assert math.isnan(frame["ofi"].iloc[0])   # its previous state had no top


def test_features_never_look_ahead() -> None:
    """The lookahead guard: a row depends only on messages at or before it.

    Features built from a prefix of the stream must equal the same rows built from
    the whole stream. Anything that peeks forward (a centred window, a fill from
    the future) breaks this equality.
    """
    full = build_features(replay(STREAM))
    for k in range(1, len(STREAM)):
        prefix = build_features(replay(STREAM[:k]))
        pd.testing.assert_frame_equal(prefix, full.iloc[: len(prefix)])


CAPTURE = Path("data/coinbase_BTC-USD_2026-08-21.jsonl")


@pytest.mark.skipif(not CAPTURE.exists(), reason="capture file not present")
def test_features_never_look_ahead_on_real_capture() -> None:
    messages = list(islice(load_messages(CAPTURE), 1500))
    full = build_features(replay(messages))
    prefix = build_features(replay(messages[:700]))
    assert len(full) == 1500 and len(prefix) == 700
    pd.testing.assert_frame_equal(prefix, full.iloc[:700])
    assert full["ofi"].notna().sum() == 1499   # NaN only on the snapshot's row


def test_features_from_tops_matches_build_features() -> None:
    """The vectorised path (ITCH tops from C++) equals the replay path exactly."""

    rng = random.Random(7)
    messages = [msg(0, ("bid", 100_00, 1.0), ("ask", 100_02, 1.0), snap=True)]
    bids, asks = {100_00: 1.0}, {100_02: 1.0}
    for seq in range(1, 400):
        if seq == 200:  # a second snapshot opens segment 1
            bids, asks = {200_00: 2.0}, {200_03: 1.5}
            messages.append(msg(seq, ("bid", 200_00, 2.0), ("ask", 200_03, 1.5), snap=True))
            continue
        base = 100_00 if seq < 200 else 200_00
        levels: list[tuple[Side, int, float]] = []
        for _ in range(rng.randint(1, 3)):
            if rng.random() < 0.5:
                p = base - rng.randint(0, 3)
                remove = rng.random() < 0.3 and len(bids) > 1 and p in bids
                size = 0.0 if remove else rng.uniform(0.1, 3)
                bids[p] = size
                if size == 0.0:
                    del bids[p]
                levels.append(("bid", p, size))
            else:
                p = base + 2 + rng.randint(0, 3)
                remove = rng.random() < 0.3 and len(asks) > 1 and p in asks
                size = 0.0 if remove else rng.uniform(0.1, 3)
                asks[p] = size
                if size == 0.0:
                    del asks[p]
                levels.append(("ask", p, size))
        messages.append(msg(seq, *levels))
    expected = build_features(replay(messages))
    tops = expected[["segment", "seq", "ts", "bid_px", "bid_sz", "ask_px", "ask_sz"]]
    got = features_from_tops(tops)
    assert len(expected) == 400  # every message yields a two-sided row
    pd.testing.assert_frame_equal(got[list(FEATURE_COLUMNS)], expected, check_dtype=False)
