"""The research pipeline's shared steps, for scripts/phase1.py and phase3.py.

Wire the stages together (cached features, labels,
walk-forward signals) so that every script runs the same research code. Each
stage's logic stays in its own module; this file only orders the calls.
"""

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

from orderflow.backtest.engine import itch_top_of_book
from orderflow.book.replay import replay
from orderflow.data.loader import load_messages
from orderflow.features.labels import FLAT, executable_move_labels_by_time, rows_spanned
from orderflow.features.microstructure import build_features, features_from_tops
from orderflow.model.baseline import MODEL_INPUTS, fit_baseline, predict
from orderflow.validation.splits import walk_forward_splits


def cached_features(capture: Path) -> pd.DataFrame:
    """Features for one Coinbase capture, cached as parquet beside it.

    The cache is rebuilt whenever its capture is newer, so a capture still
    being appended to is never read from a stale cache.
    """
    cache = capture.with_suffix(".features.parquet")
    if not cache.exists() or cache.stat().st_mtime < capture.stat().st_mtime:
        print(f"  building features for {capture.name} ...", flush=True)
        build_features(replay(load_messages(capture))).to_parquet(cache)
    return pd.read_parquet(cache)


def load_all(captures: Sequence[Path]) -> pd.DataFrame:
    """Concatenate captures in time order, renumbering segments to stay unique."""
    frames, offset = [], 0
    for capture in captures:
        f = cached_features(capture)
        f["segment"] += offset
        offset = int(f["segment"].max()) + 1 if len(f) else offset
        frames.append(f)
    return pd.concat(frames, ignore_index=True)


@dataclass(frozen=True)
class Fold:
    """One walk-forward fold's out-of-sample predictions."""

    train: range
    test: range
    direction: pd.Series     # UP / FLAT / DOWN per test row
    probability: pd.Series   # the predicted class's probability
    accuracy: float
    majority_accuracy: float  # always predicting the training set's most common class


def walk_forward_predictions(
    rows: pd.DataFrame, n_folds: int, horizon: int, embargo: int
) -> list[Fold]:
    """Fit on each fold's training block and predict its test block.

    rows must be labelled ('label') with no missing model inputs, in time
    order. Every prediction is out of sample: the model that makes it was
    fitted only on rows before the test block, minus the purge and embargo.
    """
    folds = []
    for train, test in walk_forward_splits(len(rows), n_folds, horizon, embargo):
        tr, te = rows.iloc[train.start : train.stop], rows.iloc[test.start : test.stop]
        out = predict(fit_baseline(tr, tr["label"]), te)
        folds.append(Fold(
            train=train,
            test=test,
            direction=out["direction"],
            probability=out["probability"],
            accuracy=float((out["direction"] == te["label"]).mean()),
            majority_accuracy=float((te["label"] == tr["label"].mode()[0]).mean()),
        ))
    return folds


def trade_signal(fold: Fold, min_probability: float = 0.0) -> pd.Series:
    """+1 / -1 / 0 per test row: trade the predicted direction, never FLAT."""
    signal = fold.direction.where(fold.direction != FLAT, 0)
    return signal.where(fold.probability >= min_probability, 0).astype(np.int8)


NS_PER_SECOND = 10**9
NS_PER_HOUR = 3_600 * NS_PER_SECOND
# NASDAQ's regular session, 9:30 to 16:00 Eastern; ITCH times are ns since midnight.
REGULAR_SESSION = (int(9.5 * NS_PER_HOUR), 16 * NS_PER_HOUR)
# Label horizons in clock time, the same grid for every symbol. A message count
# spans very different times on different stocks (1,000 AAPL messages took ~15 s
# on 01302019; 5,000 MSFT messages ~70 s), so horizons in messages aren't
# comparable across symbols.
ITCH_HORIZONS_S = (1, 2, 5, 10, 30, 60)
# The headline horizon: the shortest one whose FLAT share is at most 1/3, measured
# on the first walk-forward block only, so the choice uses no test-period data.
MAX_FLAT_SHARE = 1 / 3


def first_block_shares(
    rows: pd.DataFrame, labels: pd.Series, n_folds: int, purge: int
) -> pd.Series:
    """Label class shares on the first fold's training rows: the data available
    before any out-of-sample prediction is made."""
    block = len(rows) // (n_folds + 1)
    first = labels.iloc[: max(0, block - purge)]
    return first.value_counts(normalize=True).sort_index()


def choose_horizon(
    shares_by_horizon: dict[int, pd.Series], max_flat: float = MAX_FLAT_SHARE
) -> int:
    """The shortest horizon whose first-block FLAT share is at most max_flat
    (or the longest candidate, if none qualifies)."""
    for h, shares in sorted(shares_by_horizon.items()):
        if float(shares.get(float(FLAT), 0.0)) <= max_flat:
            return h
    return max(shares_by_horizon)


def itch_rows(path: Path, symbol: str) -> tuple[pd.DataFrame, int]:
    """Usable feature rows for one symbol of an ITCH day, unlabelled, and the
    symbol's total book-message count.

    A row is usable if the symbol is trading, the book is two-sided and not
    crossed, and the time is in the regular session. A new segment starts after
    every unusable state, so no feature or label spans a halt or a one-sided
    book. event_index is the row's index in the full top-of-book table, which is
    how signals are keyed for the backtest.
    """
    tops = itch_top_of_book(path, symbol)
    tops["event_index"] = np.arange(len(tops))
    usable = (
        tops["tradable"]
        & (tops["bid_px"] > 0) & (tops["ask_px"] > 0) & (tops["bid_px"] < tops["ask_px"])
        & (tops["ts"] >= REGULAR_SESSION[0]) & (tops["ts"] < REGULAR_SESSION[1])
    )
    tops["segment"] = (~usable).cumsum()
    return features_from_tops(tops[usable].reset_index(drop=True)), len(tops)


@dataclass(frozen=True)
class Labelled:
    """Rows with a label at one clock horizon, and the purge window it needs."""

    rows: pd.DataFrame  # RangeIndex; rows without a label or a model input dropped
    horizon_s: int
    purge: int          # rows: the longest any label looks ahead


def label_rows(rows: pd.DataFrame, horizon_s: int) -> Labelled:
    out = rows.copy()
    h_ns = horizon_s * NS_PER_SECOND
    out["label"] = executable_move_labels_by_time(out, h_ns)
    purge = rows_spanned(out, h_ns)
    out = out.dropna(subset=["label", *MODEL_INPUTS]).reset_index(drop=True)
    return Labelled(out, horizon_s, purge)
