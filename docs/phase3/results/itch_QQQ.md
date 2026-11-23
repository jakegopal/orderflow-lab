## QQQ

3,448,973 book messages, 3,287,776 usable rows in regular hours; spread is 1 cent on 80.0% of rows; median best-level size 900 bid / 840 ask shares

Horizon curve, colocated, 100 shares, gross bps per round trip (round trips):

| horizon | FLAT share (first block) | accuracy / majority | taker model | taker random | maker model | maker random | maker fill rate |
|---|---|---|---|---|---|---|---|
| 1 s | 0.802 | 0.901 / 0.901 | -0.720 ± 0.126 (16) | -0.568 ± 0.117 (16) | +0.000 ± 0.000 (2) | -0.761 ± 0.291 (4) | 12% |
| 2 s | 0.651 | 0.822 / 0.822 | -0.419 ± 0.174 (13) | -0.607 ± 0.182 (13) | +0.007 ± 1.215 (2) | -0.760 ± 0.291 (4) | 14% |
| 5 s | 0.461 | 0.661 / 0.661 | +0.065 ± 0.399 (10) | -1.090 ± 0.451 (10) | +1.223 ± 1.223 (2) | +0.412 ± 1.075 (3) | 18% |
| 10 s | 0.343 | 0.486 / 0.515 | -0.539 ± 0.056 (760) | -0.634 ± 0.056 (760) | -0.353 ± 0.097 (341) | -0.466 ± 0.067 (566) | 15% |
| **30 s** | 0.170 | 0.361 / 0.355 | -0.406 ± 0.122 (693) | -0.628 ± 0.123 (693) | -0.351 ± 0.126 (610) | -0.566 ± 0.125 (611) | 22% |
| 60 s | 0.136 | 0.426 / 0.427 | -0.196 ± 0.229 (347) | -0.394 ± 0.229 (347) | -0.109 ± 0.237 (326) | -0.113 ± 0.240 (328) | 23% |

Headline horizon 30 s (shortest with first-block FLAT share <= 1/3).

Net bps per round trip at the headline, colocated, 100 shares (take $0.0030 on every taker leg):

| strategy | round trips | gross | net, no rebate | net, default $0.0015 | net, top tier $0.00305 |
|---|---|---|---|---|---|
| taker model | 693 | -0.406 ± 0.122 | -0.771 ± 0.122 | – | – |
| taker random | 693 | -0.628 ± 0.123 | -0.993 ± 0.123 | – | – |
| maker model | 610 | -0.351 ± 0.126 | -0.533 ± 0.126 | -0.442 ± 0.126 | -0.348 ± 0.126 |
| maker random | 611 | -0.566 ± 0.125 | -0.748 ± 0.125 | -0.657 ± 0.125 | -0.563 ± 0.125 |

Entry markouts at the headline (bps, positive = in our favour):

| strategy | 0 s | 0.1 s | 1 s | 10 s |
|---|---|---|---|---|
| taker model | -0.352 | -0.339 | -0.340 | -0.387 |
| taker random | -0.349 | -0.356 | -0.345 | -0.399 |
| maker model | -0.199 | -0.196 | -0.180 | -0.064 |
| maker random | -0.215 | -0.211 | -0.197 | -0.271 |

Latency and size at the headline (gross bps; share of decisions where the order is larger than the best level on the side it takes from):

| latency | size | taker model | maker model | larger than best level |
|---|---|---|---|---|
| colocated | 100 | -0.406 ± 0.122 | -0.351 ± 0.126 | 2.5% |
| colocated | 200 | -0.450 ± 0.122 | -0.385 ± 0.126 | 9.9% |
| colocated | 500 | -0.588 ± 0.121 | -0.395 ± 0.127 | 30.4% |
| NJ, not colocated | 100 | -0.416 ± 0.121 | -0.337 ± 0.127 | 2.5% |

Queue model at the last run: {'passive_fills': 679, 'trade_through_fills': 506, 'behind_while_ahead': 16}; reference regressions 6502, the last at 34230.3 s after midnight

