# 5. Python bindings, the parity test, and the language boundary

Code: [module.cpp](../../cpp/bindings/module.cpp),
[l2_book.hpp](../../cpp/include/orderflow/l2_book.hpp),
[_core.pyi](../../src/orderflow/_core.pyi),
[pyproject.toml](../../pyproject.toml).
Tests: [test_parity.py](../../tests/test_parity.py),
[test_l2_book.cpp](../../cpp/tests/test_l2_book.cpp).
Benchmark: [bench_bindings.py](../../scripts/bench_bindings.py).

## How Python gets the C++ code

`pip install -e .` now runs CMake. The build backend changed from hatchling
(pure Python packaging) to **scikit-build-core**, which runs `CMakeLists.txt`,
compiles `cpp/bindings/module.cpp` with **pybind11**, and installs the result
as `orderflow/_core.cpython-314-x86_64-linux-gnu.so`. In Python:

```python
from orderflow._core import L2Book
book = L2Book()
book.apply("bid", 7_700_000, 1.25)      # $77,000.00, 1.25 BTC
book.best_bid()                          # (7700000, 1.25)
```

pybind11 generates the glue: it converts Python `int`/`float`/`str` to C++
types on the way in, converts `std::optional<std::pair<…>>` back to `None` or a
tuple on the way out, and turns a C++ exception into a Python `ValueError`.
`_core.pyi` is a hand-written type stub so `mypy --strict` can check calls into
the compiled module, which has no Python source to read types from.

After changing C++ code, re-run `pip install -e .`. The persistent build
directory (`build/{wheel_tag}`) makes that an incremental rebuild.

## Why the Coinbase book is a separate class

ITCH (L3) and Coinbase (L2) are different in kind ([01-itch.md](01-itch.md)). An ITCH message
says "order 5001 lost 30 shares", so the book *adds and subtracts* and must
track orders. A Coinbase update says "this price now has 1.25 BTC", so it
*replaces* an absolute size, and 0 removes the level. Forcing both through one
class would complicate both. `L2Book` is a sorted map per side with exactly
`BookState`'s semantics.

## The parity test: Python is the oracle

`BookState` is simple enough to verify by reading it: two dicts, a `max()` and
a `min()`. `L2Book` is the fast one. **If they ever disagree on the same input,
the C++ book is wrong.** [test_parity.py](../../tests/test_parity.py) checks:

- **Random updates** (runs in CI): 20,000 updates with 30% removals, top of
  book compared after every update, full depth at the end.
- **A whole real capture** (08-21, local only since `data/` is gitignored):
  replay drives `BookState` and, for each message, the same updates go to
  `L2Book` through one `apply_many` call. A snapshot clears the C++ book
  exactly where replay starts a fresh `BookState`. The test compares top of
  book after **every one of the 53,474 messages**, full depth every 2,000, and
  asserts that the depth comparison really ran 26 times, so a test that
  silently compared nothing can't pass.

Sizes are compared with `==`, not "approximately equal". Neither book does
arithmetic on sizes, so any difference at all is a bug, not rounding.

### A bug this found in the loader

Binding the book exposed that the loader passed Coinbase's raw side string
through: `BookUpdate.side` held `"offer"` while the schema says
`Literal["bid", "ask"]`. Nothing broke, because `BookState` only tests
`== "bid"`. But the first code to test `== "ask"` would have silently matched
nothing, and mypy could not see it, because JSON values are typed `Any`. The
loader now maps `offer → ask` and raises on anything unexpected (commit
`b4d66f8`).

## The cost of crossing the language boundary

Every call from Python into C++ pays a fixed cost: pybind11 converts each
argument, checks types, and builds the return value. Measured on the 08-21
capture (2,764 live messages, 97,536 updates, a ~43k-level book), reading the
top of book after every message:

| | ns per update | vs Python |
|---|---|---|
| Python `BookState` | 7,014 | 1× |
| C++, one call per update | 172 | 41× |
| C++, one call per message, NumPy arrays built from Python objects | 185 | 38× |
| C++, one call per message, arrays prebuilt | 81 | 87× |

How to read it:
- **Python's cost is the best-price scan,** not the dict update. `max()` over
  ~43k levels after every message is O(n). `std::map::begin()` is O(1).
- **Batching saved nothing here.** One call per message instead of per update
  should cut boundary crossings ~35×, but building three NumPy arrays from
  Python objects costs about as much as the crossings it replaced (185 vs 172).
- **The floor is 81 ns.** That's the C++ map work plus one crossing per
  message. Getting below it means the data must never exist as Python objects:
  parse the JSON in C++ (a C++ loader) or replay inside C++ entirely. That's how the
  backtester ([docs/phase3](../phase3/01-engine.md)) works: Python configures a run
  and C++ owns the event loop.
