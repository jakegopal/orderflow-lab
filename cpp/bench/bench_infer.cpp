// bench_infer: per-decision cost of the hand-written evaluator against ONNX Runtime,
// both called from C++, one row per call.
//
//   bench_infer <model.txt> <model.onnx> <rows.f64>
//
// model.txt is the infer.hpp export and model.onnx the same fitted pipeline converted
// by skl2onnx (scripts/phase5.py writes both, plus rows.f64: n x inputs doubles,
// row-major, raw). Every row is evaluated by each method and timed with serialised
// TSC reads. ONNX Runtime runs single-threaded with sequential execution and
// preallocated input and output tensors, its fastest setup for one small row.
// The predicted class must agree between the two; the run fails if it doesn't
// on more than 0.1% of rows (float32 inside ONNX Runtime can flip near-ties).
#include <onnxruntime_c_api.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "orderflow/infer.hpp"
#include "orderflow/tsc.hpp"

namespace {

using orderflow::infer::Model;

const OrtApi* g_ort = nullptr;

void check(OrtStatus* status) {
    if (status == nullptr) return;
    const std::string msg = g_ort->GetErrorMessage(status);
    g_ort->ReleaseStatus(status);
    throw std::runtime_error("onnxruntime: " + msg);
}

std::vector<double> read_rows(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    const auto bytes = static_cast<std::size_t>(f.tellg());
    std::vector<double> v(bytes / sizeof(double));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(double)));
    return v;
}

void report(const char* name, std::vector<std::uint64_t>& ticks, double per_ns) {
    std::sort(ticks.begin(), ticks.end());
    auto q = [&](double p) {
        return static_cast<double>(ticks[static_cast<std::size_t>(p * static_cast<double>(ticks.size() - 1))]) / per_ns;
    };
    std::printf("  %-28s p50 %8.1f  p99 %8.1f  p99.9 %9.1f ns  (n=%zu)\n", name, q(0.5), q(0.99), q(0.999),
                ticks.size());
}

// ONNX Runtime with one preallocated input row and preallocated outputs.
class OrtRunner {
public:
    OrtRunner(const std::string& path, std::size_t inputs) : in_(inputs) {
        check(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "bench_infer", &env_));
        OrtSessionOptions* opts = nullptr;
        check(g_ort->CreateSessionOptions(&opts));
        check(g_ort->SetIntraOpNumThreads(opts, 1));
        check(g_ort->SetInterOpNumThreads(opts, 1));
        check(g_ort->SetSessionExecutionMode(opts, ORT_SEQUENTIAL));
        check(g_ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL));
        check(g_ort->CreateSession(env_, path.c_str(), opts, &session_));
        g_ort->ReleaseSessionOptions(opts);

        OrtAllocator* alloc = nullptr;
        check(g_ort->GetAllocatorWithDefaultOptions(&alloc));
        char* name = nullptr;
        check(g_ort->SessionGetInputName(session_, 0, alloc, &name));
        input_name_ = name;
        check(g_ort->AllocatorFree(alloc, name));
        std::size_t outputs = 0;
        check(g_ort->SessionGetOutputCount(session_, &outputs));
        for (std::size_t i = 0; i < outputs; ++i) {
            check(g_ort->SessionGetOutputName(session_, i, alloc, &name));
            output_names_.emplace_back(name);
            check(g_ort->AllocatorFree(alloc, name));
        }

        check(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_));
        const std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(inputs)};
        check(g_ort->CreateTensorWithDataAsOrtValue(mem_, in_.data(), in_.size() * sizeof(float), shape.data(),
                                                    shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_));
        for (const auto& n : output_names_) output_ptrs_.push_back(n.c_str());
        outputs_.assign(output_names_.size(), nullptr);
    }
    OrtRunner(const OrtRunner&) = delete;
    OrtRunner& operator=(const OrtRunner&) = delete;
    ~OrtRunner() {
        for (OrtValue* v : outputs_) {
            if (v) g_ort->ReleaseValue(v);
        }
        g_ort->ReleaseValue(input_);
        g_ort->ReleaseMemoryInfo(mem_);
        g_ort->ReleaseSession(session_);
        g_ort->ReleaseEnv(env_);
    }

    // skl2onnx's classifiers output (label int64, probabilities float); return the label.
    std::int64_t predict(const double* x) {
        for (std::size_t i = 0; i < in_.size(); ++i) in_[i] = static_cast<float>(x[i]);
        for (OrtValue*& v : outputs_) {
            if (v) {
                g_ort->ReleaseValue(v);
                v = nullptr;
            }
        }
        const char* in_name = input_name_.c_str();
        check(g_ort->Run(session_, nullptr, &in_name, &input_, 1, output_ptrs_.data(), output_ptrs_.size(),
                         outputs_.data()));
        void* data = nullptr;
        check(g_ort->GetTensorMutableData(outputs_[0], &data));
        return *static_cast<const std::int64_t*>(data);
    }

private:
    std::vector<float> in_;
    OrtEnv* env_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* mem_ = nullptr;
    OrtValue* input_ = nullptr;
    std::string input_name_;
    std::vector<std::string> output_names_;
    std::vector<const char*> output_ptrs_;
    std::vector<OrtValue*> outputs_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <model.txt> <model.onnx> <rows.f64>\n", argv[0]);
        return 2;
    }
    try {
        g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        if (g_ort == nullptr) throw std::runtime_error("this libonnxruntime is older than its headers");
        const Model native = Model::load_file(argv[1]);
        const std::vector<double> rows = read_rows(argv[3]);
        const std::size_t k = native.inputs();
        const std::size_t n = rows.size() / k;
        OrtRunner ort(argv[2], k);
        const double per_ns = orderflow::tsc_per_ns();

        std::vector<std::uint64_t> t_scalar(n), t_simd(n), t_ort(n);
        std::size_t disagree = 0;
        volatile int sink = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double* x = &rows[i * k];
            auto a = orderflow::tsc_start();
            const auto p = native.predict(x);
            auto b = orderflow::tsc_stop();
            t_scalar[i] = b - a;
            a = orderflow::tsc_start();
            sink = native.predict_simd(x).cls;
            b = orderflow::tsc_stop();
            t_simd[i] = b - a;
            a = orderflow::tsc_start();
            const std::int64_t label = ort.predict(x);
            b = orderflow::tsc_stop();
            t_ort[i] = b - a;
            disagree += label != p.cls;
        }
        (void)sink;
        std::printf("%zu rows, %zu inputs; ONNX Runtime %s, 1 thread, sequential\n", n, k,
                    OrtGetApiBase()->GetVersionString());
        report("infer.hpp, scalar double", t_scalar, per_ns);
        report("infer.hpp, AVX2 float", t_simd, per_ns);
        report("ONNX Runtime C API, Run()", t_ort, per_ns);
        std::printf("  class disagreement, ONNX Runtime vs infer.hpp: %zu of %zu rows\n", disagree, n);
        return disagree * 1000 <= n ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
