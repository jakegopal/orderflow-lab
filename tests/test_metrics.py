"""Tests for scoring metrics, against hand-computed values."""

import numpy as np
import pytest

from orderflow.metrics.performance import edge_bps, hit_rate, turnover


def test_edge_bps_is_mean_per_trade_return() -> None:
    # +$1 on $10,000 is +1 bp; -$3 on $10,000 is -3 bp; mean -1 bp.
    pnl, notional = np.array([1.0, -3.0]), np.array([10_000.0, 10_000.0])
    assert edge_bps(pnl, notional) == pytest.approx(-1.0)


def test_edge_bps_weights_trades_equally() -> None:
    # +1 bp on a small trade and -1 bp on a huge one average to 0, not to about -1.
    pnl, notional = np.array([0.01, -100.0]), np.array([100.0, 1_000_000.0])
    assert edge_bps(pnl, notional) == pytest.approx(0.0)


def test_hit_rate() -> None:
    assert hit_rate(np.array([1.0, -1.0, 0.0, 2.0])) == 0.5   # a zero is not a win


def test_turnover() -> None:
    assert turnover(np.array([50_000.0, 150_000.0]), capital=100_000.0) == 2.0
