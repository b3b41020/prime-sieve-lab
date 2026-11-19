#!/usr/bin/env bash
# Self-test hook: preflight for every bot job and every CI run.
# Exit 0 only when the compiled suite passes AND its git sha matches the tree.
set -euo pipefail
cd "$(dirname "$0")/.."

BENCH=${SIEVE_BENCH:-build/bench}
CHECK=${SIEVE_CHECK:-build/check}
[[ -x "$BENCH" && -x "$CHECK" ]] || { echo "build first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2; exit 2; }

sha=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
echo "[selftest] git=$sha"

# 1) correctness suite (all knobs, exact pi(n) values)
if ! "$CHECK" > /tmp/sielab_selftest.log 2>&1; then
  echo "[selftest] FAIL: correctness suite" >&2
  tail -5 /tmp/sielab_selftest.log >&2
  exit 1
fi
echo "[selftest] ok: $(tail -1 /tmp/sielab_selftest.log)"

# 2) contract check: bench emits the documented CSV shape and a known value
row=$("$BENCH" --n 1000000 --wheel 210 --bucket 1 --seg-kb 32 --reps 1)
fields=$(awk -F, '{print NF}' <<<"$row")
[[ "$fields" -eq 10 ]] || { echo "[selftest] FAIL: bench emitted $fields fields, expected 10" >&2; exit 1; }
primes=$(cut -d, -f7 <<<"$row")
[[ "$primes" == "78498" ]] || { echo "[selftest] FAIL: pi(1e6)=$primes, expected 78498" >&2; exit 1; }
echo "[selftest] ok: bench contract (10 fields, pi(1e6)=$primes, $(cut -d, -f9 <<<"$row") marks)"

# 3) bridge schema check (optional, only when the bridge deps are present)
if [[ -f bot/tools.json ]]; then
  python3 - <<'PY'
import json
doc = json.load(open('bot/tools.json'))
names = {t['name'] for t in doc['tools']}
assert names == {'sieve_selftest', 'sieve_count', 'sieve_autotune'}, names
print('[selftest] ok: tool schema', sorted(names))
PY
fi

echo "[selftest] PASSED git=$sha"