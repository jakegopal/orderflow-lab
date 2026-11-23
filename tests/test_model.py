"""Tests for the baseline model: it learns a planted signal and only from train rows."""

import numpy as np
import pandas as pd

from orderflow.features.labels import DOWN, UP
from orderflow.model.baseline import fit_baseline, predict


def planted(n: int, seed: int) -> tuple[pd.DataFrame, pd.Series]:
    """Imbalance decides the label outright; OFI and spread are noise."""
    rng = np.random.default_rng(seed)
    features = pd.DataFrame({
        "imbalance": rng.uniform(-1, 1, n),
        "ofi": rng.normal(0, 5, n),
        "spread": rng.integers(1, 3, n),
    })
    labels = pd.Series(np.where(features["imbalance"] > 0, UP, DOWN))
    return features, labels


def test_learns_a_planted_signal_out_of_sample() -> None:
    train_x, train_y = planted(2000, seed=0)
    test_x, test_y = planted(500, seed=1)
    out = predict(fit_baseline(train_x, train_y), test_x)
    assert (out["direction"] == test_y).mean() > 0.95
    assert out["probability"].between(0.5, 1.0).all()   # the max of two classes


def test_prediction_is_aligned_to_the_input_rows() -> None:
    x, y = planted(300, seed=2)
    subset = x.iloc[100:120]
    out = predict(fit_baseline(x, y), subset)
    assert out.index.equals(subset.index)
