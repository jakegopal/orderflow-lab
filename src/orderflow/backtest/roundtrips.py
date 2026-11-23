"""Phase 1's scoring: fixed-horizon round trips at executable prices.

Turn a direction per feature row into priced trades.
This is not the event-driven backtester (that is the C++ engine in backtest/).
Each trade is scored on its own, trades may overlap, and every order is
assumed to fit in the best level. The point is the per-trade edge, not a
portfolio's equity curve.
"""

import pandas as pd

from orderflow.backtest.fills import fill_price
from orderflow.data.loader import TICKS_PER_UNIT
from orderflow.data.schema import Direction

TRADE_COLUMNS = ("row", "segment", "direction", "entry_px", "exit_px", "pnl", "half_spread_bps")


def round_trips(
    features: pd.DataFrame, signal: pd.Series, horizon: int, latency: int = 1
) -> pd.DataFrame:
    """One row per signalled trade, priced across the spread at entry and exit.

    signal: +1 buy, -1 sell, 0 no trade, aligned with the feature rows.
    latency: rows between the decision and the entry. With 1, the order reaches
        the book one message after the one the decision saw, so a trade never
        fills at the very price that triggered it.
    horizon: rows from entry to exit.

    Entry and exit both cross the spread: a buy enters at the ask and exits at
    the bid, and a sell the reverse. Prices are USD per 1 BTC, and pnl is USD per
    1 BTC before fees. half_spread_bps is measured at the decision, as the
    benchmark the edge has to beat. A trade whose entry or exit falls outside
    the decision's segment is dropped, because its prices would come from a
    different book.
    """
    by_segment = features.groupby("segment")
    prices = pd.DataFrame({
        "entry_bid": by_segment["bid_px"].shift(-latency),
        "entry_ask": by_segment["ask_px"].shift(-latency),
        "exit_bid": by_segment["bid_px"].shift(-(latency + horizon)),
        "exit_ask": by_segment["ask_px"].shift(-(latency + horizon)),
    }) / TICKS_PER_UNIT
    half_spread_bps = features["spread"] / 2 / features["mid"] * 10_000

    rows = []
    traded = signal[signal != 0].index.intersection(prices.dropna().index)
    for row in traded:
        direction: Direction = "buy" if signal[row] > 0 else "sell"
        exit_direction: Direction = "sell" if direction == "buy" else "buy"
        p = prices.loc[row]
        entry = fill_price(direction, p["entry_bid"], p["entry_ask"])
        exit_ = fill_price(exit_direction, p["exit_bid"], p["exit_ask"])
        pnl = exit_ - entry if direction == "buy" else entry - exit_
        rows.append((
            row, features.at[row, "segment"], direction, entry, exit_, pnl,
            half_spread_bps[row],
        ))
    return pd.DataFrame.from_records(rows, columns=TRADE_COLUMNS)


def non_overlapping(trades: pd.DataFrame, horizon: int, latency: int = 1) -> pd.DataFrame:
    """Keep each trade only if the previous kept trade has already exited.

    Trades opened on consecutive messages share almost all of their price path,
    so their pnls are strongly correlated, and a standard error that treats them
    as independent overstates the evidence. One position at a time, taken
    greedily in time order, gives trades whose paths don't overlap. That is also
    what a single trader with one position could actually have done.
    """
    kept, free_from = [], -1
    for i, row in enumerate(trades["row"]):
        if row >= free_from:
            kept.append(i)
            free_from = row + latency + horizon
    return trades.iloc[kept].reset_index(drop=True)
