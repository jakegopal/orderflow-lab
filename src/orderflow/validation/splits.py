"""Time-series splitting with purge and embargo.

Decide which rows may be trained on and which are held out,
given that labels look forward. Separate from model/ because this is where
lookahead hides, and it gets exactly one home and one test file.
"""

from collections.abc import Iterator


def walk_forward_splits(
    n_rows: int,
    n_folds: int,
    label_horizon: int,
    embargo: int,
) -> Iterator[tuple[range, range]]:
    """Yield (train_idx, test_idx) index ranges, rolling forward through time.

    Rows are split into n_folds + 1 equal blocks in time order. Fold k tests on
    block k (the last fold also takes any remainder) and trains on everything
    before it. The training window expands, and never contains a row from the
    test block or after it.

    label_horizon: rows the label looks ahead. A training row within
        label_horizon of the test block has a label computed from test-period
        prices, so those rows are purged.
    embargo: a further gap between the purged training rows and the test block,
        against serial correlation (features with memory, like OFI's previous row).
        In k-fold cross-validation the embargo sits after the test fold, because
        training rows can follow it there. Walk-forward never trains after the
        test fold, so the gap goes before it instead.
    """
    if n_folds < 1:
        raise ValueError(f"n_folds must be >= 1, got {n_folds}")
    block = n_rows // (n_folds + 1)
    if block <= label_horizon + embargo:
        raise ValueError(
            f"{n_rows} rows in {n_folds + 1} blocks leaves no training data "
            f"after purging {label_horizon} and embargoing {embargo} rows"
        )
    for k in range(1, n_folds + 1):
        test_start = k * block
        test_end = n_rows if k == n_folds else test_start + block
        yield range(test_start - label_horizon - embargo), range(test_start, test_end)
