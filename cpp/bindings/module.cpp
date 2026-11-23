// orderflow._core: the C++ core as a Python extension module (pybind11).
//
// Crossing the Python/C++ boundary costs a fixed amount per call (argument
// conversion, GIL bookkeeping), on the order of 100 ns, which is more than
// the C++ work of applying one update. So there are two ways in:
//   apply(side, price, size)               one update per call: simple, for tests
//   apply_many(is_bid, prices, sizes)      a whole message's updates in one call,
//                                          from NumPy arrays: the boundary is
//                                          crossed once per message, not per update
// scripts/bench_bindings.py measures both against the pure-Python book.
//
// The backtester is exposed the same way: Python hands over a whole
// run's inputs (signals as arrays, configuration as small structs) and gets
// whole result tables back as NumPy arrays. Per-event Python callbacks would
// cost far more than the simulation itself, so strategies run in C++, and
// the GIL is released while they do.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "orderflow/backtest/run.hpp"
#include "orderflow/infer.hpp"
#include "orderflow/l2_book.hpp"
#include "orderflow/mapped_file.hpp"
#include "orderflow/tsc.hpp"

namespace py = pybind11;
using orderflow::L2Book;
namespace bt = orderflow::bt;

namespace {

template <class T>
using Array = py::array_t<T, py::array::c_style | py::array::forcecast>;

template <class T>
py::array_t<T> to_numpy(const std::vector<T>& v) {
    py::array_t<T> out(static_cast<py::ssize_t>(v.size()));
    std::copy(v.begin(), v.end(), out.mutable_data());
    return out;
}

template <class T>
std::span<const T> view(const Array<T>& a) {
    return {a.data(), static_cast<std::size_t>(a.size())};
}

bt::StrategySpec make_spec(const Array<std::uint64_t>& index, const Array<std::int8_t>& direction, double qty,
                           std::string_view entry, std::int64_t hold_ns, std::uint64_t hold_events,
                           std::int64_t maker_timeout_ns, std::int64_t session_start, std::int64_t session_end) {
    if (index.size() != direction.size()) throw py::value_error("index and direction must have the same length");
    bt::StrategySpec s;
    s.params.qty = qty;
    if (entry == "taker") {
        s.params.entry = bt::Entry::taker;
    } else if (entry == "maker") {
        s.params.entry = bt::Entry::maker;
    } else {
        throw py::value_error("entry must be 'taker' or 'maker'");
    }
    s.params.hold_ns = hold_ns;
    s.params.hold_events = hold_events;
    s.params.maker_timeout_ns = maker_timeout_ns;
    s.params.session_start = session_start;
    s.params.session_end = session_end;
    const auto idx = view(index);
    const auto dir = view(direction);
    for (std::size_t i = 0; i < idx.size(); ++i) {
        if (dir[i] != 0) s.signals.emplace_back(idx[i], dir[i] > 0 ? 1 : -1);
    }
    return s;
}

py::dict trades_to_dict(const std::vector<bt::Trade>& trades) {
    const auto n = static_cast<py::ssize_t>(trades.size());
    Array<std::uint64_t> decision_index(n);
    Array<std::int64_t> decision_ts(n), decision_bid(n), decision_ask(n), decision_segment(n), entry_ts(n),
        exit_ts(n), entry_segment(n), exit_segment(n);
    Array<std::int8_t> dir(n);
    Array<double> qty(n), entry_px(n), exit_px(n), exit_qty(n), fees(n);
    Array<bool> entry_maker(n), complete(n);
    for (py::ssize_t i = 0; i < n; ++i) {
        const auto& t = trades[static_cast<std::size_t>(i)];
        decision_index.mutable_at(i) = t.decision_index;
        decision_ts.mutable_at(i) = t.decision_ts;
        decision_bid.mutable_at(i) = t.decision_bid;
        decision_ask.mutable_at(i) = t.decision_ask;
        decision_segment.mutable_at(i) = t.decision_segment;
        dir.mutable_at(i) = static_cast<std::int8_t>(t.dir);
        qty.mutable_at(i) = t.qty;
        exit_qty.mutable_at(i) = t.exit_qty;
        entry_px.mutable_at(i) = t.entry_px();
        exit_px.mutable_at(i) = t.exit_px();
        fees.mutable_at(i) = t.fees;
        entry_ts.mutable_at(i) = t.entry_ts;
        exit_ts.mutable_at(i) = t.exit_ts;
        entry_maker.mutable_at(i) = t.entry_maker;
        entry_segment.mutable_at(i) = t.entry_segment;
        exit_segment.mutable_at(i) = t.exit_segment;
        complete.mutable_at(i) = t.complete;
    }
    py::dict d;
    d["decision_index"] = decision_index;
    d["decision_ts"] = decision_ts;
    d["dir"] = dir;
    d["decision_bid"] = decision_bid;
    d["decision_ask"] = decision_ask;
    d["decision_segment"] = decision_segment;
    d["qty"] = qty;
    d["exit_qty"] = exit_qty;
    d["entry_px"] = entry_px;
    d["exit_px"] = exit_px;
    d["fees"] = fees;
    d["entry_ts"] = entry_ts;
    d["exit_ts"] = exit_ts;
    d["entry_maker"] = entry_maker;
    d["entry_segment"] = entry_segment;
    d["exit_segment"] = exit_segment;
    d["complete"] = complete;
    return d;
}

py::dict result_to_dict(const bt::RunResult& r) {
    py::list per_strategy;
    for (std::size_t i = 0; i < r.trades.size(); ++i) {
        const auto& s = r.stats[i];
        py::dict stats;
        stats["decisions"] = s.decisions;
        stats["skipped_busy"] = s.skipped_busy;
        stats["skipped_untradable"] = s.skipped_untradable;
        stats["entries_sent"] = s.entries_sent;
        stats["entries_unfilled"] = s.entries_unfilled;
        stats["exit_retries"] = s.exit_retries;
        py::dict one;
        one["trades"] = trades_to_dict(r.trades[i]);
        one["stats"] = stats;
        per_strategy.append(one);
    }
    py::dict queue;
    queue["passive_fills"] = r.queue.passive_fills;
    queue["trade_through_fills"] = r.queue.trade_through_fills;
    queue["behind_while_ahead"] = r.queue.behind_while_ahead;
    py::dict out;
    out["strategies"] = per_strategy;
    out["queue"] = queue;
    out["book_anomalies"] = r.book_anomalies;
    out["ref_regressions"] = r.ref_regressions;
    out["last_regression_ts"] = r.last_regression_ts;
    out["malformed"] = r.malformed;
    out["symbol_found"] = r.symbol_found;
    return out;
}

bool parse_side(std::string_view side) {
    if (side == "bid") return true;
    if (side == "ask") return false;
    throw py::value_error("side must be 'bid' or 'ask'");
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "C++ core of orderflow-lab: books and replay, exposed to Python.";

    py::class_<L2Book>(m, "L2Book", "Price-level book with BookState's semantics, in C++.")
        .def(py::init<>())
        .def(
            "apply",
            [](L2Book& b, std::string_view side, std::int64_t price_ticks, double size) {
                b.apply(parse_side(side), price_ticks, size);
            },
            py::arg("side"), py::arg("price_ticks"), py::arg("size"),
            "Set the size at a price; size 0 removes the level.")
        .def(
            "apply_many",
            [](L2Book& b, const py::array_t<bool, py::array::c_style | py::array::forcecast>& is_bid,
               const py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>& prices,
               const py::array_t<double, py::array::c_style | py::array::forcecast>& sizes) {
                const auto n = is_bid.size();
                if (prices.size() != n || sizes.size() != n) {
                    throw py::value_error("is_bid, prices and sizes must have the same length");
                }
                const bool* side = is_bid.data();
                const std::int64_t* px = prices.data();
                const double* sz = sizes.data();
                for (py::ssize_t i = 0; i < n; ++i) b.apply(side[i], px[i], sz[i]);
            },
            py::arg("is_bid"), py::arg("prices"), py::arg("sizes"),
            "Apply many updates in order, crossing into C++ once.")
        .def("clear", &L2Book::clear, "Remove every level (a snapshot starts a fresh book).")
        .def("best_bid", &L2Book::best_bid, "(price_ticks, size) of the best bid, or None.")
        .def("best_ask", &L2Book::best_ask, "(price_ticks, size) of the best ask, or None.")
        .def("bids", &L2Book::bids, "All bid levels, best first.")
        .def("asks", &L2Book::asks, "All ask levels, best first.")
        .def_property_readonly("bid_levels", &L2Book::bid_levels)
        .def_property_readonly("ask_levels", &L2Book::ask_levels);

    py::class_<bt::Fees>(m, "Fees", "Per fill: notional x bps / 1e4 + qty x per_unit; negative is a rebate.")
        .def(py::init([](double taker_bps, double maker_bps, double taker_per_unit, double maker_per_unit) {
                 return bt::Fees{taker_bps, maker_bps, taker_per_unit, maker_per_unit};
             }),
             py::kw_only(), py::arg("taker_bps") = 0.0, py::arg("maker_bps") = 0.0,
             py::arg("taker_per_unit") = 0.0, py::arg("maker_per_unit") = 0.0)
        .def_readonly("taker_bps", &bt::Fees::taker_bps)
        .def_readonly("maker_bps", &bt::Fees::maker_bps)
        .def_readonly("taker_per_unit", &bt::Fees::taker_per_unit)
        .def_readonly("maker_per_unit", &bt::Fees::maker_per_unit);

    py::class_<bt::Latency>(m, "Latency", "Market-data, decision and order-entry delays, in ns.")
        .def(py::init([](std::int64_t market_data_ns, std::int64_t decision_ns, std::int64_t order_entry_ns) {
                 return bt::Latency{market_data_ns, decision_ns, order_entry_ns};
             }),
             py::kw_only(), py::arg("market_data_ns") = 0, py::arg("decision_ns") = 0, py::arg("order_entry_ns") = 0)
        .def_readonly("market_data_ns", &bt::Latency::market_data_ns)
        .def_readonly("decision_ns", &bt::Latency::decision_ns)
        .def_readonly("order_entry_ns", &bt::Latency::order_entry_ns);

    py::class_<bt::StrategySpec>(m, "StrategySpec", "A round-trip strategy driven by precomputed signals.")
        .def(py::init(&make_spec), py::kw_only(), py::arg("index"), py::arg("direction"), py::arg("qty"),
             py::arg("entry"), py::arg("hold_ns") = 0, py::arg("hold_events") = 0,
             py::arg("maker_timeout_ns") = 1'000'000'000, py::arg("session_start") = INT64_MIN,
             py::arg("session_end") = INT64_MAX);

    m.def(
        "itch_top_of_book",
        [](const std::string& path, const std::string& symbol) {
            const orderflow::MappedFile file(path);
            bt::TopOfBookRows rows;
            {
                py::gil_scoped_release release;
                rows = bt::itch_top_of_book(file.bytes(), symbol);
            }
            if (!rows.symbol_found) throw py::value_error("symbol not in the file's stock directory: " + symbol);
            py::dict d;
            d["ts"] = to_numpy(rows.ts);
            d["bid_px"] = to_numpy(rows.bid_px);
            d["bid_sz"] = to_numpy(rows.bid_sz);
            d["ask_px"] = to_numpy(rows.ask_px);
            d["ask_sz"] = to_numpy(rows.ask_sz);
            d["tradable"] = to_numpy(rows.tradable);
            d["book_anomalies"] = rows.book_anomalies;
            return d;
        },
        py::arg("path"), py::arg("symbol"),
        "Top of book after every message that changed symbol's book (row i = event index i).");

    m.def(
        "backtest_itch",
        [](const std::string& path, const std::string& symbol, const std::vector<bt::StrategySpec>& specs,
           const bt::Fees& fees, const bt::Latency& latency) {
            const orderflow::MappedFile file(path);
            bt::RunResult r;
            {
                py::gil_scoped_release release;
                r = bt::run_itch(file.bytes(), symbol, specs, fees, latency);
            }
            if (!r.symbol_found) throw py::value_error("symbol not in the file's stock directory: " + symbol);
            return result_to_dict(r);
        },
        py::arg("path"), py::arg("symbol"), py::arg("strategies"), py::arg("fees"), py::arg("latency"),
        "Backtest strategies on one symbol of an ITCH file, with queue positions.");

    m.def(
        "backtest_top_of_book",
        [](const Array<std::int64_t>& ts, const Array<std::int64_t>& bid_px, const Array<double>& bid_sz,
           const Array<std::int64_t>& ask_px, const Array<double>& ask_sz, const Array<std::int64_t>& segment,
           double price_scale, const std::vector<bt::StrategySpec>& specs, const bt::Fees& fees,
           const bt::Latency& latency, bool batch_intervals) {
            const bt::TopOfBookMarket::Rows rows{view(ts), view(bid_px), view(bid_sz),
                                                 view(ask_px), view(ask_sz), view(segment)};
            bt::RunResult r;
            {
                py::gil_scoped_release release;
                r = bt::run_top_of_book(rows, price_scale, specs, fees, latency, batch_intervals);
            }
            return result_to_dict(r);
        },
        py::arg("ts"), py::arg("bid_px"), py::arg("bid_sz"), py::arg("ask_px"), py::arg("ask_sz"),
        py::arg("segment"), py::arg("price_scale"), py::arg("strategies"), py::arg("fees"), py::arg("latency"),
        py::arg("batch_intervals") = true,
        "Backtest taker strategies on a top-of-book table (one row per message).");

    py::class_<orderflow::infer::Model>(m, "InferenceModel", "The C++ evaluator for an exported model (infer.hpp).")
        .def(py::init(&orderflow::infer::Model::load_file), py::arg("path"))
        .def_property_readonly("inputs", &orderflow::infer::Model::inputs)
        .def_property_readonly("classes", &orderflow::infer::Model::classes)
        .def(
            "predict",
            [](const orderflow::infer::Model& model, const Array<double>& x, bool simd) {
                if (x.ndim() != 2 || static_cast<std::size_t>(x.shape(1)) != model.inputs()) {
                    throw py::value_error("x must have shape (n, inputs)");
                }
                const auto n = x.shape(0);
                Array<std::int64_t> cls(n);
                Array<double> prob(n);
                const double* p = x.data();
                for (py::ssize_t i = 0; i < n; ++i) {
                    const auto pr = simd ? model.predict_simd(p + i * x.shape(1)) : model.predict(p + i * x.shape(1));
                    cls.mutable_at(i) = pr.cls;
                    prob.mutable_at(i) = pr.probability;
                }
                return py::make_tuple(cls, prob);
            },
            py::arg("x"), py::arg("simd") = false, "Predicted class and its probability per row.");

    m.def(
        "backtest_itch_model",
        [](const std::string& path, const std::string& symbol, const std::string& model_path, std::string_view evaluator,
           const bt::StrategySpec& spec, std::int64_t usable_start, std::int64_t usable_end, const bt::Fees& fees,
           const bt::Latency& latency) {
            const orderflow::MappedFile file(path);
            const auto model = orderflow::infer::Model::load_file(model_path);
            if (evaluator != "scalar" && evaluator != "simd") throw py::value_error("evaluator must be scalar or simd");
            const auto ev = evaluator == "scalar" ? bt::ModelStrategy::Evaluator::scalar
                                                  : bt::ModelStrategy::Evaluator::simd;
            bt::ModelRun r;
            {
                py::gil_scoped_release release;
                r = bt::run_itch_model(file.bytes(), symbol, model, ev, spec.params, usable_start, usable_end, fees,
                                       latency);
            }
            if (!r.symbol_found) throw py::value_error("symbol not in the file's stock directory: " + symbol);
            Array<std::uint64_t> idx(static_cast<py::ssize_t>(r.decisions.size()));
            Array<std::int64_t> cls(static_cast<py::ssize_t>(r.decisions.size()));
            Array<double> prob(static_cast<py::ssize_t>(r.decisions.size()));
            for (std::size_t i = 0; i < r.decisions.size(); ++i) {
                const auto j = static_cast<py::ssize_t>(i);
                idx.mutable_at(j) = r.decisions[i].event_index;
                cls.mutable_at(j) = r.decisions[i].cls;
                prob.mutable_at(j) = r.decisions[i].probability;
            }
            bt::RunResult as_run;
            as_run.trades.push_back(r.trades);
            as_run.stats.push_back(r.stats);
            as_run.queue = r.queue;
            as_run.book_anomalies = r.book_anomalies;
            as_run.ref_regressions = r.ref_regressions;
            as_run.last_regression_ts = r.last_regression_ts;
            py::dict out = result_to_dict(as_run);
            py::dict decisions;
            decisions["event_index"] = idx;
            decisions["cls"] = cls;
            decisions["probability"] = prob;
            out["decisions"] = decisions;
            out["decision_ticks"] = to_numpy(r.decision_ticks);
            return out;
        },
        py::arg("path"), py::arg("symbol"), py::arg("model_path"), py::arg("evaluator"), py::arg("strategy"),
        py::arg("usable_start"), py::arg("usable_end"), py::arg("fees"), py::arg("latency"),
        "Backtest the exported model deciding online inside the engine (its signals in strategy are ignored).");

    m.def("tsc_per_ns", &orderflow::tsc_per_ns, py::arg("ms") = 200,
          "TSC ticks per nanosecond, measured against steady_clock (to convert decision_ticks).");
}
