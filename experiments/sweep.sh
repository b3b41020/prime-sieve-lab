#!/usr/bin/env bash
# Re-run the full parameter sweep and append rows to experiments/RESULTS.csv.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -x build/bench ]] || { echo "build first: cmake -S . -B build && cmake --build build -j" >&2; exit 1; }
OUT=experiments/RESULTS.csv

# A: wheel-width ablation at 1e8
for w in 2 30 210; do
  for b in 0 1; do
    for kb in 16 32 64; do
      ./build/bench --n 100m --wheel "$w" --bucket "$b" --seg-kb "$kb" --reps 3 >> "$OUT"
    done
  done
done

# B: segment-size sweep at 1e9
for kb in 16 32 64 128; do
  ./build/bench --n 1g --wheel 210 --bucket 1 --seg-kb "$kb" --reps 2 >> "$OUT"
done

# C: scaling
for n in 1m 10m 100m 1g 10g; do
  ./build/bench --n "$n" --wheel 210 --bucket 1 --seg-kb 32 --reps 2 >> "$OUT"
done

# D: bucket on/off at 1e10
for b in 0 1; do
  ./build/bench --n 10g --wheel 210 --bucket "$b" --seg-kb 32 --reps 1 >> "$OUT"
done
echo "appended to $OUT"
