"""What crossing the Python/C++ boundary costs, measured on a real capture.

    python scripts/bench_bindings.py [capture.jsonl]

Replays the same messages into the pure-Python BookState and into the C++
L2Book three ways, and reports nanoseconds per update and per message:

  python         BookState.apply per update, best bid/ask by O(n) scan
  cpp per update L2Book.apply: one boundary crossing per update
  cpp per message L2Book.apply_many: one crossing per message, with the NumPy
                 arrays built from Python objects inside the timed region
                 (that conversion is part of the real cost)
  cpp prebuilt   apply_many with arrays built beforehand: the C++ work plus
                 one crossing, the floor for this design

Each run also reads the top of book after every message, as feature building
does, because that is where the data structures differ most (dict scan vs
std::map begin()). Best of 3 runs; the machine is not otherwise idle-checked.
"""

import sys
import time
import warnings
from collections.abc import Callable
from pathlib import Path

import numpy as np

from orderflow._core import L2Book
from orderflow.book.state import BookState
from orderflow.data.loader import load_messages
from orderflow.data.schema import BookMessage

N_MESSAGES = 3_000  # after the opening snapshot; the Python scan makes more slow


def first_segment(path: Path) -> list[BookMessage]:
    """The opening snapshot plus the next N_MESSAGES live messages."""
    out: list[BookMessage] = []
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        for m in load_messages(path):
            if not out and not m.is_snapshot:
                continue
            if out and m.is_snapshot:
                break
            out.append(m)
            if len(out) > N_MESSAGES:
                break
    return out


def best_of_3(run: Callable[[], None]) -> float:
    times = []
    for _ in range(3):
        start = time.perf_counter_ns()
        run()
        times.append(time.perf_counter_ns() - start)
    return min(times)


def main() -> None:
    default = Path("data/coinbase_BTC-USD_2026-08-21.jsonl")
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else default
    messages = first_segment(path)
    snapshot, live = messages[0], messages[1:]
    n_updates = sum(len(m.updates) for m in live)
    arrays = [
        (
            np.array([u.side == "bid" for u in m.updates], dtype=np.bool_),
            np.array([u.price_ticks for u in m.updates], dtype=np.int64),
            np.array([u.size for u in m.updates], dtype=np.float64),
        )
        for m in live
    ]

    def python() -> None:
        book = BookState()
        for u in snapshot.updates:
            book.apply(u)
        for m in live:
            for u in m.updates:
                book.apply(u)
            book.top_of_book()

    def fresh_cpp() -> L2Book:
        book = L2Book()
        for u in snapshot.updates:
            book.apply(u.side, u.price_ticks, u.size)
        return book

    def cpp_per_update(book: L2Book) -> None:
        for m in live:
            for u in m.updates:
                book.apply(u.side, u.price_ticks, u.size)
            book.best_bid()
            book.best_ask()

    def cpp_per_message(book: L2Book) -> None:
        for m in live:
            book.apply_many(
                np.array([u.side == "bid" for u in m.updates], dtype=np.bool_),
                np.array([u.price_ticks for u in m.updates], dtype=np.int64),
                np.array([u.size for u in m.updates], dtype=np.float64),
            )
            book.best_bid()
            book.best_ask()

    def cpp_prebuilt(book: L2Book) -> None:
        for is_bid, prices, sizes in arrays:
            book.apply_many(is_bid, prices, sizes)
            book.best_bid()
            book.best_ask()

    # Snapshot loading is excluded from the C++ timings, so subtract it from Python's too.
    def python_snapshot_only() -> None:
        book = BookState()
        for u in snapshot.updates:
            book.apply(u)

    results = {"python": best_of_3(python) - best_of_3(python_snapshot_only)}
    for name, fn in [("cpp per update", cpp_per_update), ("cpp per message", cpp_per_message),
                     ("cpp prebuilt", cpp_prebuilt)]:
        times = []
        for _ in range(3):
            book = fresh_cpp()
            start = time.perf_counter_ns()
            fn(book)
            times.append(time.perf_counter_ns() - start)
        results[name] = min(times)

    print(f"{len(live):,} live messages, {n_updates:,} updates, "
          f"book of ~{len(snapshot.updates):,} levels")
    print(f"{'':16} {'ns/update':>10} {'us/message':>11} {'vs python':>10}")
    for name, ns in results.items():
        print(f"{name:16} {ns / n_updates:10.0f} {ns / len(live) / 1e3:11.1f} "
              f"{results['python'] / ns:9.1f}x")


if __name__ == "__main__":
    main()
