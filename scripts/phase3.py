"""Phase 3: the Phase 1 signal through the C++ event-driven backtester.

    python scripts/phase3.py coinbase              Phase 1's 13 Coinbase captures
    python scripts/phase3.py itch FILE SYMBOL ...  one ITCH day (or an itch_extract of it)

Coinbase: Phase 1's out-of-sample signals are re-scored by the engine. First a
parity run (zero latency, event-clock hold, an order too small to exceed the
best level) that must reproduce Phase 1's round trips exactly, then realistic
latency, sizes and fees.

ITCH: the research pipeline retrained on each symbol at each horizon of a
fixed clock grid, then traded by crossing the spread (taker) or by joining the
queue at our own best price (maker). The settings below were fixed before the
results were looked at; the sources for each are in docs/phase3/03-settings.md.
"""

import sys
from pathlib import Path

import numpy as np
import pandas as pd

from orderflow.backtest.engine import Fees, Latency, Result, RoundTrip, run_itch, run_top_of_book
from orderflow.backtest.roundtrips import round_trips
from orderflow.data.loader import TICKS_PER_UNIT
from orderflow.features.labels import FLAT, executable_move_labels
from orderflow.model.baseline import MODEL_INPUTS
from orderflow.pipeline import (
    ITCH_HORIZONS_S,
    NS_PER_SECOND,
    REGULAR_SESSION,
    Labelled,
    choose_horizon,
    first_block_shares,
    itch_rows,
    label_rows,
    load_all,
    trade_signal,
    walk_forward_predictions,
)

N_FOLDS = 5
MS = 1_000_000
US = 1_000

# Coinbase --------------------------------------------------------------------
CB_HORIZON = 100            # messages, ~5 s: Coinbase publishes about every 50 ms
CB_SIZES = (0.001, 0.01, 0.1)
CB_HEADLINE_SIZE = 0.01     # BTC, ~$770
PARITY_SIZE = 1e-8
PHASE1_LAST_CAPTURE = "2026-10-07"
CB_LATENCIES = {
    "cloud, us-east-1 (10 ms each way)": Latency(market_data_ns=10 * MS, order_entry_ns=10 * MS),
    "home connection (25 ms each way)": Latency(market_data_ns=25 * MS, order_entry_ns=25 * MS),
}
# Taker fee per side, bps of notional: the US entry tier and the top tier of the
# schedule in force since 16 Sep 2026. (Before that the entry taker fee was 120.)
CB_TAKER_BPS = {"US entry tier": 90.0, "top tier": 2.0}

# ITCH ----------------------------------------------------------------------
SESSION = (REGULAR_SESSION[0] + 5 * 60 * NS_PER_SECOND, REGULAR_SESSION[1] - 5 * 60 * NS_PER_SECOND)
ITCH_SIZES = (100, 200, 500)
ITCH_HEADLINE_SIZE = 100
MAKER_TIMEOUT_NS = NS_PER_SECOND
ITCH_LATENCIES = {
    # Nasdaq quotes under 50 us round trip for order entry to acknowledgement at
    # the customer's port in Carteret; ~20 us each way plus 1 us to decide.
    "colocated": Latency(market_data_ns=20 * US, decision_ns=1 * US, order_entry_ns=20 * US),
    # Another New Jersey data centre: ~150 us more each way over fibre.
    "NJ, not colocated": Latency(
        market_data_ns=170 * US, decision_ns=1 * US, order_entry_ns=170 * US
    ),
}
# Per share, Nasdaq Equity 7 Section 118(a) (formerly Rule 7018(a)), Jan 2019.
# AAPL, MSFT and QQQ are Nasdaq-listed (Tape C): the default credit for adding
# displayed liquidity was $0.0015 and the top volume tier $0.00305.
ITCH_TAKE = 0.0030
ITCH_REBATES = {"no rebate": 0.0, "default $0.0015": -0.0015, "top tier $0.00305": -0.00305}


def mean_se(x: pd.Series) -> str:
    if len(x) < 2:
        return f"{x.mean():+.3f}" if len(x) else "n/a"
    return f"{x.mean():+.3f} ± {x.std(ddof=1) / np.sqrt(len(x)):.3f}"


def counted(t: pd.DataFrame) -> str:
    """Mean gross bps ± s.e., with the number of round trips it rests on."""
    return f"{mean_se(gross_bps(t))} ({len(t):,})"


def clean(trades: pd.DataFrame) -> pd.DataFrame:
    """Whole trades that stayed in one trusted segment."""
    t = trades
    return t[t["complete"] & (t["entry_segment"] == t["exit_segment"])
             & (t["decision_segment"] == t["entry_segment"])]


def gross_bps(t: pd.DataFrame) -> pd.Series:
    return t["gross_pnl"] / t["notional"] * 10_000


# Coinbase --------------------------------------------------------------------


def coinbase(captures: list[Path]) -> None:
    print(f"captures: {', '.join(p.stem.rsplit('_', 1)[1] for p in captures)}")
    features = load_all(captures)
    features["label"] = executable_move_labels(features, CB_HORIZON)
    rows = features.dropna(subset=["label", *MODEL_INPUTS]).reset_index(drop=True)

    shares = first_block_shares(rows, rows["label"], N_FOLDS, CB_HORIZON)
    print(f"first-block label shares at {CB_HORIZON} messages: "
          f"{ {k: round(v, 3) for k, v in shares.items()} } "
          f"(FLAT <= 1/3: {shares.get(float(FLAT), 0) <= 1 / 3})")

    folds = walk_forward_predictions(rows, N_FOLDS, CB_HORIZON, CB_HORIZON)
    signal = pd.concat([trade_signal(f) for f in folds]).reindex(rows.index, fill_value=0)

    parity_run = [RoundTrip("parity", signal, PARITY_SIZE, hold_events=CB_HORIZON)]
    parity = run_top_of_book(rows, parity_run, Fees(), Latency(), TICKS_PER_UNIT)["parity"].trades
    whole = parity[parity["complete"] & (parity["exit_segment"] == parity["decision_segment"])]
    python = round_trips(rows, signal, CB_HORIZON, latency=0).set_index("row")
    decided = whole["decision_index"].astype(np.int64).to_numpy()
    same = bool(np.isin(decided, python.index.to_numpy()).all()) and np.allclose(
        python.loc[decided, ["entry_px", "exit_px"]].to_numpy(),
        whole[["entry_px", "exit_px"]].to_numpy() / TICKS_PER_UNIT,
    )
    print(f"parity with Phase 1: {len(whole):,} of {len(parity):,} trades compared, "
          f"identical prices: {same}")
    if not same:
        raise SystemExit("parity failed")

    smaller = (rows["bid_sz"] < CB_HEADLINE_SIZE) | (rows["ask_sz"] < CB_HEADLINE_SIZE)
    print(f"best level smaller than {CB_HEADLINE_SIZE} BTC on {smaller.mean():.1%} of rows\n")
    buy_all = pd.Series(1, index=rows.index, dtype=np.int8)
    print("| latency | size (BTC) | strategy | round trips | partial entries "
          "| gross bps | net, top tier | net, US entry |")
    print("|---|---|---|---|---|---|---|---|")
    for lat_name, lat in CB_LATENCIES.items():
        sizes = CB_SIZES if lat_name.startswith("cloud") else (CB_HEADLINE_SIZE,)
        for size in sizes:
            runs = [RoundTrip("model", signal, size, hold_events=CB_HORIZON),
                    RoundTrip("buy every row", buy_all, size, hold_events=CB_HORIZON)]
            res = run_top_of_book(rows, runs, Fees(), lat, TICKS_PER_UNIT)
            for name, r in res.items():
                t = clean(r.trades)
                g = gross_bps(t)
                partial = (t["qty"] < size - 1e-12).mean()
                net = {k: mean_se(g - 2 * v) for k, v in CB_TAKER_BPS.items()}
                print(f"| {lat_name} | {size:g} | {name} | {len(t):,} | {partial:.1%} "
                      f"| {mean_se(g)} | {net['top tier']} | {net['US entry tier']} |")


# ITCH ------------------------------------------------------------------------

MARKOUTS_NS = (0, 100 * MS, NS_PER_SECOND, 10 * NS_PER_SECOND)


def markouts(t: pd.DataFrame, rows: pd.DataFrame) -> list[float]:
    """Mean mid move after each entry fill, in bps, signed so positive is in our favour.

    Measured on the mid just after the message that filled us, then 0.1 s, 1 s and
    10 s later. A taker fill starts about half a spread down. A maker fill that the
    market didn't select against would start about half a spread up.
    """
    ts, mid = rows["ts"].to_numpy(), rows["mid"].to_numpy()
    out = []
    for delay in MARKOUTS_NS:
        at = np.searchsorted(ts, t["entry_ts"].to_numpy() + delay, side="right") - 1
        move = t["dir"].to_numpy() * (mid[at] - t["entry_px"].to_numpy()) / t["entry_px"].to_numpy()
        out.append(float(move.mean() * 1e4))
    return out


def signals(rows: pd.DataFrame, lab: Labelled) -> tuple[pd.Series, pd.Series, float, float]:
    """Out-of-sample model signals keyed by event index, the same decisions with random
    sides, and mean accuracy vs the train-majority baseline."""
    folds = walk_forward_predictions(lab.rows, N_FOLDS, lab.purge, lab.purge)
    by_row = pd.concat([trade_signal(f) for f in folds])
    events = lab.rows.loc[by_row.index, "event_index"].to_numpy()
    model = pd.Series(by_row.to_numpy(), index=events, dtype=np.int8)
    sides = np.where(np.random.default_rng(0).random(len(model)) < 0.5, 1, -1)
    random_side = pd.Series(np.where(model.to_numpy() != 0, sides, 0), index=model.index,
                            dtype=np.int8)
    acc = float(np.mean([f.accuracy for f in folds]))
    majority = float(np.mean([f.majority_accuracy for f in folds]))
    return model, random_side, acc, majority


def strategies(
    model: pd.Series, random_side: pd.Series, horizon_s: int, size: int
) -> list[RoundTrip]:
    return [
        RoundTrip(f"{entry} {kind}", sig, size, entry=entry, hold_ns=horizon_s * NS_PER_SECOND,
                  maker_timeout_ns=MAKER_TIMEOUT_NS, session=SESSION)
        for entry in ("taker", "maker")
        for kind, sig in (("model", model), ("random", random_side))
    ]


def net_bps(t: pd.DataFrame, rebate: float) -> pd.Series:
    taker_legs = t["exit_qty"] + t["qty"].where(~t["entry_maker"], 0)
    maker_legs = t["qty"].where(t["entry_maker"], 0)
    fees = taker_legs * ITCH_TAKE + maker_legs * rebate
    return (t["gross_pnl"] - fees) / t["notional"] * 10_000


def itch(path: Path, symbol: str) -> None:
    rows, messages = itch_rows(path, symbol)
    print(f"## {symbol}\n\n{messages:,} book messages, {len(rows):,} usable rows in regular hours; "
          f"spread is 1 cent on {(rows['spread'] == 100).mean():.1%} of rows; median best-level "
          f"size {rows['bid_sz'].median():.0f} bid / {rows['ask_sz'].median():.0f} ask shares\n")
    labelled = {h: label_rows(rows, h) for h in ITCH_HORIZONS_S}
    shares = {h: first_block_shares(lab.rows, lab.rows["label"], N_FOLDS, lab.purge)
              for h, lab in labelled.items()}
    headline = choose_horizon(shares)
    colocated = ITCH_LATENCIES["colocated"]

    print("Horizon curve, colocated, 100 shares, gross bps per round trip (round trips):\n")
    print("| horizon | FLAT share (first block) | accuracy / majority | taker model | taker random "
          "| maker model | maker random | maker fill rate |")
    print("|---|---|---|---|---|---|---|---|")
    chosen: dict[str, Result] = {}
    chosen_signals: tuple[pd.Series, pd.Series] | None = None
    for h, lab in labelled.items():
        model, random_side, acc, majority = signals(rows, lab)
        res, diag = run_itch(path, symbol, strategies(model, random_side, h, ITCH_HEADLINE_SIZE),
                             Fees(), colocated)
        g = {name: counted(r.trades[r.trades["complete"]]) for name, r in res.items()}
        m = res["maker model"]
        fill = len(m.trades) / max(1, len(m.trades) + m.stats["entries_unfilled"])
        mark = "**" if h == headline else ""
        flat = shares[h].get(float(FLAT), 0)
        print(f"| {mark}{h} s{mark} | {flat:.3f} | {acc:.3f} / {majority:.3f} "
              f"| {g['taker model']} | {g['taker random']} | {g['maker model']} "
              f"| {g['maker random']} | {fill:.0%} |")
        if h == headline:
            chosen, chosen_signals = res, (model, random_side)
    assert chosen_signals is not None

    print(f"\nHeadline horizon {headline} s (shortest with first-block FLAT share <= 1/3).\n")
    print("Net bps per round trip at the headline, colocated, 100 shares "
          "(take $0.0030 on every taker leg):\n")
    nets = " | ".join(f"net, {k}" for k in ITCH_REBATES)
    print(f"| strategy | round trips | gross | {nets} |")
    print("|---|---|---|" + "---|" * len(ITCH_REBATES))
    for name, r in chosen.items():
        t = r.trades[r.trades["complete"]]
        cells = [mean_se(net_bps(t, rb)) if "maker" in name or rb == 0 else "–"
                 for rb in ITCH_REBATES.values()]
        print(f"| {name} | {len(t):,} | {mean_se(gross_bps(t))} | " + " | ".join(cells) + " |")

    print("\nEntry markouts at the headline (bps, positive = in our favour):\n")
    print("| strategy | " + " | ".join(f"{d / NS_PER_SECOND:g} s" for d in MARKOUTS_NS) + " |")
    print("|---|" + "---|" * len(MARKOUTS_NS))
    for name, r in chosen.items():
        t = r.trades[r.trades["complete"]]
        print(f"| {name} | " + " | ".join(f"{v:+.3f}" for v in markouts(t, rows)) + " |")

    model, random_side = chosen_signals
    print("\nLatency and size at the headline (gross bps; share of decisions where the "
          "order is larger than the best level on the side it takes from):\n")
    print("| latency | size | taker model | maker model | larger than best level |")
    print("|---|---|---|---|---|")
    at_decision = rows.set_index("event_index")
    decided = model[model != 0]
    side_size = np.where(decided.to_numpy() > 0, at_decision.loc[decided.index, "ask_sz"],
                         at_decision.loc[decided.index, "bid_sz"])
    for lat_name, lat in ITCH_LATENCIES.items():
        for size in ITCH_SIZES if lat_name == "colocated" else (ITCH_HEADLINE_SIZE,):
            res, diag = run_itch(path, symbol, strategies(model, random_side, headline, size)[::2],
                                 Fees(), lat)
            g = {n: mean_se(gross_bps(r.trades[r.trades["complete"]])) for n, r in res.items()}
            print(f"| {lat_name} | {size} | {g['taker model']} | {g['maker model']} "
                  f"| {(side_size < size).mean():.1%} |")
    last = diag["last_regression_ts"] / NS_PER_SECOND
    print(f"\nQueue model at the last run: {diag['queue']}; reference regressions "
          f"{diag['ref_regressions']}, the last at {last:.1f} s after midnight\n")


def main(argv: list[str]) -> None:
    if argv[:1] == ["coinbase"]:
        captures = [Path(a) for a in argv[1:]] or [
            p for p in sorted(Path("data").glob("coinbase_BTC-USD_*.jsonl"))
            if p.stem.rsplit("_", 1)[1] <= PHASE1_LAST_CAPTURE
        ]
        coinbase(captures)
    elif argv[:1] == ["itch"] and len(argv) >= 3:
        for symbol in argv[2:]:
            itch(Path(argv[1]), symbol)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
