// The research model running inside the backtester.
//
// SignalStrategy replays decisions that Python computed offline. ModelStrategy
// makes them online, as a live system would: on every book it computes the
// features from this top and the previous one, standardises them, evaluates
// the model, and trades the predicted direction (FLAT means no trade). Each
// decision's compute time is measured with the TSC, so the latency the model
// adds is a measured number, not an assumption.
//
// It reproduces the Python pipeline's rules exactly, so the two can be
// checked decision for decision:
//   - a book is usable only if tradable, two-sided, not crossed, and inside
//     the regular session [usable_start, usable_end)
//   - OFI needs the previous book, and only a previous *usable* book with the
//     immediately preceding event index counts (Python starts a new segment
//     at every unusable state, and OFI is NaN on a segment's first row)
// One intended difference: Python can only score rows whose label (h
// messages ahead) exists, so it has no decisions in the last h rows of each
// segment. A live model can't know it is near the end of a segment, so it
// decides there too. Parity is checked on the rows both have.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "orderflow/backtest/round_trip.hpp"
#include "orderflow/infer.hpp"
#include "orderflow/tsc.hpp"

namespace orderflow::bt {

struct Decision {
    std::uint64_t event_index;
    int cls;
    double probability;
};

class ModelStrategy final : public RoundTripStrategy {
public:
    enum class Evaluator : std::uint8_t { scalar, simd };

    ModelStrategy(RoundTripParams p, const infer::Model& model, Evaluator ev, Time usable_start, Time usable_end,
                  bool record_decisions)
        : RoundTripStrategy(p),
          model_(model),
          ev_(ev),
          usable_start_(usable_start),
          usable_end_(usable_end),
          record_(record_decisions) {}

    [[nodiscard]] const std::vector<Decision>& decisions() const noexcept { return decisions_; }
    // TSC ticks per decision: features + standardise + model + argmax.
    [[nodiscard]] const std::vector<std::uint32_t>& decision_ticks() const noexcept { return ticks_; }

protected:
    int decide(const BookSnapshot& b) override {
        const bool usable = b.tradable && b.two_sided() && b.bids[0].price < b.asks[0].price &&
                            b.exchange_ts >= usable_start_ && b.exchange_ts < usable_end_;
        if (!usable) {
            have_prev_ = false;
            return 0;
        }
        const infer::Top cur{static_cast<double>(b.bids[0].price), b.bids[0].qty,
                             static_cast<double>(b.asks[0].price), b.asks[0].qty};
        const bool continues = have_prev_ && b.event_index == prev_index_ + 1;
        const infer::Top prev = prev_;
        prev_ = cur;
        prev_index_ = b.event_index;
        have_prev_ = true;
        if (!continues) return 0;  // OFI undefined on a segment's first row

        const std::uint64_t t0 = tsc_start();
        const auto x = infer::features(cur, prev);
        const infer::Prediction pr = ev_ == Evaluator::scalar ? model_.predict(x.data()) : model_.predict_simd(x.data());
        const std::uint64_t t1 = tsc_stop();
        ticks_.push_back(static_cast<std::uint32_t>(std::min<std::uint64_t>(t1 - t0, UINT32_MAX)));
        if (record_) decisions_.push_back({b.event_index, pr.cls, pr.probability});
        return pr.cls;  // classes are -1, 0, +1: FLAT is 0, so it means no trade
    }

private:
    const infer::Model& model_;
    Evaluator ev_;
    Time usable_start_, usable_end_;
    bool record_;
    bool have_prev_ = false;
    infer::Top prev_{};
    std::uint64_t prev_index_ = 0;
    std::vector<Decision> decisions_;
    std::vector<std::uint32_t> ticks_;
};

}  // namespace orderflow::bt
