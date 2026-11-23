"""The C++ backtester, driven from Python (orderflow.backtest.engine)."""

from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from orderflow.backtest.engine import (
    Fees,
    Latency,
    RoundTrip,
    itch_top_of_book,
    run_itch,
    run_top_of_book,
)
from orderflow.backtest.roundtrips import round_trips

ITCH_EXTRACT = Path("data/itch/01302019.AAPL")


def random_tops(n: int, seed: int) -> pd.DataFrame:
    """A random-walk top of book in cents, two segments, sizes large enough
    that every 0.01-unit order fits in the best level."""
    rng = np.random.default_rng(seed)
    bid = 7_700_000 + np.cumsum(rng.integers(-2, 3, n))
    spread = rng.integers(1, 4, n)
    return pd.DataFrame({
        "segment": (np.arange(n) >= n // 2).astype(np.int64),
        "ts": np.arange(n, dtype=np.int64) * 50_000_000,
        "bid_px": bid,
        "bid_sz": rng.uniform(1, 5, n),
        "ask_px": bid + spread,
        "ask_sz": rng.uniform(1, 5, n),
        "mid": bid + spread / 2,
        "spread": spread,
    })


def test_engine_reproduces_phase1_round_trips_at_zero_latency() -> None:
    """Zero latency + an event-clock hold: every trade's entry and exit prices
    equal Phase 1's round_trips(latency=0) for the same decision row."""
    tops = random_tops(5_000, 1)
    rng = np.random.default_rng(2)
    signal = pd.Series(rng.choice([-1, 0, 0, 0, 1], len(tops)), index=tops.index).astype(np.int8)
    trades = run_top_of_book(
        tops, [RoundTrip("s", signal, 0.01, hold_events=20)], Fees(), Latency(), 100
    )["s"].trades
    clean = trades[trades["complete"] & (trades["exit_segment"] == trades["decision_segment"])]
    assert len(clean) > 100
    python = round_trips(tops, signal, 20, latency=0).set_index("row")
    decided = clean["decision_index"].astype(np.int64).to_numpy()
    np.testing.assert_allclose(
        python.loc[decided, ["entry_px", "exit_px"]].to_numpy(),
        clean[["entry_px", "exit_px"]].to_numpy() / 100,
    )


def test_latency_moves_entry_to_a_later_book() -> None:
    """Rows are 50 ms batches. An order decided on row i that arrives 40 ms later
    falls in row i+1's interval; 60 ms later, in row i+2's. Zero latency meets row i."""
    tops = random_tops(200, 3)
    signal = pd.Series(0, index=tops.index, dtype=np.int8)
    signal[[10, 60, 110]] = 1
    zero = Latency()
    lat = Latency(market_data_ns=20_000_000, order_entry_ns=20_000_000)
    late = Latency(market_data_ns=30_000_000, order_entry_ns=30_000_000)
    strategy = [RoundTrip("s", signal, 0.01, hold_events=5)]
    for latency, offset in ((zero, 0), (lat, 1), (late, 2)):
        t = run_top_of_book(tops, strategy, Fees(), latency, 100)["s"].trades
        expected = tops["ask_px"].to_numpy()[[10 + offset, 60 + offset, 110 + offset]]
        np.testing.assert_allclose(t["entry_px"].to_numpy(), expected, rtol=1e-12)  # a VWAP


def test_fees_are_charged_on_both_legs() -> None:
    tops = random_tops(100, 4)
    signal = pd.Series(0, index=tops.index, dtype=np.int8)
    signal[5] = 1
    fees = Fees(taker_bps=10.0)
    t = run_top_of_book(tops, [RoundTrip("s", signal, 0.01, hold_events=5)], fees, Latency(), 100)
    trade = t["s"].trades.iloc[0]
    entry, exit_ = float(trade["entry_px"]), float(trade["exit_px"])
    expected = 0.01 * (entry + exit_) / 100 * 10 / 10_000
    assert float(trade["fees"]) == pytest.approx(expected)
    assert float(trade["pnl"]) == pytest.approx(float(trade["gross_pnl"]) - expected)


def test_requires_a_range_index() -> None:
    tops = random_tops(10, 5).iloc[2:]
    with pytest.raises(ValueError, match="0..n-1 index"):
        run_top_of_book(tops, [], Fees(), Latency(), 100)


@pytest.mark.skipif(not ITCH_EXTRACT.exists(), reason="needs an ITCH extract in data/itch")
def test_itch_extract_replays_cleanly_and_trades() -> None:
    tops = itch_top_of_book(ITCH_EXTRACT, "AAPL")
    assert len(tops) > 100_000
    ok = tops["tradable"] & (tops["bid_px"] > 0) & (tops["ask_px"] > 0)
    assert (tops.loc[ok, "bid_px"] < tops.loc[ok, "ask_px"]).mean() > 0.999
    signal = pd.Series(0, index=pd.RangeIndex(len(tops)), dtype=np.int8)
    signal[::5_000] = 1
    results, diag = run_itch(
        ITCH_EXTRACT,
        "AAPL",
        [RoundTrip("maker", signal, 100, entry="maker", hold_events=500,
                   session=(35_700 * 10**9, 57_300 * 10**9))],  # 9:55 to 15:55
        Fees(),
        Latency(market_data_ns=5_000, order_entry_ns=5_000),
    )
    assert diag["book_anomalies"] == 0
    assert diag["last_regression_ts"] < 9.6 * 3_600 * 10**9  # only in the open's re-entries
    assert diag["queue"]["passive_fills"] > 0
    assert len(results["maker"].trades) > 0
