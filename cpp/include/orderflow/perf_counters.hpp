// Hardware performance counters for the calling thread, via perf_event_open(2).
//
// The perf tool doesn't run on WSL2's kernel, but the system call does, and at
// perf_event_paranoid 2 a process may count its own user-space events. Events
// (Linux generic names, mapped to vendor events by the kernel): cycles,
// instructions, L1d load misses, dTLB load misses, cache misses (vendor
// defined; on AMD not the same as LLC load misses, which this kernel doesn't
// support), branch misses.
//
// Each event is opened separately. If more are open than the CPU has counters,
// the kernel multiplexes them; values are scaled by enabled/running time and
// the fraction of time each was actually counted is reported with it.
#pragma once

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace orderflow {

class PerfCounters {
public:
    enum Event : std::size_t { cycles, instructions, l1d_misses, dtlb_misses, cache_misses, branch_misses, kCount };
    static constexpr std::array<std::string_view, kCount> kNames = {
        "cycles", "instructions", "L1d-load-misses", "dTLB-load-misses", "cache-misses", "branch-misses"};

    struct Reading {
        std::array<double, kCount> value{};    // scaled to the full enabled time
        std::array<double, kCount> running{};  // fraction of enabled time actually counted, 0..1
        std::array<bool, kCount> available{};
    };

    PerfCounters() {
        constexpr auto cache = [](std::uint64_t id, std::uint64_t op, std::uint64_t result) {
            return id | (op << 8) | (result << 16);
        };
        const std::array<std::pair<std::uint32_t, std::uint64_t>, kCount> spec = {{
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
            {PERF_TYPE_HW_CACHE,
             cache(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
            {PERF_TYPE_HW_CACHE,
             cache(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
        }};
        for (std::size_t i = 0; i < kCount; ++i) fd_[i] = open_event(spec[i].first, spec[i].second);
    }
    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;
    ~PerfCounters() {
        for (const int fd : fd_) {
            if (fd >= 0) close(fd);
        }
    }

    [[nodiscard]] bool any_available() const noexcept {
        for (const int fd : fd_) {
            if (fd >= 0) return true;
        }
        return false;
    }

    // Counting accumulates across start/stop pairs, so a benchmark can count
    // only its timed regions and skip the untimed setup between them.
    void start() noexcept { ioctl_all(PERF_EVENT_IOC_ENABLE); }
    void stop() noexcept { ioctl_all(PERF_EVENT_IOC_DISABLE); }

    [[nodiscard]] Reading read() const noexcept {
        Reading r;
        for (std::size_t i = 0; i < kCount; ++i) {
            if (fd_[i] < 0) continue;
            // With TOTAL_TIME_ENABLED | TOTAL_TIME_RUNNING the kernel returns
            // {value, time_enabled, time_running}.
            std::array<std::uint64_t, 3> raw{};
            if (::read(fd_[i], raw.data(), sizeof raw) != static_cast<ssize_t>(sizeof raw)) continue;
            const auto [value, enabled, running] = raw;
            r.available[i] = true;
            if (running == 0) continue;  // never scheduled: leave the value at 0, running at 0
            r.running[i] = static_cast<double>(running) / static_cast<double>(enabled);
            r.value[i] = static_cast<double>(value) * static_cast<double>(enabled) / static_cast<double>(running);
        }
        return r;
    }

    // One line per event, normalised per unit of work (per message, per op).
    static void print(const Reading& r, double units, std::string_view unit_name) {
        for (std::size_t i = 0; i < kCount; ++i) {
            if (!r.available[i]) {
                std::printf("  %-17s unavailable on this kernel/CPU\n", kNames[i].data());
                continue;
            }
            std::printf("  %-17s %10.3f per %.*s  (counted %3.0f%% of the time)\n", kNames[i].data(),
                        r.value[i] / units, static_cast<int>(unit_name.size()), unit_name.data(),
                        100.0 * r.running[i]);
        }
        if (r.available[cycles] && r.available[instructions] && r.value[cycles] > 0) {
            std::printf("  IPC (instructions per cycle) %.2f\n", r.value[instructions] / r.value[cycles]);
        }
    }

private:
    static int open_event(std::uint32_t type, std::uint64_t config) noexcept {
        perf_event_attr attr{};
        attr.size = sizeof attr;
        attr.type = type;
        attr.config = config;
        attr.disabled = 1;        // start() enables
        attr.exclude_kernel = 1;  // user space only: allowed at perf_event_paranoid 2
        attr.exclude_hv = 1;
        attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        // pid 0, cpu -1: this thread, on whichever CPU it runs.
        return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
    }

    void ioctl_all(unsigned long request) const noexcept {
        for (const int fd : fd_) {
            if (fd >= 0) ioctl(fd, request, 0);
        }
    }

    std::array<int, kCount> fd_{};
};

}  // namespace orderflow
