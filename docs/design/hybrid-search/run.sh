#!/bin/sh
# Builds vecbench, dry-runs it, then runs the sweep under a strict hold.
set -e
cd "$(dirname "$0")"
OUT=.
quiet light vecbench-build 5 "hybrid-search vecbench build" -- sh -c 'clang++ -std=c++20 -O3 -DNDEBUG -mavx2 -mfma -mf16c -mpopcnt -o vecbench vecbench.cc && clang++ -std=c++20 -O3 -target arm64-apple-macos13 -c -o vecbench-arm64.o vecbench.cc'
quiet light vecbench-dry 5 "hybrid-search vecbench dry run" -- sh -c './vecbench 0.001 16 int8 > dry.jsonl'
test "$(wc -l < dry.jsonl)" -gt 10
start=$(date +%s)
quiet strict vecbench 20 "hybrid-search exact kNN scan sweep (1 thread)" -- ./vecbench 0.3 256 > $OUT/vecbench-mac.jsonl
echo "wall_s=$(( $(date +%s) - start )) host=$(sysctl -n machdep.cpu.brand_string) clang=$(clang++ --version | head -1)" > $OUT/vecbench-mac.meta
echo done
