"""Baseline classifier: multinomial logistic regression on standardised inputs.

Fit and predict. It knows nothing about how the data was
split -- that lives in validation/ -- so leakage has one place to hide.
"""

import pandas as pd
from sklearn.linear_model import LogisticRegression
from sklearn.neural_network import MLPClassifier
from sklearn.pipeline import Pipeline, make_pipeline
from sklearn.preprocessing import StandardScaler

MODEL_INPUTS = ("imbalance", "ofi", "spread")


def fit_baseline(features: pd.DataFrame, labels: pd.Series) -> Pipeline:
    """Fit logistic regression on MODEL_INPUTS to predict UP / FLAT / DOWN.

    Inputs are standardised first, using the training rows' mean and spread only.
    OFI is in BTC and can be large, while imbalance sits in [-1, 1], so on raw
    scales the regularisation would penalise them unequally. The scaler is part
    of the fitted pipeline, so it never sees test rows.
    """
    model = make_pipeline(StandardScaler(), LogisticRegression(max_iter=1000))
    model.fit(features[list(MODEL_INPUTS)], labels)
    return model


def predict(model: Pipeline, features: pd.DataFrame) -> pd.DataFrame:
    """Per row: the most likely class (`direction`) and its probability."""
    proba = model.predict_proba(features[list(MODEL_INPUTS)])
    best = proba.argmax(axis=1)
    return pd.DataFrame(
        {"direction": model.classes_[best], "probability": proba.max(axis=1)},
        index=features.index,
    )


def fit_mlp(
    features: pd.DataFrame, labels: pd.Series, hidden: tuple[int, ...] = (32, 32)
) -> Pipeline:
    """A small neural network on the same inputs: the workload for the C++ inference benchmark.

    Two ReLU hidden layers of 32 units. Its purpose is the C++ inference
    comparison: a 3-input logistic regression is too small for SIMD to help,
    and a 32-wide layer fills four AVX2 registers. Early stopping holds out a
    random 10% of the *training* rows to choose the epoch. That split is
    shuffled (scikit-learn always shuffles it), so neighbouring rows of the
    training block straddle it, which flatters the validation score but
    never touches the test block. Fixed seed, so the fit is reproducible.
    """
    model = make_pipeline(
        StandardScaler(),
        MLPClassifier(
            hidden_layer_sizes=hidden,
            early_stopping=True,
            random_state=0,
            max_iter=50,
        ),
    )
    model.fit(features[list(MODEL_INPUTS)], labels)
    return model
