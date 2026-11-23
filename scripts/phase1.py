"""Phase 1 end to end: captures -> features -> labels -> walk-forward -> per-trade edge.

Run from the repo root: `python scripts/phase1.py` (all finished captures), or
pass capture paths. Today's file is skipped by default because the recorder
may still be appending to it, which would make the result unrepeatable.

Features are cached as parquet next to the captures (data/ is gitignored).
Replay's best-price scans are O(levels) per message on purpose, so a full
build takes a while. The cache is rebuilt whenever its capture is newer.
"""

import sys
from datetime import UTC, datetime
from pathlib import Path

import numpy as np
import pandas as pd

from orderflow.backtest.roundtrips import non_overlapping, round_trips
from orderflow.features.labels import executable_move_labels
from orderflow.metrics.performance import edge_bps, hit_rate
from orderflow.model.baseline import MODEL_INPUTS
from orderflow.pipeline import load_all, trade_signal, walk_forward_predictions

HORIZON = 100         # messages, ~5 s at Coinbase's 50 ms cadence; also the purge
EMBARGO = HORIZON     # one more horizon of gap before each test block
LATENCY = 1           # messages between decision and entry
N_FOLDS = 5
# Taker fee per side, bps. Coinbase shows the authoritative tier table only to a
# logged-in account. These are the VIP floor and the US entry rate after the
# Sept 2026 schedule change, as reported secondhand. Verify against your account.
FEES_BPS = {"gross": 0.0, "VIP floor 2 bp": 2.0, "US entry 90 bp": 90.0}
CONFIDENT = 0.6       # also report trades where the predicted class has p >= this


def report(name: str, trades: pd.DataFrame) -> None:
    """Edge for every trade, then for the non-overlapping subset with its error bar.

    Only the non-overlapping trades get a standard error: overlapping trades
    share price paths, so treating them as independent would overstate the
    evidence.
    """
    print(f"\n{name}:")
    for subset, t in [("all", trades), ("non-overlapping", non_overlapping(trades, HORIZON))]:
        pnl, notional = t["pnl"].to_numpy(), t["entry_px"].to_numpy()
        gross = edge_bps(pnl, notional)
        se = (pnl / notional * 10_000).std(ddof=1) / np.sqrt(len(t)) if len(t) > 1 else np.nan
        error = f" +/- {se:.3f} (1 s.e.)" if subset != "all" else ""
        print(f"  {subset}: {len(t):,} round trips, hit rate {hit_rate(pnl):.1%}, "
              f"mean half spread {t['half_spread_bps'].mean():.4f} bps")
        for label, fee in FEES_BPS.items():
            print(f"    edge {label:>15}: {gross - 2 * fee:+9.4f} bps{error}")


def main(argv: list[str]) -> None:
    today = datetime.now(UTC).strftime("%Y-%m-%d")
    captures = (
        [Path(a) for a in argv]
        if argv
        else [p for p in sorted(Path("data").glob("coinbase_BTC-USD_*.jsonl"))
              if today not in p.name]
    )
    print(f"captures: {', '.join(p.stem.rsplit('_', 1)[1] for p in captures)}")
    features = load_all(captures)
    features["label"] = executable_move_labels(features, HORIZON)
    rows = features.dropna(subset=["label", *MODEL_INPUTS]).reset_index(drop=True)
    shares = rows["label"].value_counts(normalize=True).sort_index()
    print(f"rows: {len(features):,} features, {len(rows):,} labelled; "
          f"label shares {shares.round(3).to_dict()}")

    trades, confident, always_buy = [], [], []
    folds = walk_forward_predictions(rows, N_FOLDS, HORIZON, EMBARGO)
    for k, fold in enumerate(folds):
        te = rows.iloc[fold.test.start : fold.test.stop]
        trades.append(round_trips(te, trade_signal(fold), HORIZON, LATENCY))
        confident.append(round_trips(te, trade_signal(fold, CONFIDENT), HORIZON, LATENCY))
        always_buy.append(round_trips(te, pd.Series(1, index=te.index), HORIZON, LATENCY))
        print(f"fold {k}: train {len(fold.train):,} test {len(fold.test):,} accuracy "
              f"{fold.accuracy:.3f} (train-majority baseline {fold.majority_accuracy:.3f}), "
              f"{len(trades[-1]):,} trades")
    accuracy = [f.accuracy for f in folds]
    majority = [f.majority_accuracy for f in folds]

    print(f"\nmean accuracy {np.mean(accuracy):.3f} vs majority {np.mean(majority):.3f}")
    report("all non-flat predictions", pd.concat(trades, ignore_index=True))
    report(f"predictions with p >= {CONFIDENT}", pd.concat(confident, ignore_index=True))
    report("naive benchmark: buy on every row", pd.concat(always_buy, ignore_index=True))


if __name__ == "__main__":
    main(sys.argv[1:])
