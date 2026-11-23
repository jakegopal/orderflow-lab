# Phase 5: inference in the hot path

1. [The model inside the backtester](01-inference.md): the model as
   arithmetic, export, the two C++ evaluators, and parity with Python.
2. [Results](02-results.md): exact parity, cost per decision for every way
   of calling the model, and whether that cost matters to the strategy.

Raw output in [results/](results/). Reproduce with
`pip install -e ".[onnx]"`, `cmake --preset onnx && cmake --build --preset onnx`, then
`python scripts/phase5.py data/itch/01302019.AAPL AAPL`.
