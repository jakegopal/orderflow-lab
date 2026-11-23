"""The exported model, evaluated in C++, must equal scikit-learn's."""

from collections.abc import Callable
from pathlib import Path

import numpy as np
import pandas as pd
import pytest
from sklearn.linear_model import LogisticRegression
from sklearn.pipeline import Pipeline, make_pipeline

from orderflow import _core
from orderflow.model.baseline import MODEL_INPUTS, fit_baseline, fit_mlp
from orderflow.model.export import export_model


def data(n: int, seed: int) -> tuple[pd.DataFrame, pd.Series]:
    rng = np.random.default_rng(seed)
    x = pd.DataFrame({
        "imbalance": rng.uniform(-1, 1, n),
        "ofi": rng.normal(0, 3, n),
        "spread": rng.integers(1, 4, n).astype(float),
    })
    score = 2 * x["imbalance"] + 0.3 * x["ofi"] + rng.normal(0, 1, n)
    y = pd.Series(np.select([score > 0.7, score < -0.7], [1.0, -1.0], 0.0))
    return x, y


@pytest.mark.parametrize("fit", [fit_baseline, fit_mlp], ids=["logistic", "mlp"])
def test_cpp_inference_matches_sklearn(
    fit: Callable[[pd.DataFrame, pd.Series], Pipeline], tmp_path: Path
) -> None:
    x, y = data(5_000, 1)
    model = fit(x, y)
    path = tmp_path / "model.txt"
    export_model(model, path)
    cpp = _core.InferenceModel(str(path))
    assert cpp.classes == [-1, 0, 1]

    test, _ = data(2_000, 2)
    proba = model.predict_proba(test[list(MODEL_INPUTS)])
    want_cls = model.classes_[proba.argmax(axis=1)].astype(np.int64)
    want_p = proba.max(axis=1)
    arr = test[list(MODEL_INPUTS)].to_numpy(np.float64)

    # Double precision: the same function, to rounding.
    cls, p = cpp.predict(arr)
    np.testing.assert_array_equal(cls, want_cls)
    np.testing.assert_allclose(p, want_p, rtol=1e-10, atol=1e-12)

    # Single-precision SIMD: may differ only where two classes are nearly tied.
    cls_s, p_s = cpp.predict(arr, simd=True)
    assert (cls_s == want_cls).mean() > 0.995
    np.testing.assert_allclose(p_s[cls_s == want_cls], want_p[cls_s == want_cls], atol=1e-4)


def test_export_rejects_a_missing_scaler(tmp_path: Path) -> None:
    x, y = data(500, 3)
    bare = make_pipeline(LogisticRegression(max_iter=200)).fit(x, y)
    with pytest.raises(ValueError, match="StandardScaler"):
        export_model(bare, tmp_path / "m.txt")
