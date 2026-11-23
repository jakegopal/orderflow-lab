"""Type stubs for the C++ extension module (cpp/bindings/module.cpp)."""

from typing import Any, Literal

import numpy as np
import numpy.typing as npt

class L2Book:
    def __init__(self) -> None: ...
    def apply(self, side: Literal["bid", "ask"], price_ticks: int, size: float) -> None: ...
    def apply_many(
        self,
        is_bid: npt.NDArray[np.bool_],
        prices: npt.NDArray[np.int64],
        sizes: npt.NDArray[np.float64],
    ) -> None: ...
    def clear(self) -> None: ...
    def best_bid(self) -> tuple[int, float] | None: ...
    def best_ask(self) -> tuple[int, float] | None: ...
    def bids(self) -> list[tuple[int, float]]: ...
    def asks(self) -> list[tuple[int, float]]: ...
    @property
    def bid_levels(self) -> int: ...
    @property
    def ask_levels(self) -> int: ...

class Fees:
    def __init__(
        self,
        *,
        taker_bps: float = 0.0,
        maker_bps: float = 0.0,
        taker_per_unit: float = 0.0,
        maker_per_unit: float = 0.0,
    ) -> None: ...
    @property
    def taker_bps(self) -> float: ...
    @property
    def maker_bps(self) -> float: ...
    @property
    def taker_per_unit(self) -> float: ...
    @property
    def maker_per_unit(self) -> float: ...

class Latency:
    def __init__(
        self, *, market_data_ns: int = 0, decision_ns: int = 0, order_entry_ns: int = 0
    ) -> None: ...
    @property
    def market_data_ns(self) -> int: ...
    @property
    def decision_ns(self) -> int: ...
    @property
    def order_entry_ns(self) -> int: ...

class StrategySpec:
    def __init__(
        self,
        *,
        index: npt.NDArray[np.uint64],
        direction: npt.NDArray[np.int8],
        qty: float,
        entry: Literal["taker", "maker"],
        hold_ns: int = 0,
        hold_events: int = 0,
        maker_timeout_ns: int = 1_000_000_000,
        session_start: int = ...,
        session_end: int = ...,
    ) -> None: ...

def itch_top_of_book(path: str, symbol: str) -> dict[str, Any]: ...
def backtest_itch(
    path: str, symbol: str, strategies: list[StrategySpec], fees: Fees, latency: Latency
) -> dict[str, Any]: ...
def backtest_top_of_book(
    ts: npt.NDArray[np.int64],
    bid_px: npt.NDArray[np.int64],
    bid_sz: npt.NDArray[np.float64],
    ask_px: npt.NDArray[np.int64],
    ask_sz: npt.NDArray[np.float64],
    segment: npt.NDArray[np.int64],
    price_scale: float,
    strategies: list[StrategySpec],
    fees: Fees,
    latency: Latency,
    batch_intervals: bool = True,
) -> dict[str, Any]: ...

class InferenceModel:
    def __init__(self, path: str) -> None: ...
    @property
    def inputs(self) -> int: ...
    @property
    def classes(self) -> list[int]: ...
    def predict(
        self, x: npt.NDArray[np.float64], simd: bool = False
    ) -> tuple[npt.NDArray[np.int64], npt.NDArray[np.float64]]: ...

def backtest_itch_model(
    path: str,
    symbol: str,
    model_path: str,
    evaluator: Literal["scalar", "simd"],
    strategy: StrategySpec,
    usable_start: int,
    usable_end: int,
    fees: Fees,
    latency: Latency,
    batch_intervals: bool = True,
) -> dict[str, Any]: ...
def tsc_per_ns(ms: int = 200) -> float: ...
