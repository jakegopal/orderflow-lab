// C++ evaluation of a model exported by orderflow.model.export.
//
// The model is a StandardScaler followed by dense layers: ReLU hidden layers
// (none for logistic regression) and a linear output layer; the prediction is
// the argmax class and its softmax probability.
//
// File format, whitespace-separated text, numbers with 17 significant digits:
//   orderflow-model 1
//   inputs <n>
//   mean <n numbers>
//   scale <n numbers>
//   classes <k> <k integers>
//   layers <L>
//   per layer: dense <in> <out> <relu|linear> <out*in weights, row-major> <out biases>
//
// predict() is double precision and matches scikit-learn to rounding.
// predict_simd() is single precision with AVX2 FMA, 8 outputs per
// instruction, used when the CPU supports it. Both use scratch buffers owned
// by the model, so a Model must not be shared between threads.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace orderflow::infer {

struct Dense {
    std::size_t in = 0, out = 0;
    bool relu = false;
    std::vector<double> w;   // out x in, row-major (scikit-learn's coef_ layout)
    std::vector<double> b;
    std::vector<float> wt;   // in x out, column-major, padded to a multiple of 8 outputs, for SIMD
    std::vector<float> bf;
    std::size_t out_padded = 0;
};

struct Prediction {
    int cls = 0;
    double probability = 0;
};

class Model {
public:
    static constexpr std::size_t kMaxWidth = 256;

    static Model load(std::istream& in) {
        auto expect = [&](const char* word) {
            std::string got;
            if (!(in >> got) || got != word) throw std::runtime_error(std::string("model file: expected ") + word);
        };
        Model m;
        int version = 0;
        expect("orderflow-model");
        if (!(in >> version) || version != 1) throw std::runtime_error("model file: unsupported version");
        expect("inputs");
        in >> m.inputs_;
        if (!in || m.inputs_ == 0 || m.inputs_ > kMaxWidth) throw std::runtime_error("model file: bad input count");
        m.mean_.resize(m.inputs_);
        m.inv_scale_.resize(m.inputs_);
        expect("mean");
        for (auto& v : m.mean_) in >> v;
        expect("scale");
        for (auto& v : m.inv_scale_) {
            in >> v;
            if (!(v > 0)) throw std::runtime_error("model file: scale must be positive");
            v = 1.0 / v;
        }
        expect("classes");
        std::size_t k = 0;
        in >> k;
        m.classes_.resize(k);
        for (auto& c : m.classes_) in >> c;
        expect("layers");
        std::size_t layers = 0;
        in >> layers;
        std::size_t width = m.inputs_;
        for (std::size_t l = 0; l < layers; ++l) {
            Dense d;
            std::string act;
            expect("dense");
            in >> d.in >> d.out >> act;
            if (!in || d.in != width || d.out == 0 || d.out > kMaxWidth) throw std::runtime_error("model file: bad layer shape");
            if (act != "relu" && act != "linear") throw std::runtime_error("model file: bad activation " + act);
            d.relu = act == "relu";
            d.w.resize(d.in * d.out);
            d.b.resize(d.out);
            for (auto& v : d.w) in >> v;
            for (auto& v : d.b) in >> v;
            width = d.out;
            m.layers_.push_back(std::move(d));
        }
        if (!in || layers == 0 || width != k) throw std::runtime_error("model file: output width != class count");
        if (m.layers_.back().relu) throw std::runtime_error("model file: the output layer must be linear");
        m.prepare_simd();
        return m;
    }

    static Model load_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("cannot open " + path);
        return load(f);
    }

    [[nodiscard]] std::size_t inputs() const noexcept { return inputs_; }
    [[nodiscard]] const std::vector<int>& classes() const noexcept { return classes_; }

    // Reference evaluator, double precision. x has inputs() values.
    // Not thread-safe: it uses the model's scratch buffers (one model per
    // thread), so a call does no allocation and no clearing.
    [[nodiscard]] Prediction predict(const double* x) const noexcept {
        double* a = scratch_a_.data();
        double* b = scratch_b_.data();
        for (std::size_t i = 0; i < inputs_; ++i) a[i] = (x[i] - mean_[i]) * inv_scale_[i];
        std::size_t width = inputs_;
        for (const Dense& d : layers_) {
            for (std::size_t o = 0; o < d.out; ++o) {
                double s = d.b[o];
                const double* row = &d.w[o * d.in];
                for (std::size_t i = 0; i < d.in; ++i) s += row[i] * a[i];
                b[o] = d.relu ? std::max(0.0, s) : s;
            }
            std::swap(a, b);  // pointers: the output becomes the next layer's input
            width = d.out;
        }
        return finish(a, width);
    }

    // Single precision, AVX2/FMA where the CPU has it (checked once at load).
    [[nodiscard]] Prediction predict_simd(const double* x) const noexcept {
#if defined(__x86_64__)
        if (has_avx2_) return predict_avx2(x);
#endif
        return predict(x);
    }

private:
    [[nodiscard]] Prediction finish(const double* logits, std::size_t k) const noexcept {
        std::size_t best = 0;
        for (std::size_t c = 1; c < k; ++c) {
            if (logits[c] > logits[best]) best = c;
        }
        // softmax(best) = 1 / sum_c exp(l_c - l_best), stable because every exponent is <= 0.
        double denom = 0;
        for (std::size_t c = 0; c < k; ++c) denom += std::exp(logits[c] - logits[best]);
        return {classes_[best], 1.0 / denom};
    }

    void prepare_simd() {
        for (Dense& d : layers_) {
            d.out_padded = (d.out + 7) / 8 * 8;
            d.wt.assign(d.in * d.out_padded, 0.0f);
            d.bf.assign(d.out_padded, 0.0f);
            for (std::size_t o = 0; o < d.out; ++o) {
                for (std::size_t i = 0; i < d.in; ++i) d.wt[i * d.out_padded + o] = static_cast<float>(d.w[o * d.in + i]);
                d.bf[o] = static_cast<float>(d.b[o]);
            }
        }
#if defined(__x86_64__)
        has_avx2_ = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
        // Scratch for activations: the widest layer, padded for 8-wide stores.
        std::size_t widest = inputs_;
        for (const Dense& d : layers_) widest = std::max(widest, d.out_padded);
        scratch_a_.assign(widest, 0.0);
        scratch_b_.assign(widest, 0.0);
        scratch_fa_.assign(widest, 0.0f);
        scratch_fb_.assign(widest, 0.0f);
    }

#if defined(__x86_64__)
    // Each layer: out[0..8) += x[i] * W[i][0..8) for every input i, one FMA per
    // 8 outputs. The weights are stored input-major so those 8 are contiguous.
    [[nodiscard]] __attribute__((target("avx2,fma"))) Prediction predict_avx2(const double* x) const noexcept {
        float* a = scratch_fa_.data();
        float* b = scratch_fb_.data();
        for (std::size_t i = 0; i < inputs_; ++i) a[i] = static_cast<float>((x[i] - mean_[i]) * inv_scale_[i]);
        const __m256 zero = _mm256_setzero_ps();
        for (const Dense& d : layers_) {
            for (std::size_t o = 0; o < d.out_padded; o += 8) {
                __m256 acc = _mm256_loadu_ps(&d.bf[o]);
                for (std::size_t i = 0; i < d.in; ++i) {
                    acc = _mm256_fmadd_ps(_mm256_set1_ps(a[i]), _mm256_loadu_ps(&d.wt[i * d.out_padded + o]), acc);
                }
                if (d.relu) acc = _mm256_max_ps(acc, zero);
                _mm256_storeu_ps(&b[o], acc);
            }
            std::swap(a, b);
        }
        const std::size_t k = classes_.size();
        double* logits = scratch_a_.data();
        for (std::size_t c = 0; c < k; ++c) logits[c] = a[c];
        return finish(logits, k);
    }
#endif

    std::size_t inputs_ = 0;
    std::vector<double> mean_, inv_scale_;
    std::vector<int> classes_;
    std::vector<Dense> layers_;
    bool has_avx2_ = false;
    mutable std::vector<double> scratch_a_, scratch_b_;
    mutable std::vector<float> scratch_fa_, scratch_fb_;
};

// The model's inputs computed online from consecutive tops of book, the C++
// twin of features/microstructure.py (imbalance, OFI, spread, in that order:
// model.baseline.MODEL_INPUTS). prev is the previous usable top in the same
// segment, or none.
struct Top {
    double bid_px, bid_sz, ask_px, ask_sz;
};

[[nodiscard]] inline std::array<double, 3> features(const Top& cur, const Top& prev) noexcept {
    const double total = cur.bid_sz + cur.ask_sz;
    const double imbalance = total == 0 ? 0.0 : (cur.bid_sz - cur.ask_sz) / total;
    const double ofi = cur.bid_sz * (cur.bid_px >= prev.bid_px) - prev.bid_sz * (cur.bid_px <= prev.bid_px) -
                       cur.ask_sz * (cur.ask_px <= prev.ask_px) + prev.ask_sz * (cur.ask_px >= prev.ask_px);
    return {imbalance, ofi, cur.ask_px - cur.bid_px};
}

}  // namespace orderflow::infer
