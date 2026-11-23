# The model inside the backtester

Code: [infer.hpp](../../cpp/include/orderflow/infer.hpp),
[model_strategy.hpp](../../cpp/include/orderflow/backtest/model_strategy.hpp),
[export.py](../../src/orderflow/model/export.py),
[bench_infer.cpp](../../cpp/bench/bench_infer.cpp),
[phase5.py](../../scripts/phase5.py).
Tests: [test_export.py](../../tests/test_export.py).
Results: [02-results.md](02-results.md).

In Phases 1 and 3 the model ran in Python ahead of time and the backtester
replayed its decisions. A live system has to compute features, evaluate the
model and decide on every book update, inside the process that holds the
book. Here the model is exported to C++ and evaluated inside the engine on
every book, and the time each decision takes is measured.

## The model as arithmetic

The fitted scikit-learn pipeline is a short function:

```
z      = (x - mean) / scale          StandardScaler
logits = W z + b                     3 classes x 3 inputs
class  = argmax(logits)              DOWN / FLAT / UP
p      = softmax(logits)[class]
```

The MLP inserts two hidden layers, `h = relu(W h + b)`, before the output
layer. `export.py` writes `mean`, `scale` and every `W` and `b` to a text
file with 17 significant digits, which is enough for every double to read
back exactly, so the C++ model holds the same numbers as the Python one.

## Two evaluators

- `predict()` works in double precision with plain loops. It is the
  reference, and `test_export.py` requires it to give the same class as
  scikit-learn on every row and probabilities equal to within 1e-10.
- `predict_simd()` works in single precision with AVX2: each fused
  multiply-add instruction computes 8 outputs of a layer
  (`acc += x[i] * W[i][0..8]`), with the weights stored input-major so those
  8 are contiguous. It can in principle disagree with the reference where two
  classes are nearly tied.

Both use scratch buffers allocated when the model is loaded, so a call
allocates nothing.

## Matching the research pipeline's rules

`ModelStrategy` has to make the same decisions Python would. A book is usable
only if the symbol is trading, the book is two-sided and not crossed, and the
time is in the regular session. OFI needs the previous book, and only an
immediately preceding usable book counts, which is the C++ form of Python
starting a new segment at every unusable state. Python has no decision in the
last h of each segment, because those rows have no label; the live model
decides there too, so the comparison covers the rows both have.

## The ONNX Runtime comparison

`bench_infer` loads the same pipeline converted to ONNX with skl2onnx and
times `OrtApi::Run` on one row at a time next to `infer.hpp`, from C++, with
the same timer. The session uses one thread, sequential execution and a
preallocated input tensor, which is the cheapest way to run one small row.
The build fetches ONNX Runtime 1.32.0 from Microsoft's NuGet package, pinned
by SHA256. The Python timings
use `perf_counter_ns` around one call, so they include Python's own call
overhead, which is the relevant cost for a strategy written in Python.
