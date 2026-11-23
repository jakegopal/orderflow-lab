"""Tests for forward-looking labels."""

import math

import pandas as pd

from orderflow.features.labels import (
    DOWN,
    FLAT,
    UP,
    executable_move_labels,
    executable_move_labels_by_time,
    rows_spanned,
)


def frame(segment: list[int], bid: list[int], ask: list[int]) -> pd.DataFrame:
    return pd.DataFrame({"segment": segment, "bid_px": bid, "ask_px": ask})


def test_move_must_clear_the_spread() -> None:
    f = frame([0] * 4, bid=[100, 101, 102, 100], ask=[101, 102, 103, 101])
    labels = executable_move_labels(f, horizon=1).tolist()
    # Row 0 -> 1: future bid 101 is not above ask 101, so the 1-tick mid move is FLAT.
    # Row 1 -> 2: same again. Row 2 -> 3: future ask 101 < bid 102, so DOWN.
    assert labels[:3] == [FLAT, FLAT, DOWN]
    assert math.isnan(labels[3])


def test_up_when_future_bid_beats_ask_now() -> None:
    f = frame([0, 0], bid=[100, 102], ask=[101, 103])
    assert executable_move_labels(f, horizon=1).tolist()[0] == UP


def test_labels_never_cross_a_segment_boundary() -> None:
    """Row 1's horizon would reach row 2, which is a different book: NaN."""
    f = frame([0, 0, 1, 1], bid=[100, 100, 500, 500], ask=[101, 101, 501, 501])
    labels = executable_move_labels(f, horizon=1).tolist()
    assert labels[0] == FLAT
    assert math.isnan(labels[1])
    assert labels[2] == FLAT
    assert math.isnan(labels[3])


def timed(segment: list[int], ts: list[int], bid: list[int], ask: list[int]) -> pd.DataFrame:
    return pd.DataFrame({"segment": segment, "ts": ts, "bid_px": bid, "ask_px": ask})


def test_clock_horizon_reads_the_first_row_at_or_after_t_plus_h() -> None:
    # h = 10: row 0 (t=0) reads row 2 (t=10, exactly at), row 1 (t=3) reads row 3 (t=15).
    f = timed([0] * 4, ts=[0, 3, 10, 15], bid=[100, 100, 102, 98], ask=[101, 101, 103, 99])
    labels = executable_move_labels_by_time(f, 10).tolist()
    assert labels[0] == UP    # future bid 102 > ask now 101
    assert labels[1] == DOWN  # future ask 99 < bid now 100
    assert math.isnan(labels[2]) and math.isnan(labels[3])


def test_clock_horizon_never_crosses_a_segment() -> None:
    f = timed([0, 0, 1, 1], ts=[0, 5, 20, 30], bid=[100, 100, 500, 500], ask=[101, 101, 501, 501])
    labels = executable_move_labels_by_time(f, 10).tolist()
    assert math.isnan(labels[0]) and math.isnan(labels[1])  # segment 0 ends before t + 10
    assert labels[2] == FLAT
    assert math.isnan(labels[3])


def test_rows_spanned_is_the_longest_lookahead() -> None:
    f = timed([0] * 5, ts=[0, 1, 2, 3, 20], bid=[1] * 5, ask=[2] * 5)
    assert rows_spanned(f, 10) == 4  # row 0 must look all the way to row 4
