# Phase 5 run, AAPL

AMD Ryzen AI 5 340, WSL2, GCC 15.2 -O3; onnxruntime 1.32.0, scikit-learn 1.9.0.

```
AAPL: horizon 30 s; train 862,688 rows, test 631,515 rows

logistic: test accuracy 0.416
  in engine, scalar: 1,586,524 decisions; 631,515 on test rows, agreement with scikit-learn 1.000000
    per decision (features + scale + model): p50     40.1  p99     60.1  p99.9     130.2 ns  (n=1,586,524)
  in engine,   simd: 1,586,524 decisions; 631,515 on test rows, agreement with scikit-learn 1.000000
    per decision (features + scale + model): p50     40.1  p99     60.1  p99.9     130.2 ns  (n=1,586,524)
  called once per decision from Python:
    C++ via pybind11 : p50    561.0  p99    862.0  p99.9    9730.4 ns  (n=20,000)
    scikit-learn     : p50 565716.0  p99 1775253.9  p99.9 2425279.9 ns  (n=20,000)
    ONNX Runtime     : p50   3447.0  p99  18206.3  p99.9   40056.0 ns  (n=20,000)
  called once per decision from C++:
  20000 rows, 3 inputs; ONNX Runtime 1.32.0, 1 thread, sequential
    infer.hpp, scalar double     p50     20.0  p99    130.2  p99.9     200.4 ns  (n=20000)
    infer.hpp, AVX2 float        p50     40.1  p99     50.1  p99.9     230.4 ns  (n=20000)
    ONNX Runtime C API, Run()    p50   1643.1  p99   2615.0  p99.9   19487.1 ns  (n=20000)
    class disagreement, ONNX Runtime vs infer.hpp: 0 of 20000 rows
  backtest, colocated network, taker, 100 shares:
    decision latency 0      : 759 trades, gross -1.425 +/- 0.228 bps, net -1.792 bps
    decision latency   61 ns: 759 trades, gross -1.425 +/- 0.228 bps, net -1.792 bps  (measured p99)

mlp 32x32: test accuracy 0.411
  in engine, scalar: 1,586,524 decisions; 631,515 on test rows, agreement with scikit-learn 1.000000
    per decision (features + scale + model): p50    811.6  p99   1482.8  p99.9   24471.6 ns  (n=1,586,524)
  in engine,   simd: 1,586,524 decisions; 631,515 on test rows, agreement with scikit-learn 1.000000
    per decision (features + scale + model): p50    130.2  p99    230.4  p99.9     711.4 ns  (n=1,586,524)
  called once per decision from Python:
    C++ via pybind11 : p50   1693.0  p99   6863.7  p99.9   85953.3 ns  (n=20,000)
    scikit-learn     : p50 652330.0  p99 2647126.8  p99.9 6998055.3 ns  (n=20,000)
    ONNX Runtime     : p50   5530.0  p99  67448.1  p99.9  134636.0 ns  (n=20,000)
  called once per decision from C++:
  20000 rows, 3 inputs; ONNX Runtime 1.32.0, 1 thread, sequential
    infer.hpp, scalar double     p50    681.3  p99   1011.9  p99.9    8917.0 ns  (n=20000)
    infer.hpp, AVX2 float        p50    130.2  p99    200.4  p99.9     400.8 ns  (n=20000)
    ONNX Runtime C API, Run()    p50   3516.7  p99   8105.5  p99.9   28103.7 ns  (n=20000)
    class disagreement, ONNX Runtime vs infer.hpp: 0 of 20000 rows
  backtest, colocated network, taker, 100 shares:
    decision latency 0      : 759 trades, gross -0.838 +/- 0.224 bps, net -1.206 bps
    decision latency 1483 ns: 759 trades, gross -0.838 +/- 0.224 bps, net -1.206 bps  (measured p99)
```
