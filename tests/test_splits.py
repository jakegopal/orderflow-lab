"""Tests for walk-forward splits: the leakage guard lives here."""

import pytest

from orderflow.validation.splits import walk_forward_splits


def test_no_training_label_reaches_the_test_block() -> None:
    """The leakage guard. Fails if the purge or the embargo is weakened.

    The last training row's label looks label_horizon rows ahead. That window,
    plus the embargo, must end before the test block starts.
    """
    horizon, embargo = 100, 20
    for train, test in walk_forward_splits(10_000, 4, horizon, embargo):
        assert train[-1] + horizon + embargo < test[0]
        assert train[-1] + horizon + embargo == test[0] - 1   # and no wider than needed


def test_folds_walk_forward_and_cover_the_tail() -> None:
    splits = list(walk_forward_splits(1003, 4, label_horizon=10, embargo=5))
    tests = [(t.start, t.stop) for _, t in splits]
    assert tests == [(200, 400), (400, 600), (600, 800), (800, 1003)]   # remainder in last
    for train, _test in splits:
        assert train.start == 0                                          # expanding window


def test_too_few_rows_to_purge_is_an_error() -> None:
    with pytest.raises(ValueError, match="no training data"):
        list(walk_forward_splits(100, 4, label_horizon=20, embargo=0))
