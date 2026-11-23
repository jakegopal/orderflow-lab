## MSFT

1,642,525 book messages, 1,569,454 usable rows in regular hours; spread is 1 cent on 79.4% of rows; median best-level size 540 bid / 600 ask shares

Horizon curve, colocated, 100 shares, gross bps per round trip (round trips):

| horizon | FLAT share (first block) | accuracy / majority | taker model | taker random | maker model | maker random | maker fill rate |
|---|---|---|---|---|---|---|---|
| 1 s | 0.883 | 0.922 / 0.923 | -0.533 ± 0.123 (49) | -1.247 ± 0.145 (49) | -0.135 ± 0.246 (7) | -1.088 ± 0.254 (18) | 15% |
| 2 s | 0.777 | 0.863 / 0.863 | -1.243 ± 0.470 (50) | -1.902 ± 0.425 (50) | +0.429 ± 0.481 (8) | -0.858 ± 0.358 (21) | 15% |
| 5 s | 0.604 | 0.735 / 0.735 | -0.406 ± 0.188 (70) | -1.256 ± 0.221 (70) | -0.526 ± 0.825 (8) | -1.541 ± 0.493 (20) | 10% |
| 10 s | 0.491 | 0.600 / 0.600 | -0.552 ± 0.163 (164) | -0.920 ± 0.169 (164) | -0.070 ± 0.543 (24) | -0.968 ± 0.266 (77) | 10% |
| **30 s** | 0.263 | 0.335 / 0.321 | -0.883 ± 0.152 (645) | -0.946 ± 0.151 (645) | -0.653 ± 0.171 (451) | -0.906 ± 0.177 (502) | 11% |
| 60 s | 0.203 | 0.407 / 0.409 | -0.518 ± 0.284 (335) | -0.708 ± 0.285 (335) | -0.216 ± 0.324 (292) | -1.231 ± 0.307 (296) | 12% |

Headline horizon 30 s (shortest with first-block FLAT share <= 1/3).

Net bps per round trip at the headline, colocated, 100 shares (take $0.0030 on every taker leg):

| strategy | round trips | gross | net, no rebate | net, default $0.0015 | net, top tier $0.00305 |
|---|---|---|---|---|---|
| taker model | 645 | -0.883 ± 0.152 | -1.452 ± 0.152 | – | – |
| taker random | 645 | -0.946 ± 0.151 | -1.515 ± 0.151 | – | – |
| maker model | 451 | -0.653 ± 0.171 | -0.938 ± 0.171 | -0.795 ± 0.171 | -0.648 ± 0.171 |
| maker random | 502 | -0.906 ± 0.177 | -1.194 ± 0.177 | -1.054 ± 0.177 | -0.908 ± 0.177 |

Entry markouts at the headline (bps, positive = in our favour):

| strategy | 0 s | 0.1 s | 1 s | 10 s |
|---|---|---|---|---|
| taker model | -0.568 | -0.459 | -0.441 | -0.364 |
| taker random | -0.553 | -0.568 | -0.515 | -0.447 |
| maker model | -0.120 | -0.261 | -0.310 | -0.200 |
| maker random | -0.210 | -0.345 | -0.324 | -0.484 |

Latency and size at the headline (gross bps; share of decisions where the order is larger than the best level on the side it takes from):

| latency | size | taker model | maker model | larger than best level |
|---|---|---|---|---|
| colocated | 100 | -0.883 ± 0.152 | -0.653 ± 0.171 | 4.4% |
| colocated | 200 | -0.960 ± 0.152 | -0.650 ± 0.172 | 15.7% |
| colocated | 500 | -1.242 ± 0.151 | -0.745 ± 0.171 | 48.5% |
| NJ, not colocated | 100 | -0.895 ± 0.151 | -0.608 ± 0.174 | 4.4% |

Queue model at the last run: {'passive_fills': 527, 'trade_through_fills': 264, 'behind_while_ahead': 41}; reference regressions 19799, the last at 34231.1 s after midnight

