"""Scoring: what the strategy earned and what it risked.

Blotter in, numbers out. The headline is per-trade edge in
basis points against half the spread; Sharpe is secondary and always reported with
its annualisation factor stated.
"""

import numpy as np
from numpy.typing import NDArray


def edge_bps(pnl: NDArray[np.float64], notional: NDArray[np.float64]) -> float:
    """Mean per-trade edge in basis points of notional traded.

    Each trade's pnl / notional is its return; x 10,000 puts it in bps. The mean
    is taken per trade, not as total pnl over total notional, so a few large
    trades can't dominate: the headline is what a typical trade earns.
    """
    return float(np.mean(pnl / notional) * 10_000)


def hit_rate(pnl: NDArray[np.float64]) -> float:
    """Fraction of trades that made money. Read it next to edge_bps: a high hit
    rate with negative edge means small wins and large losses."""
    return float(np.mean(pnl > 0))


def sharpe(returns: NDArray[np.float64], periods_per_year: float) -> float:
    """Annualised return over volatility. periods_per_year is stated, never assumed."""
    raise NotImplementedError


def max_drawdown(equity: NDArray[np.float64]) -> float:
    """Worst peak-to-trough decline in the equity curve, as a positive fraction."""
    raise NotImplementedError


def turnover(notional: NDArray[np.float64], capital: float) -> float:
    """Total notional traded per unit of capital."""
    return float(np.sum(notional) / capital)
