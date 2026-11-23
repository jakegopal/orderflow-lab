## AAPL

1,656,597 book messages, 1,586,525 usable rows in regular hours; spread is 1 cent on 20.9% of rows; median best-level size 295 bid / 300 ask shares

Horizon curve, colocated, 100 shares, gross bps per round trip (round trips):

| horizon | FLAT share (first block) | accuracy / majority | taker model | taker random | maker model | maker random | maker fill rate |
|---|---|---|---|---|---|---|---|
| 1 s | 0.735 | 0.892 / 0.892 | -0.547 ± 0.747 (10) | -2.711 ± 0.611 (10) | -1.534 ± 0.638 (4) | -2.390 ± 0.516 (8) | 40% |
| 2 s | 0.580 | 0.803 / 0.803 | -1.113 ± 0.239 (101) | -0.931 ± 0.228 (101) | -1.554 ± 0.350 (30) | -0.574 ± 0.306 (40) | 29% |
| 5 s | 0.369 | 0.591 / 0.540 | -1.667 ± 0.116 (701) | -1.818 ± 0.118 (701) | -1.457 ± 0.159 (387) | -1.353 ± 0.144 (418) | 26% |
| 10 s | 0.350 | 0.396 / 0.427 | -1.195 ± 0.083 (1,271) | -1.297 ± 0.083 (1,271) | -1.362 ± 0.113 (788) | -1.208 ± 0.102 (848) | 20% |
| **30 s** | 0.237 | 0.394 / 0.400 | -1.286 ± 0.211 (718) | -1.008 ± 0.211 (718) | -1.245 ± 0.242 (606) | -1.654 ± 0.240 (617) | 18% |
| 60 s | 0.065 | 0.411 / 0.369 | -0.969 ± 0.434 (360) | -0.953 ± 0.435 (360) | -1.721 ± 0.443 (330) | -1.945 ± 0.449 (334) | 18% |

Headline horizon 30 s (shortest with first-block FLAT share <= 1/3).

Net bps per round trip at the headline, colocated, 100 shares (take $0.0030 on every taker leg):

| strategy | round trips | gross | net, no rebate | net, default $0.0015 | net, top tier $0.00305 |
|---|---|---|---|---|---|
| taker model | 718 | -1.286 ± 0.211 | -1.653 ± 0.211 | – | – |
| taker random | 718 | -1.008 ± 0.211 | -1.376 ± 0.211 | – | – |
| maker model | 606 | -1.245 ± 0.242 | -1.429 ± 0.242 | -1.338 ± 0.242 | -1.243 ± 0.242 |
| maker random | 617 | -1.654 ± 0.240 | -1.837 ± 0.240 | -1.746 ± 0.240 | -1.651 ± 0.240 |

Entry markouts at the headline (bps, positive = in our favour):

| strategy | 0 s | 0.1 s | 1 s | 10 s |
|---|---|---|---|---|
| taker model | -0.724 | -0.722 | -0.754 | -0.682 |
| taker random | -0.705 | -0.697 | -0.727 | -0.626 |
| maker model | -0.126 | -0.323 | -0.343 | -0.612 |
| maker random | -0.141 | -0.264 | -0.323 | -0.713 |

Latency and size at the headline (gross bps; share of decisions where the order is larger than the best level on the side it takes from):

| latency | size | taker model | maker model | larger than best level |
|---|---|---|---|---|
| colocated | 100 | -1.286 ± 0.211 | -1.245 ± 0.242 | 11.9% |
| colocated | 200 | -1.465 ± 0.210 | -1.250 ± 0.241 | 38.1% |
| colocated | 500 | -1.836 ± 0.211 | -1.214 ± 0.236 | 69.2% |
| NJ, not colocated | 100 | -1.312 ± 0.211 | -1.193 ± 0.244 | 11.9% |

Queue model at the last run: {'passive_fills': 731, 'trade_through_fills': 393, 'behind_while_ahead': 2}; reference regressions 37981, the last at 34231.7 s after midnight

