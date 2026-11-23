# Backtest settings and where they come from

These values are set in [phase3.py](../../scripts/phase3.py) and
[phase5.py](../../scripts/phase5.py). They were fixed before the results
below them were computed, and each section says what isn't verified.

## Fees

| | value | source |
|---|---|---|
| Nasdaq, remove liquidity, Jan 2019 | $0.0030/share (stocks at $1 or more) | Nasdaq price list archived [8 Jan](http://web.archive.org/web/20190108211435/http://www.nasdaqtrader.com:80/Trader.aspx?id=PriceListTrading2) and [22 Jan 2019](http://web.archive.org/web/20190122231632/http://nasdaqtrader.com:80/Trader.aspx?id=PriceListTrading2); [Federal Register 2019-01393](https://www.federalregister.gov/documents/2019/02/07/2019-01393/self-regulatory-organizations-the-nasdaq-stock-market-llc-notice-of-filing-and-immediate) |
| Nasdaq, add displayed liquidity, Tape C (Nasdaq-listed: AAPL, MSFT, QQQ) | default credit $0.0015/share; $0 and the top tier $0.00305 as the range | same price list |
| Coinbase Advanced, taker | 90 bps (US entry tier), 2 bps (top tier) | schedule in force since 16 Sep 2026, as reported by [Investing.com](https://www.investing.com/news/cryptocurrency-news/coinbase-cuts-trading-fees-for-active-users-on-advanced-platform-432SI-4904040) and [CryptoDaily](https://cryptodaily.co.uk/2026/09/coinbase-advanced-fees-usdc-vip-tiers); Coinbase's [help page](http://web.archive.org/web/20261020152744/https://help.coinbase.com/en/coinbase/trading-and-funding/advanced-trade/advanced-trade-fees) shows the tier table only after login |

The rule at the time was Equity 7, Section 118(a), formerly Rule 7018(a).
Lower take fees existed only for qualified market makers on non-Nasdaq-listed
stocks, so they don't apply here. Not verified: the rulebook text itself (the
price list says it may be abbreviated) and the full post-change Coinbase US
tier table. The captures from before 16 Sep 2026 were recorded when the US
entry taker fee was reportedly 120 bps, so 90 bps is the more favourable
assumption for them.

## Latency

| profile | market data | decision | order entry | basis |
|---|---|---|---|---|
| Nasdaq, colocated | 20 µs | 1 µs | 20 µs | Nasdaq quotes under 50 µs round trip, order to acknowledgement, measured at the customer port ([colocation](https://nasdaqtrader.com/trader.aspx?id=colo)) |
| Nasdaq, another NJ data centre | 170 µs | 1 µs | 170 µs | about 150 µs more each way over fibre to Carteret |
| Coinbase, AWS us-east-1 | 10 ms | 0 | 10 ms | the exchange runs in AWS us-east-1 ([Coinbase docs](https://docs.cdp.coinbase.com/exchange/introduction/systems-operations)); one third-party measurement of public REST requests from a US-east data centre gave p50 13.6 ms round trip |
| Coinbase, home connection | 25 ms | 0 | 25 ms | an estimate |

Coinbase's level2 feed adds its own delay: it is published in batches about
50 ms apart (the spacing in my recordings; Coinbase documents 50 ms only for
its older Exchange channel). How the engine handles that is described in
[01-engine.md](01-engine.md). Not verified: none of the Nasdaq figures is
dated to 2019, the 1 µs decision time is an assumption (Phase 5 measures the
model at 20-1,500 ns), and no Coinbase order round trip was measured.

## Labels and horizons

The label asks whether a trade across the spread would have paid before fees:
UP if the best bid at t + h is above the best ask at t, DOWN if the best ask
at t + h is below the best bid at t, FLAT otherwise. Using the spread rather
than a mid-price threshold ties the label to prices that can actually be
traded. The deep order-book literature usually labels smoothed mid-price
moves against a threshold picked to balance the classes:
[DeepLOB](https://ar5iv.labs.arxiv.org/html/1808.03668) and its
[follow-up](https://ar5iv.labs.arxiv.org/html/2105.10430), the
[FI-2010 benchmark](https://ar5iv.labs.arxiv.org/html/1705.03233),
[LOBCAST](https://arxiv.org/html/2308.01915).

- ITCH horizons are in clock time, the same grid for every symbol:
  1, 2, 5, 10, 30 and 60 s. Message counts span very different times on
  different stocks, so they aren't comparable across symbols.
- Every horizon is reported. The headline is the shortest horizon whose FLAT
  share is at most 1/3, measured on the first walk-forward block only, so the
  choice uses no test data. It is 30 s for AAPL, MSFT and QQQ on 01302019.
- Coinbase keeps 100 messages (about 5 s, since batches come every ~50 ms);
  its first-block FLAT share is 16%.
- The purge window is the largest number of rows any label looks ahead.

[Kolm, Turiel and Westray (2023)](https://ideas.repec.org/a/bla/mathfi/v33y2023i4p1044-1081.html)
find stock-level order-flow predictability peaking within a couple of price
changes, which is much shorter than 30 s. The horizon curve in the results is
there so that a short-horizon edge would show up if it existed.

## Order sizes

Headline sizes are 100 shares and 0.01 BTC (about $770), with sweeps over
100/200/500 shares and 0.001/0.01/0.1 BTC. Measured on the data:

| market | median best-level size | best level smaller than the headline order |
|---|---|---|
| AAPL (Nasdaq book) | 295 bid / 300 ask shares | 11.9% of decisions |
| MSFT | 540 / 600 | 4.4% |
| QQQ | 900 / 840 | 2.5% |
| BTC-USD | | 34% of rows below 0.01 BTC |

There is no standard threshold below which market impact is negligible.
Backtesters such as [hftbacktest](https://hftbacktest.readthedocs.io/en/latest/order_fill.html)
make the same no-impact assumption and warn against large aggressive orders.
