"""Phase 5: the research model inside the C++ backtester, and what it costs.

Run from the repo root:  python scripts/phase5.py FILE SYMBOL
(FILE is an ITCH day or an itch_extract of it.)

1. Rows and label for SYMBOL exactly as in Phase 3. One purged, embargoed
   split: train on the first 60% of the day, test on the rest.
2. Fit the logistic baseline and a small MLP on the training rows; export both.
3. Run each model *inside* the engine (ModelStrategy): features, scaling and
   inference in C++ on every book. Check its decisions against scikit-learn's
   predictions on the same test rows, class for class.
4. Per-decision latency, measured, for every way of evaluating the model:
   C++ in the engine (scalar double, AVX2 float), C++ through pybind11,
   scikit-learn, and ONNX Runtime's Python API (when installed).
5. Backtest with the measured decision latency charged, against zero.
"""

import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
from sklearn.pipeline import Pipeline

from orderflow import _core
from orderflow.backtest.engine import ITCH_PRICE_SCALE, Fees, Latency, RoundTrip
from orderflow.model.baseline import MODEL_INPUTS, fit_baseline, fit_mlp
from orderflow.model.export import export_model
from orderflow.pipeline import (
    ITCH_HORIZONS_S,
    NS_PER_SECOND,
    REGULAR_SESSION,
    choose_horizon,
    itch_rows,
    label_rows,
)

TRAIN_FRACTION = 0.6
QTY = 100
SESSION = (REGULAR_SESSION[0] + 5 * 60 * 10**9, REGULAR_SESSION[1] - 5 * 60 * 10**9)
NETWORK = Latency(market_data_ns=20_000, order_entry_ns=20_000)  # colocated, as in Phase 3
TAKE = 0.0030  # $/share, Reg NMS 610(c) cap (see phase3.py)
SINGLE_ROW_CALLS = 20_000


def percentiles(ns: Any) -> str:
    p50, p99, p999 = np.percentile(ns, [50, 99, 99.9])
    return f"p50 {p50:8.1f}  p99 {p99:8.1f}  p99.9 {p999:9.1f} ns  (n={len(ns):,})"


Rows = np.ndarray[Any, np.dtype[np.float64]]


def time_single_rows(call: Callable[[Any], object], x: Any) -> Rows:
    """Wall time of one call per row: what a per-decision call from Python costs."""
    out = np.empty(min(SINGLE_ROW_CALLS, len(x)))
    for i in range(len(out)):
        row = x[i : i + 1]
        t0 = time.perf_counter_ns()
        call(row)
        out[i] = time.perf_counter_ns() - t0
    return out


def from_python(model: Pipeline, model_file: Path, x: Rows) -> None:
    """Per-decision latency of each way to call the model from Python."""
    cpp = _core.InferenceModel(str(model_file))
    columns = list(MODEL_INPUTS)
    print("  called once per decision from Python:")
    print(f"    C++ via pybind11 : {percentiles(time_single_rows(cpp.predict, x))}")
    sk = time_single_rows(lambda r: model.predict_proba(pd.DataFrame(r, columns=columns)), x)
    print(f"    scikit-learn     : {percentiles(sk)}")
    try:
        import onnxruntime as ort
        from skl2onnx import to_onnx
    except ImportError:
        print("    ONNX Runtime     : not installed (pip install -e '.[onnx]')")
        return
    onx = to_onnx(model, x[:1].astype(np.float32), options={"zipmap": False})
    session = ort.InferenceSession(onx.SerializeToString(), providers=["CPUExecutionProvider"])
    feed = session.get_inputs()[0].name
    onnx_ns = time_single_rows(lambda r: session.run(None, {feed: r}), x.astype(np.float32))
    print(f"    ONNX Runtime     : {percentiles(onnx_ns)}")


BENCH_INFER = Path("build/onnx/bench_infer")  # cmake --preset onnx && cmake --build --preset onnx


def from_cpp(model: Pipeline, model_file: Path, x: Rows) -> None:
    """infer.hpp against ONNX Runtime's C API, both called from C++, one row per call."""
    if not BENCH_INFER.exists():
        print("  ONNX Runtime from C++: not built (cmake --preset onnx)")
        return
    from skl2onnx import to_onnx

    onnx_file = model_file.with_suffix(".onnx")
    rows_file = model_file.with_suffix(".f64")
    onx = to_onnx(model, x[:1].astype(np.float32), options={"zipmap": False})
    onnx_file.write_bytes(onx.SerializeToString())
    np.ascontiguousarray(x[:SINGLE_ROW_CALLS], dtype=np.float64).tofile(rows_file)
    out = subprocess.run([str(BENCH_INFER), str(model_file), str(onnx_file), str(rows_file)],
                         capture_output=True, text=True, check=False)
    print("  called once per decision from C++:")
    print("\n".join("  " + line for line in (out.stdout + out.stderr).strip().splitlines()))


def in_engine(path: Path, symbol: str, model_file: Path, evaluator: str, h: int,
              decision_ns: int = 0) -> dict[str, Any]:
    no_signals = pd.Series(dtype=np.int8)
    spec = RoundTrip("model", no_signals, QTY, hold_ns=h * NS_PER_SECOND, session=SESSION).spec()
    latency = Latency(market_data_ns=NETWORK.market_data_ns, decision_ns=decision_ns,
                      order_entry_ns=NETWORK.order_entry_ns)
    return _core.backtest_itch_model(str(path), symbol, str(model_file), evaluator,  # type: ignore[arg-type]
                                     spec, REGULAR_SESSION[0], REGULAR_SESSION[1], Fees(), latency)


def net_bps(raw: dict[str, Any]) -> str:
    t = pd.DataFrame(raw["strategies"][0]["trades"])
    t = t[t["complete"]]
    if t.empty:
        return "no trades"
    side = t["dir"].astype(float)
    gross = side * (t["exit_px"] - t["entry_px"]) / ITCH_PRICE_SCALE * t["exit_qty"]
    notional = t["entry_px"] / ITCH_PRICE_SCALE * t["qty"]
    fees = (t["qty"] + t["exit_qty"]) * TAKE
    g = gross / notional * 1e4
    n = (gross - fees) / notional * 1e4
    se = g.std(ddof=1) / np.sqrt(len(g))
    return f"{len(t):,} trades, gross {g.mean():+.3f} +/- {se:.3f} bps, net {n.mean():+.3f} bps"


def main(path: Path, symbol: str) -> None:
    unlabelled, _ = itch_rows(path, symbol)
    # The horizon rule of Phase 3, applied to this split's training rows only.
    labelled = {hs: label_rows(unlabelled, hs) for hs in ITCH_HORIZONS_S}
    shares = {}
    for hs, lab in labelled.items():
        train_part = lab.rows["label"].iloc[: int(len(lab.rows) * TRAIN_FRACTION) - 2 * lab.purge]
        shares[hs] = train_part.value_counts(normalize=True)
    h = choose_horizon(shares)
    lab = labelled[h]
    rows = lab.rows
    split = int(len(rows) * TRAIN_FRACTION)
    train = rows.iloc[: split - 2 * lab.purge]  # purge + embargo, as in walk_forward_splits
    test = rows.iloc[split:]
    print(f"{symbol}: horizon {h} s; train {len(train):,} rows, test {len(test):,} rows")
    tsc = _core.tsc_per_ns()
    x_test: Rows = test[list(MODEL_INPUTS)].to_numpy(np.float64)

    with tempfile.TemporaryDirectory() as tmp:
        for name, fit in (("logistic", fit_baseline), ("mlp 32x32", fit_mlp)):
            model: Pipeline = fit(train, train["label"])
            model_file = Path(tmp) / f"{name.split()[0]}.txt"
            export_model(model, model_file)
            proba = model.predict_proba(test[list(MODEL_INPUTS)])
            classes = model.classes_[proba.argmax(axis=1)].astype(int)
            want = pd.Series(classes, index=test["event_index"].to_numpy())
            acc = float((want.to_numpy() == test["label"].to_numpy()).mean())
            print(f"\n{name}: test accuracy {acc:.3f}")

            for evaluator in ("scalar", "simd"):
                out = in_engine(path, symbol, model_file, evaluator, h)
                d = out["decisions"]
                got = pd.Series(d["cls"], index=d["event_index"])
                common = want.index.intersection(got.index)
                agree = float((got.loc[common] == want.loc[common]).mean())
                ticks = out["decision_ticks"].astype(np.float64) / tsc
                print(f"  in engine, {evaluator:>6}: {len(got):,} decisions; "
                      f"{len(common):,} on test rows, agreement with scikit-learn {agree:.6f}")
                print(f"    per decision (features + scale + model): {percentiles(ticks)}")
                if evaluator == "scalar":
                    measured_p99 = int(np.ceil(np.percentile(ticks, 99)))
                    scalar_out = out

            from_python(model, model_file, x_test)
            from_cpp(model, model_file, x_test)

            print(f"  backtest, colocated network, taker, {QTY} shares:")
            print(f"    decision latency 0      : {net_bps(scalar_out)}")
            charged = in_engine(path, symbol, model_file, "scalar", h, decision_ns=measured_p99)
            print(f"    decision latency {measured_p99:>4} ns: {net_bps(charged)}  (measured p99)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(Path(sys.argv[1]), sys.argv[2])
