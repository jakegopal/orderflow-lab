"""Labels: the one place in the pipeline that is allowed to look forward.

For each feature row, say what happened next. Kept apart
from microstructure.py, whose promise is that nothing there peeks ahead. The
horizon set here is also the purge window in validation/splits.py.
"""

import numpy as np
import pandas as pd

UP, FLAT, DOWN = 1, 0, -1


def executable_move_labels(features: pd.DataFrame, horizon: int) -> pd.Series:
    """Label each row by whether a trade across the spread would have paid, before fees.

    With bid, ask now and bid', ask' `horizon` rows later in the same segment:
      UP   if bid' > ask   (buy at the ask now, sell at the bid later: a profit)
      DOWN if ask' < bid   (sell at the bid now, buy back at the ask later)
      FLAT otherwise       (the move didn't clear the spread either way)

    Using the spread as the threshold ties the label to executable prices, not
    the mid. With a 1-tick spread most of the time, a mid move of one tick is FLAT.

    The horizon counts rows (one per message, ~50 ms apart on Coinbase). Rows
    whose horizon runs past the end of their segment get NaN: the future state
    belongs to a different book, so no label crosses a segment boundary.
    """
    by_segment = features.groupby("segment")
    future_bid = by_segment["bid_px"].shift(-horizon)
    future_ask = by_segment["ask_px"].shift(-horizon)
    label = np.select(
        [future_bid > features["ask_px"], future_ask < features["bid_px"]],
        [UP, DOWN],
        FLAT,
    ).astype(float)
    label[future_bid.isna().to_numpy()] = np.nan
    return pd.Series(label, index=features.index, name="label")


def future_row_after(features: pd.DataFrame, horizon_ns: int) -> pd.Series:
    """For each row, the position of the first row of its segment at or after ts + horizon_ns.

    NaN when the segment ends first. Positions are into features as given (0..n-1),
    so features must have a RangeIndex and be in time order within each segment.
    """
    ts = features["ts"].to_numpy()
    seg = features["segment"].to_numpy()
    out = np.full(len(features), np.nan)
    # Segments are contiguous runs, so each one is a slice [start, end).
    bounds = np.flatnonzero(np.diff(seg)) + 1
    for start, end in zip(np.r_[0, bounds], np.r_[bounds, len(seg)], strict=True):
        at = np.searchsorted(ts[start:end], ts[start:end] + horizon_ns, side="left") + start
        out[start:end] = np.where(at < end, at, np.nan)
    return pd.Series(out, index=features.index, name="future_row")


def executable_move_labels_by_time(features: pd.DataFrame, horizon_ns: int) -> pd.Series:
    """executable_move_labels with the horizon in clock time instead of rows.

    The future state is the first message boundary at or after ts + horizon_ns in
    the same segment. A clock horizon means the same thing on every symbol, whereas
    a row count spans 15 s on one stock and 70 s on another. Rows whose horizon
    runs past the end of their segment get NaN.
    """
    future = future_row_after(features, horizon_ns)
    known = future.notna().to_numpy()
    idx = future.to_numpy()[known].astype(np.int64)
    bid, ask = features["bid_px"].to_numpy(), features["ask_px"].to_numpy()
    label = np.full(len(features), np.nan)
    fb, fa = bid[idx], ask[idx]
    label[known] = np.select([fb > ask[known], fa < bid[known]], [UP, DOWN], FLAT)
    return pd.Series(label, index=features.index, name="label")


def rows_spanned(features: pd.DataFrame, horizon_ns: int) -> int:
    """The most rows any label reaches ahead: the purge window for a clock horizon."""
    future = future_row_after(features, horizon_ns)
    ahead = future - np.arange(len(features))
    return int(np.nanmax(ahead.to_numpy())) if ahead.notna().any() else 0
