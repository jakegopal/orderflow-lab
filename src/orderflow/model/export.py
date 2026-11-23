"""Write a fitted model in the format cpp/include/orderflow/infer.hpp reads.

Serialise. The text format is documented in infer.hpp.
Numbers are written with 17 significant digits ("%.17g"), the precision at
which every double round-trips exactly, so C++ evaluates the very same model.
"""

from pathlib import Path

import numpy as np
from numpy.typing import NDArray
from sklearn.linear_model import LogisticRegression
from sklearn.neural_network import MLPClassifier
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler


def _numbers(values: NDArray[np.float64]) -> str:
    return " ".join(f"{v:.17g}" for v in np.asarray(values, dtype=np.float64).ravel())


Layer = tuple[NDArray[np.float64], NDArray[np.float64], str]


def _layers(model: LogisticRegression | MLPClassifier) -> list[Layer]:
    """(weights out x in, biases, activation) per layer."""
    if isinstance(model, LogisticRegression):
        # Multinomial logistic regression: softmax(W z + b), one linear layer.
        if model.coef_.shape[0] != len(model.classes_):
            raise ValueError("binary logistic regression stores one row; use multinomial")
        return [(model.coef_, model.intercept_, "linear")]
    if model.activation != "relu" or model.out_activation_ != "softmax":
        raise ValueError("only relu hidden layers with a softmax output are supported")
    n = len(model.coefs_)
    return [
        # scikit-learn stores each layer as in x out; the file is out x in.
        (w.T, b, "linear" if i == n - 1 else "relu")
        for i, (w, b) in enumerate(zip(model.coefs_, model.intercepts_, strict=True))
    ]


def export_model(pipeline: Pipeline, path: Path) -> None:
    """Write scaler + classifier. The pipeline must be StandardScaler -> model."""
    scaler, model = pipeline[0], pipeline[-1]
    if not isinstance(scaler, StandardScaler) or len(pipeline) != 2:
        raise ValueError("expected a two-step pipeline: StandardScaler, then the classifier")
    if not isinstance(model, LogisticRegression | MLPClassifier):
        raise ValueError(f"unsupported classifier {type(model).__name__}")
    classes = np.asarray(model.classes_)
    if not np.array_equal(classes, classes.astype(int)):
        raise ValueError("class labels must be integers")
    assert scaler.mean_ is not None and scaler.scale_ is not None
    lines = [
        "orderflow-model 1",
        f"inputs {len(scaler.mean_)}",
        f"mean {_numbers(scaler.mean_)}",
        f"scale {_numbers(scaler.scale_)}",
        f"classes {len(classes)} {' '.join(str(int(c)) for c in classes)}",
    ]
    layers = _layers(model)
    lines.append(f"layers {len(layers)}")
    for w, b, act in layers:
        lines.append(f"dense {w.shape[1]} {w.shape[0]} {act}")
        lines.append(_numbers(w))
        lines.append(_numbers(b))
    path.write_text("\n".join(lines) + "\n")
