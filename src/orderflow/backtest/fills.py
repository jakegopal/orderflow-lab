"""Executable fill prices -- the module that stops PnL being fiction.

Given a direction and the current top of book, return the
price actually payable. Buys lift the ask, sells hit the bid. The mid price does
not appear in this file.
"""

from orderflow.data.schema import Direction


def fill_price(direction: Direction, best_bid: float, best_ask: float) -> float:
    """Price paid or received when crossing the spread immediately.

    A buy takes the cheapest resting sell order (the ask); a sell takes the
    highest resting buy order (the bid). A round trip therefore pays the spread.

    Top-of-book only: it assumes the order fits in the best level's size. An order
    larger than that would walk the book to worse prices, which this ignores.
    """
    return best_ask if direction == "buy" else best_bid
