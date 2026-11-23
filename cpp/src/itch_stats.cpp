// itch_stats: count every message in an ITCH 5.0 file by type.
//
//   itch_stats data/itch/01302019.NASDAQ_ITCH50
//
// This is the parser's whole-file check: it frames every message, validates
// every length against the spec, and prints per-type counts that can be
// compared with an independent parser. It exits non-zero if any message is
// malformed or the file is truncated.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>

#include "orderflow/itch.hpp"
#include "orderflow/mapped_file.hpp"

namespace {

using namespace orderflow::itch;

struct Counter {
    std::array<std::uint64_t, 256> by_type{};
    void on_other(const Header& h, std::span<const std::byte>) {
        ++by_type[static_cast<unsigned char>(h.type)];
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 2;
    }
    try {
        const orderflow::MappedFile file(argv[1]);
        const auto start = std::chrono::steady_clock::now();

        Counter counter;  // only on_other(), so every message type lands in it
        std::uint64_t total = 0, bad = 0;
        FrameReader reader(file.bytes());
        while (auto msg = reader.next()) {
            ++total;
            if (parse(*msg, counter) != ParseStatus::ok) {
                if (++bad <= 10) {
                    std::fprintf(stderr, "malformed message at byte %zu\n", reader.offset());
                }
            }
        }
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        for (int t = 0; t < 256; ++t) {
            if (counter.by_type[static_cast<std::size_t>(t)] != 0) {
                std::printf("%c %12llu\n", t,
                            static_cast<unsigned long long>(counter.by_type[static_cast<std::size_t>(t)]));
            }
        }
        std::printf("total %llu messages, %llu malformed, truncated: %s\n",
                    static_cast<unsigned long long>(total), static_cast<unsigned long long>(bad),
                    reader.truncated() ? "yes" : "no");
        std::fprintf(stderr, "%.2f s, %.1f M msg/s, %.2f GB/s\n", secs, static_cast<double>(total) / secs / 1e6,
                     static_cast<double>(file.bytes().size()) / secs / 1e9);
        return (bad == 0 && !reader.truncated()) ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
