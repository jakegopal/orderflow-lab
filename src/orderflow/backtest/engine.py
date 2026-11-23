"""The event-driven backtester, run from Python.

Hand signals and configuration to the C++ engine
(cpp/include/orderflow/backtest/) and turn its results into DataFrames. The
simulation itself, with latency, executable fills, queue position and fees,
happens in C++. A per-event Python callback would cost more than the
simulation, so Python passes whole arrays in and gets whole tables back.

Two venues:
  run_top_of_book  Coinbase: the feature frame's top of book, one row per
                   message. Taker orders only, because L2 data can't place an
                   order in a queue.
  run_itch         NASDAQ ITCH: one symbol of a full order-level day, taker
                   or maker orders, with FIFO queue position.

Prices in the trade tables are in the venue's integer units (Coinbase cents,
ITCH 1/10,000 dollar); `price_scale` converts them. pnl is in dollars, net of
fees.
"""

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal

import numpy as np
import pandas as pd

from orderflow import _core

Fees = _core.Fees
Latency = _core.Latency

ITCH_PRICE_SCALE = 10_000  # ITCH prices have 4 implied decimals


@dataclass(frozen=True)
class RoundTrip:
    """One strategy: enter on each signal, hold, exit with a market order.

    signal: +1 buy / -1 sell / 0 nothing, indexed by event index (row number
        of the top-of-book table the features came from).
    hold_ns or hold_events: how long to hold; exactly one must be set.
        hold_events counts book messages from the decision, which reproduces
        Phase 1's fixed-horizon round trips.
    entry: "taker" crosses the spread at once; "maker" joins the queue at its
        own best price and cancels after maker_timeout_ns if unfilled.
    session: decisions only on books with exchange time in [start, end).
    """

    name: str
    signal: pd.Series
    qty: float
    entry: Literal["taker", "maker"] = "taker"
    hold_ns: int = 0
    hold_events: int = 0
    maker_timeout_ns: int = 1_000_000_000
    session: tuple[int, int] | None = None

    def spec(self) -> _core.StrategySpec:
        nonzero = self.signal[self.signal != 0]
        everything = (int(np.iinfo(np.int64).min), int(np.iinfo(np.int64).max))
        start, end = self.session if self.session else everything
        return _core.StrategySpec(
            index=nonzero.index.to_numpy(dtype=np.uint64),
            direction=np.sign(nonzero.to_numpy()).astype(np.int8),
            qty=self.qty,
            entry=self.entry,
            hold_ns=self.hold_ns,
            hold_events=self.hold_events,
            maker_timeout_ns=self.maker_timeout_ns,
            session_start=int(start),
            session_end=int(end),
        )


@dataclass(frozen=True)
class Result:
    """trades: one row per round trip; stats: counters from the strategy."""

    trades: pd.DataFrame
    stats: dict[str, int]


def _trades(raw: dict[str, Any], price_scale: float) -> pd.DataFrame:
    t = pd.DataFrame(raw)
    side = t["dir"].astype(float)
    # A partly exited trade (data ended) is scored on what exited; complete
    # says which trades are whole.
    exited = t["exit_qty"]
    t["notional"] = t["entry_px"] / price_scale * t["qty"]
    t["gross_pnl"] = side * (t["exit_px"] - t["entry_px"]) / price_scale * exited
    t["pnl"] = t["gross_pnl"] - t["fees"]
    mid = (t["decision_bid"] + t["decision_ask"]) / 2
    t["half_spread_bps"] = (t["decision_ask"] - t["decision_bid"]) / 2 / mid * 10_000
    return t


def _results(
    out: dict[str, Any], strategies: Sequence[RoundTrip], price_scale: float
) -> dict[str, Result]:
    return {
        s.name: Result(_trades(r["trades"], price_scale), dict(r["stats"]))
        for s, r in zip(strategies, out["strategies"], strict=True)
    }


def run_top_of_book(
    features: pd.DataFrame,
    strategies: Sequence[RoundTrip],
    fees: _core.Fees,
    latency: _core.Latency,
    price_scale: float,
    batch_intervals: bool = True,
) -> dict[str, Result]:
    """Backtest taker strategies on a feature frame's top of book.

    features needs ts, bid_px, bid_sz, ask_px, ask_sz and segment, one row per
    message in time order, with a 0..n-1 index: the event index the signals use.
    With batch_intervals (the default), each row stands for the book from just
    after the previous row until its own timestamp, so an order arriving between
    two batches is matched against the later one (see markets.hpp).
    """
    if not features.index.equals(pd.RangeIndex(len(features))):
        raise ValueError("features must have a 0..n-1 index (call reset_index(drop=True))")
    out = _core.backtest_top_of_book(
        features["ts"].to_numpy(np.int64),
        features["bid_px"].to_numpy(np.int64),
        features["bid_sz"].to_numpy(np.float64),
        features["ask_px"].to_numpy(np.int64),
        features["ask_sz"].to_numpy(np.float64),
        features["segment"].to_numpy(np.int64),
        price_scale,
        [s.spec() for s in strategies],
        fees,
        latency,
        batch_intervals,
    )
    return _results(out, strategies, price_scale)


def run_itch(
    path: Path,
    symbol: str,
    strategies: Sequence[RoundTrip],
    fees: _core.Fees,
    latency: _core.Latency,
) -> tuple[dict[str, Result], dict[str, Any]]:
    """Backtest strategies on one symbol of an ITCH file (or an itch_extract of it).

    Also returns the run's diagnostics: queue-model counters, book anomalies,
    malformed messages, and order-reference regressions with the time of the
    last one. Anomalies or malformed messages mean the book can't be trusted,
    and raise. Queue priority follows feed order, so regressions matter only
    if an order re-entered with an older reference appears after one of ours:
    every strategy's session must start after last_regression_ts, or this raises.
    """
    out = _core.backtest_itch(str(path), symbol, [s.spec() for s in strategies], fees, latency)
    keys = ("queue", "book_anomalies", "malformed", "ref_regressions", "last_regression_ts")
    diagnostics = {k: out[k] for k in keys}
    if out["book_anomalies"] or out["malformed"]:
        raise RuntimeError(f"untrustworthy replay of {path}: {diagnostics}")
    earliest = min((s.session[0] for s in strategies if s.session), default=None)
    if out["ref_regressions"] and (earliest is None or earliest <= out["last_regression_ts"]):
        raise RuntimeError(
            f"{symbol}: order references regress until {out['last_regression_ts']} ns, inside a "
            f"strategy's session; queue priority there is not provably feed order: {diagnostics}"
        )
    return _results(out, strategies, ITCH_PRICE_SCALE), diagnostics


def itch_top_of_book(path: Path, symbol: str) -> pd.DataFrame:
    """Top of book after every message that changed symbol's book.

    Row i is event index i. Prices are ITCH units (1/10,000 dollar), and an
    empty side has price 0 and size 0. tradable is False while the symbol is
    halted or before its first trading-state message.
    """
    raw = _core.itch_top_of_book(str(path), symbol)
    if raw["book_anomalies"]:
        raise RuntimeError(f"{raw['book_anomalies']} book anomalies replaying {path}")
    frame = pd.DataFrame({k: raw[k] for k in ("ts", "bid_px", "bid_sz", "ask_px", "ask_sz")})
    frame["tradable"] = raw["tradable"].astype(bool)
    return frame
