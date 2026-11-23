"""Tests for executable fills and explicit costs."""

import pytest

from orderflow.backtest.costs import taker_fee
from orderflow.backtest.fills import fill_price


def test_buy_lifts_the_ask() -> None:
    assert fill_price("buy", best_bid=100.0, best_ask=101.0) == 101.0


def test_sell_hits_the_bid() -> None:
    assert fill_price("sell", best_bid=100.0, best_ask=101.0) == 100.0


def test_round_trip_pays_the_spread() -> None:
    """Buy then immediately sell into an unchanged book: lose exactly the spread."""
    bid, ask = 100.0, 101.0
    assert fill_price("sell", bid, ask) - fill_price("buy", bid, ask) == -(ask - bid)


def test_taker_fee_in_basis_points() -> None:
    assert taker_fee(77_000.0, fee_bps=90.0) == pytest.approx(693.0)   # 0.90%
    assert taker_fee(10_000.0, fee_bps=1.0) == pytest.approx(1.0)      # 1 bp of $10k
    assert taker_fee(10_000.0, fee_bps=0.0) == 0.0
