# Results: why the edge doesn't survive execution

Run with `python scripts/phase3.py coinbase` and
`python scripts/phase3.py itch data/itch/01302019.<SYM> <SYM>`. The full
output, including every horizon and size, is in [results/](results/), and the
settings with their sources are in [03-settings.md](03-settings.md).

## Summary

The Phase 1 signal predicts direction slightly better than chance on
Coinbase: +0.12 bps per round trip before costs, about twelve standard errors
from zero. Each step towards realistic execution costs something:

1. Latency and batching take it to +0.110 bps from a cloud host and +0.095
   from a home connection.
2. The best level is too small for a 0.01 BTC order on a third of rows, so
   29% of entries fill only partly.
3. The taker fee, 2 bps per side even at Coinbase's top tier, is about 40
   times what remains.
4. Retrained on NASDAQ order-level data for AAPL, MSFT and QQQ, the same
   features don't cover the spread at any horizon from 1 to 60 seconds.
5. Resting at the best price instead of crossing the spread helps much less
   than the half spread it should save, because resting orders mostly fill
   when the price is moving against them.

## Coinbase

13 captures (2026-08-21 to 2026-10-07), logistic regression on imbalance, OFI
and spread, five purged and embargoed walk-forward folds, 100-message horizon.

The engine first has to reproduce Phase 1: with zero latency, an event-count
hold and an order too small to exceed the best level, it gives 12,584 trades
with exactly Phase 1's entry and exit prices. Then:

| latency | size | strategy | round trips | partial entries | gross bps | net, top tier (2 bps) | net, US entry tier (90 bps) |
|---|---|---|---|---|---|---|---|
| Phase 1 scoring | any | model | 12,605 | n/a | +0.120 ± 0.010 | −3.88 | −179.88 |
| cloud, 10 ms each way | 0.01 BTC | model | 12,434 | 28.5% | +0.110 ± 0.010 | −3.89 | −179.89 |
| cloud, 10 ms each way | 0.01 BTC | buy every row | 14,843 | 19.6% | −0.019 ± 0.011 | −4.02 | −180.02 |
| home, 25 ms each way | 0.01 BTC | model | 12,374 | 28.5% | +0.095 ± 0.010 | −3.91 | −179.91 |
| cloud | 0.001 BTC | model | 12,508 | 10.9% | +0.106 ± 0.010 | −3.89 | −179.89 |
| cloud | 0.1 BTC | model | 12,340 | 81.8% | +0.100 ± 0.010 | −3.90 | −179.90 |

Latency costs little here because the signal's horizon (about 5 s) is long
compared with a 50 ms batch. Size matters more for fills than for the edge
itself: at 0.1 BTC most entries can't fill in full at the best level. The
fee is what decides the result, at every tier.

## NASDAQ, 01302019

Each symbol is retrained on its own day with the same pipeline. The horizon
curve, colocated latency, 100 shares, gross bps per round trip (number of
round trips in brackets):

AAPL

| horizon | FLAT share | accuracy / majority class | taker, model | taker, random side | maker, model | maker, random side |
|---|---|---|---|---|---|---|
| 1 s | 0.735 | 0.892 / 0.892 | −0.55 ± 0.75 (10) | −2.71 ± 0.61 (10) | −1.53 ± 0.64 (4) | −2.39 ± 0.52 (8) |
| 5 s | 0.369 | 0.591 / 0.540 | −1.67 ± 0.12 (701) | −1.82 ± 0.12 (701) | −1.46 ± 0.16 (387) | −1.35 ± 0.14 (418) |
| 10 s | 0.350 | 0.396 / 0.427 | −1.20 ± 0.08 (1,271) | −1.30 ± 0.08 (1,271) | −1.36 ± 0.11 (788) | −1.21 ± 0.10 (848) |
| **30 s** | 0.237 | 0.394 / 0.400 | −1.29 ± 0.21 (718) | −1.01 ± 0.21 (718) | −1.25 ± 0.24 (606) | −1.65 ± 0.24 (617) |
| 60 s | 0.065 | 0.411 / 0.369 | −0.97 ± 0.43 (360) | −0.95 ± 0.44 (360) | −1.72 ± 0.44 (330) | −1.95 ± 0.45 (334) |

At short horizons the label is mostly FLAT and the model mostly predicts
FLAT, so it rarely trades. Its accuracy beats the majority class most
clearly at 5 s, and even there a taker round trip loses 1.7 bps. The 2 s row
and the MSFT and QQQ curves are in the results files; none of them has a
horizon where the model is profitable gross with a meaningful number of
trades. (QQQ at 5 s shows +0.07 for the taker model, on 10 trades.)

At the headline 30 s horizon, colocated, 100 shares:

| | AAPL | MSFT | QQQ |
|---|---|---|---|
| taker, model, gross | −1.29 ± 0.21 | −0.88 ± 0.15 | −0.41 ± 0.12 |
| taker, random side, gross | −1.01 ± 0.21 | −0.95 ± 0.15 | −0.63 ± 0.12 |
| maker, model, gross | −1.25 ± 0.24 | −0.65 ± 0.17 | −0.35 ± 0.13 |
| maker, model, net of $0.0030 take and $0.0015 rebate | −1.34 | −0.80 | −0.44 |
| maker entries filled within 1 s | 18% | 11% | 22% |

The model's taker trades are no better than random sides on AAPL or MSFT; on
QQQ the difference is 0.22 bps, about 1.3 standard errors. Moving from
colocated to another New Jersey data centre (170 µs each way) changes the
numbers by a few hundredths of a basis point, which is small next to the
standard errors: a 30-second signal isn't sensitive to microseconds.

## Why resting orders don't help

A maker entry joins the queue at our own best price and should save a whole
spread on the entry compared with a taker. At 30 s it saves 0.04 bps on AAPL,
0.23 on MSFT and 0.06 on QQQ. Markouts show where the rest goes: the mid
price after each entry fill, relative to the fill price, signed so positive
means in our favour (AAPL, 30 s):

| after the fill | 0 s | 0.1 s | 1 s | 10 s |
|---|---|---|---|---|
| taker entry | −0.72 | −0.72 | −0.75 | −0.68 |
| maker entry | −0.13 | −0.32 | −0.34 | −0.61 |

A taker fill starts about half a spread down and stays there. A maker fill
should start about half a spread up, because it bought below the mid; instead
it is already down just after the message that filled it and keeps falling.
A resting buy fills when someone sells into it, and that happens most often
when the price is falling through the level. The orders that would have done
well tend to be the ones that never fill (82% of AAPL maker entries timed
out). On MSFT and QQQ maker fills also start below zero (−0.12 and −0.20 bps
just after the fill) instead of half a spread up.

## Size

Gross bps at 30 s, colocated, with the fraction of decisions where the order
is larger than the best level on the side it takes from:

| | 100 shares | 200 | 500 |
|---|---|---|---|
| AAPL taker | −1.29 (11.9%) | −1.47 (38.1%) | −1.84 (69.2%) |
| MSFT taker | −0.88 (4.4%) | −0.96 (15.7%) | −1.24 (48.5%) |
| QQQ taker | −0.41 (2.5%) | −0.45 (9.9%) | −0.59 (30.4%) |

Larger taker orders walk into the second level and pay more. Maker results
barely change with size.

## Limits of these results

- One day and three symbols on NASDAQ. Training on one day and testing on
  another is the obvious next step.
- ITCH is NASDAQ's own book. The consolidated best quote across exchanges is
  often tighter, which is part of why AAPL's NASDAQ spread was one cent on
  only 21% of messages that day (the day after Apple's earnings).
- Orders have no market impact in the replay.
- Hidden liquidity is ignored, which can only delay our resting fills.
- Fees and latencies come from the sources in [03-settings.md](03-settings.md);
  some are estimates and are labelled as such there.
