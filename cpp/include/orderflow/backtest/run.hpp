// One call per backtest: data + strategies in, trades and diagnostics out.
//
// This is the surface the Python bindings and the command-line tool share,
// in plain structs, so neither has to know the engine's template types.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "orderflow/backtest/engine.hpp"
#include "orderflow/backtest/markets.hpp"
#include "orderflow/backtest/model_strategy.hpp"
#include "orderflow/backtest/round_trip.hpp"

namespace orderflow::bt {

struct StrategySpec {
    RoundTripParams params;
    std::vector<std::pair<std::uint64_t, int>> signals;  // (event index, +1 / -1), increasing
};

struct RunResult {
    std::vector<std::vector<Trade>> trades;  // per strategy, in order
    std::vector<RoundTripStats> stats;
    QueueStats queue;
    std::uint64_t book_anomalies = 0;   // ITCH only: must be 0
    std::uint64_t ref_regressions = 0;  // ITCH only: references not increasing, for the symbol
    Time last_regression_ts = -1;       // ... and when the last one happened (-1: never)
    std::uint64_t malformed = 0;
    bool symbol_found = true;
};

template <class Market>
RunResult run_on(Market& market, const std::vector<StrategySpec>& specs, Fees fees, Latency latency) {
    std::vector<SignalStrategy> strategies;
    strategies.reserve(specs.size());  // the engine keeps pointers: no reallocation after add()
    for (const auto& s : specs) strategies.emplace_back(s.params, s.signals);
    Engine<Market> engine(market, fees, latency);
    for (auto& s : strategies) engine.add(s);
    engine.run();
    RunResult r;
    for (const auto& s : strategies) {
        r.trades.push_back(s.trades());
        r.stats.push_back(s.stats());
    }
    r.queue = engine.queue_stats();
    return r;
}

inline RunResult run_itch(std::span<const std::byte> data, std::string_view symbol,
                          const std::vector<StrategySpec>& specs, Fees fees, Latency latency) {
    ItchMarket<FlatOpenBook> market(data, symbol);
    RunResult r = run_on(market, specs, fees, latency);
    r.book_anomalies = market.book().anomalies().total();
    r.ref_regressions = market.ref_regressions();
    r.last_regression_ts = market.last_regression_ts();
    r.malformed = market.malformed();
    r.symbol_found = market.locate().has_value();
    return r;
}

inline RunResult run_top_of_book(TopOfBookMarket::Rows rows, double price_scale,
                                 const std::vector<StrategySpec>& specs, Fees fees, Latency latency,
                                 bool batch_intervals = true) {
    TopOfBookMarket market(rows, price_scale, batch_intervals);
    return run_on(market, specs, fees, latency);
}

// The model deciding online inside the engine.
struct ModelRun {
    std::vector<Trade> trades;
    RoundTripStats stats;
    std::vector<Decision> decisions;
    std::vector<std::uint32_t> decision_ticks;
    QueueStats queue;
    std::uint64_t book_anomalies = 0;
    std::uint64_t ref_regressions = 0;
    Time last_regression_ts = -1;
    bool symbol_found = true;
};

inline ModelRun run_itch_model(std::span<const std::byte> data, std::string_view symbol, const infer::Model& model,
                               ModelStrategy::Evaluator ev, RoundTripParams params, Time usable_start,
                               Time usable_end, Fees fees, Latency latency) {
    ItchMarket<FlatOpenBook> market(data, symbol);
    ModelStrategy strategy(params, model, ev, usable_start, usable_end, true);
    Engine<ItchMarket<FlatOpenBook>> engine(market, fees, latency);
    engine.add(strategy);
    engine.run();
    ModelRun r;
    r.trades = strategy.trades();
    r.stats = strategy.stats();
    r.decisions = strategy.decisions();
    r.decision_ticks = strategy.decision_ticks();
    r.queue = engine.queue_stats();
    r.book_anomalies = market.book().anomalies().total();
    r.ref_regressions = market.ref_regressions();
    r.last_regression_ts = market.last_regression_ts();
    r.symbol_found = market.locate().has_value();
    return r;
}

// The top of book after every message that changed `symbol`'s book: the
// rows the Python research pipeline computes ITCH features from. Row i has
// event index i, the same numbering the backtest's signals use.
struct TopOfBookRows {
    std::vector<Time> ts;
    std::vector<Px> bid_px, ask_px;  // 0 when that side is empty
    std::vector<double> bid_sz, ask_sz;
    std::vector<std::uint8_t> tradable;
    std::uint64_t book_anomalies = 0;
    bool symbol_found = false;
};

inline TopOfBookRows itch_top_of_book(std::span<const std::byte> data, std::string_view symbol) {
    struct NoHooks {
        void on_reduce(std::uint64_t, Dir, Px, double, bool) noexcept {}
    } hooks;
    ItchMarket<FlatOpenBook> market(data, symbol);
    TopOfBookRows out;
    Time t = 0;
    BookSnapshot s;
    while (market.next_ts(t)) {
        if (!market.step(hooks)) continue;
        market.snapshot(s);
        out.ts.push_back(s.exchange_ts);
        out.bid_px.push_back(s.n_bids ? s.bids[0].price : 0);
        out.bid_sz.push_back(s.n_bids ? s.bids[0].qty : 0);
        out.ask_px.push_back(s.n_asks ? s.asks[0].price : 0);
        out.ask_sz.push_back(s.n_asks ? s.asks[0].qty : 0);
        out.tradable.push_back(s.tradable ? 1 : 0);
    }
    out.book_anomalies = market.book().anomalies().total();
    out.symbol_found = market.locate().has_value();
    return out;
}

}  // namespace orderflow::bt
