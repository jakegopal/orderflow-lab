#!/usr/bin/env bash
# Run every Phase 2 benchmark pinned to one CPU and save the raw output.
#
#   scripts/run_benchmarks.sh [itch-file]        CPU=2 OUT=docs/phase2/results by default
#
# Needs a release build: cmake --preset release && cmake --build --preset release
# Every result file starts with the build and machine it came from. The
# replay throughput runs twice per variant, so run-to-run spread is visible.
set -euo pipefail

file="${1:-data/itch/01302019.NASDAQ_ITCH50}"
cpu="${CPU:-2}"
out="${OUT:-docs/phase2/results}"
bench=build/release/cpp/bench
mkdir -p "$out"

{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "kernel: $(uname -r)"
    lscpu | grep -E 'Model name|^CPU\(s\)|L1d|L2|L3'
    echo "memory: $(free -m | awk '/Mem:/ {print $2 " MiB"}')"
    echo "compiler: $(c++ --version | head -1)"
    echo "pinned to cpu: $cpu (taskset)"
    echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
    echo "commit: $(git rev-parse --short HEAD)$(git diff --quiet || echo ' (dirty)')"
} > "$out/environment.txt"

taskset -c "$cpu" "$bench/bench_structures" \
    --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
    > "$out/structures.txt" 2>&1

: > "$out/replay_throughput.txt"
for run in 1 2; do
    for v in map-std map-open flat-std flat-open; do
        echo "== run $run" >> "$out/replay_throughput.txt"
        taskset -c "$cpu" "$bench/bench_replay" --variant "$v" "$file" >> "$out/replay_throughput.txt" 2>&1
    done
done

: > "$out/replay_latency.txt"
for v in map-std map-open flat-std flat-open; do
    taskset -c "$cpu" "$bench/bench_replay" --latency --variant "$v" "$file" >> "$out/replay_latency.txt" 2>&1
done
echo "results in $out"
