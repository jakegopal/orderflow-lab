# Results: inference in the hot path

Run with `python scripts/phase5.py data/itch/01302019.AAPL AAPL` (and
`cmake --preset onnx && cmake --build --preset onnx` first for the ONNX
Runtime C++ numbers). Output: [results/aapl.md](results/aapl.md). Machine as
in [docs/phase2/04-benchmarks.md](../phase2/04-benchmarks.md).

AAPL rows and labels as in Phase 3, at the same horizon rule (30 s). One
split: train on the first 60% of the day minus a purge and an embargo, test
on the remaining 631,515 rows. Two models on the same three inputs: the
logistic baseline (3 inputs, 3 classes) and an MLP with two hidden layers of
32 ReLU units. Test accuracy is 0.416 and 0.411.

## The C++ model makes the same decisions

Inside the engine, `ModelStrategy` computed the features, the scaling and the
model on all 1,586,524 books. On the 631,515 test rows that Python also
scored, its predicted class agreed with scikit-learn's on every row, for both
models and for both the double-precision and the AVX2 single-precision
evaluator. ONNX Runtime agreed with `infer.hpp` on all 20,000 rows of the
latency run.

## Cost per decision

All timed with serialised TSC reads (`tsc.hpp`). This VM's TSC advances in
steps of about 10 ns, so differences below that aren't meaningful.

| | logistic p50 | p99 | MLP p50 | p99 |
|---|---|---|---|---|
| in the engine: features + scaling + model, scalar double | 40 ns | 60 ns | 812 ns | 1,483 ns |
| in the engine: same, AVX2 float | 40 ns | 60 ns | 130 ns | 230 ns |
| from C++, model only, `infer.hpp` scalar | 20 ns | 130 ns | 681 ns | 1,012 ns |
| from C++, model only, `infer.hpp` AVX2 | 40 ns | 50 ns | 130 ns | 200 ns |
| from C++, ONNX Runtime 1.32 C API `Run()` | 1,643 ns | 2,615 ns | 3,517 ns | 8,106 ns |
| from Python, `infer.hpp` through pybind11 | 561 ns | 862 ns | 1,693 ns | 6,864 ns |
| from Python, ONNX Runtime | 3,447 ns | 18,206 ns | 5,530 ns | 67,448 ns |
| from Python, scikit-learn `predict_proba` | 566 µs | 1,775 µs | 652 µs | 2,647 µs |

- The logistic model is 9 multiply-adds, less than one 8-wide AVX2 register,
  so SIMD can't help it; its cost is mostly the timer and the feature
  arithmetic.
- The MLP's 32 × 32 layer is 1,024 multiply-adds. The compiler can't
  vectorise the scalar loop without `-ffast-math`, because reordering a
  floating-point sum changes the result, so the hand-written AVX2 version is
  about 5× faster.
- ONNX Runtime from C++ costs about 1.6 µs per call for the logistic model,
  40 to 80 times the hand-written evaluator. That is the fixed cost of a
  general graph executor (input binding, output allocation, dispatch) on a
  model with almost no arithmetic. Calling it from Python adds roughly
  another 2 µs.
- scikit-learn spends about half a millisecond per one-row call on input
  validation and building a DataFrame with the fitted feature names. That is
  what a naive per-decision Python strategy pays, about 10,000 times the
  model's own cost.

## Does it change the backtest?

Charging the measured p99 as decision latency, colocated network, taker, 100
shares, on the test rows:

| | decision latency | round trips | gross bps | net bps |
|---|---|---|---|---|
| logistic | 0 | 759 | −1.43 ± 0.23 | −1.79 |
| logistic | 61 ns | 759 | −1.43 ± 0.23 | −1.79 |
| MLP | 0 | 759 | −0.84 ± 0.22 | −1.21 |
| MLP | 1,483 ns | 759 | −0.84 ± 0.22 | −1.21 |

No difference. With 20 µs of network latency each way and a 30-second
horizon, a microsecond of compute doesn't matter. It would for a strategy
whose edge decays within microseconds, such as competing for queue position
or reacting to a stale quote, not for this one.
