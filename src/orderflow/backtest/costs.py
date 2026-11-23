"""Explicit trading costs: exchange fees and slippage.

Convert a trade into the money it costs, on top of the
spread already paid in fills.py. The fee schedule must be cited in the README.
"""


def taker_fee(notional: float, fee_bps: float) -> float:
    """Fee for removing liquidity, in the same units as `notional`.

    notional is the trade's absolute value (price x size). One basis point is
    1/10,000, so a $77,000 buy at 90 bps pays $693.

    fee_bps is always passed in, never defaulted: the rate depends on the account's
    volume tier, and the README cites the rate behind each reported number.
    """
    return notional * fee_bps / 10_000
