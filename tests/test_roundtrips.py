"""Tests for fixed-horizon round trips at executable prices."""

import pandas as pd
import pytest

from orderflow.backtest.roundtrips import non_overlapping, round_trips


def frame(segment: list[int], bid: list[int], ask: list[int]) -> pd.DataFrame:
    f = pd.DataFrame({"segment": segment, "bid_px": bid, "ask_px": ask})
    f["mid"] = (f["bid_px"] + f["ask_px"]) / 2
    f["spread"] = f["ask_px"] - f["bid_px"]
    return f


# Ticks: bid/ask $100.00/$100.01, then the book moves up a dollar and stays.
F = frame([0] * 4, bid=[10000, 10000, 10100, 10100], ask=[10001, 10001, 10101, 10101])


def test_buy_enters_at_next_ask_and_exits_at_bid() -> None:
    trades = round_trips(F, pd.Series([1, 0, 0, 0]), horizon=1, latency=1)
    t = trades.iloc[0]
    assert (t["entry_px"], t["exit_px"]) == (100.01, 101.00)   # ask at row 1, bid at row 2
    assert t["pnl"] == pytest.approx(0.99)


def test_sell_enters_at_bid_and_exits_at_ask() -> None:
    trades = round_trips(F, pd.Series([-1, 0, 0, 0]), horizon=1, latency=1)
    t = trades.iloc[0]
    assert (t["entry_px"], t["exit_px"]) == (100.00, 101.01)
    assert t["pnl"] == pytest.approx(-1.01)


def test_unchanged_book_round_trip_loses_the_spread() -> None:
    trades = round_trips(F, pd.Series([0, 0, 1, 0]), horizon=0, latency=1)
    assert trades.iloc[0]["pnl"] == pytest.approx(-0.01)


def test_trades_leaving_the_segment_are_dropped() -> None:
    f = frame([0, 0, 1, 1], bid=[10000] * 4, ask=[10001] * 4)
    trades = round_trips(f, pd.Series([1, 1, 1, 1]), horizon=1, latency=0)
    assert trades["row"].tolist() == [0, 2]   # rows 1 and 3 would exit in another book


def test_half_spread_benchmark_in_bps() -> None:
    trades = round_trips(F, pd.Series([1, 0, 0, 0]), horizon=1)
    # 1 tick / 2 on a $100.005 mid: 0.5 / 10000.5 * 10,000 = about 0.49998 bps.
    assert trades.iloc[0]["half_spread_bps"] == pytest.approx(0.5 / 10000.5 * 10_000)


def test_non_overlapping_waits_for_the_exit() -> None:
    """horizon 2, latency 1: a trade decided at row r is open until row r + 3."""
    trades = pd.DataFrame({"row": [0, 1, 2, 3, 4, 7]})
    kept = non_overlapping(trades, horizon=2, latency=1)
    assert kept["row"].tolist() == [0, 3, 7]
