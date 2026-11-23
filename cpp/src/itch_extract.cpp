// itch_extract: write the messages of chosen symbols to a smaller ITCH file.
//
//   itch_extract data/itch/01302019.NASDAQ_ITCH50 data/itch/01302019.AAPL AAPL [MSFT ...]
//
// A whole-day backtest is I/O-bound on the 11 GB file; one liquid symbol's
// extract is ~1% of it and replays from the page cache. See itch_extract.hpp
// for why filtering by stock locate is exact.
#include <cstdio>
#include <exception>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "orderflow/itch_extract.hpp"
#include "orderflow/mapped_file.hpp"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <in-itch-file> <out-file> SYMBOL [SYMBOL ...]\n", argv[0]);
        return 2;
    }
    try {
        const orderflow::MappedFile in(argv[1]);
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error(std::string("cannot open ") + argv[2]);
        const std::vector<std::string> symbols(argv + 3, argv + argc);
        const auto st = orderflow::itch::extract(in.bytes(), symbols, [&](std::span<const std::byte> m) {
            out.write(reinterpret_cast<const char*>(m.data()), static_cast<std::streamsize>(m.size()));
        });
        out.close();
        if (!out) throw std::runtime_error("write failed");
        std::printf("read %llu messages, kept %llu; found:", static_cast<unsigned long long>(st.read),
                    static_cast<unsigned long long>(st.kept));
        for (const auto& s : st.found) std::printf(" %s", s.c_str());
        std::printf("\n");
        return st.found.size() == symbols.size() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
