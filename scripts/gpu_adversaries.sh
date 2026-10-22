#!/usr/bin/env bash
# gpu_adversaries.sh — run the honest GPU prover and each GPU cheat against the
# same verifier settings, in one session, and print the p50 ratios.
#
# Why one script rather than four commands: the only trustworthy comparison is
# within a single session on a single card. Absolute microseconds here do not
# transfer to another box, another rental of the same box, or another day —
# clock locking is normally denied on a container provider, so the card's own
# clock wanders. Ratios measured minutes apart on one card are what mean
# something.
#
# Modes are interleaved rather than run in blocks for the same reason: thermal
# drift over a long session would otherwise look like a treatment effect.
#
# The verifier's --fold defaults to sequential, because the tree fold changes
# the answers and so has to be opted into. This script therefore has to name it:
# without --fold the sweep measures a fold the project no longer ships, and its
# p50s are roughly twice what the shipped one produces at the same geometry.
#
# Usage: bash scripts/gpu_adversaries.sh [--mb 4096] [--k 262144] [--lanes 16384]
#                                        [--n 50] [--hog 41000] [--cycles 2]
#                                        [--fold tree|sequential]
set -uo pipefail
cd "$(dirname "$0")/.."

MB=4096; K=262144; LANES=16384; N=50; WARMUP=10; HOG=41000; CYCLES=2; PORT=9099
FOLD=tree
while [ $# -gt 0 ]; do
  case "$1" in
    --mb) MB="$2"; shift 2 ;; --k) K="$2"; shift 2 ;;
    --lanes) LANES="$2"; shift 2 ;; --n) N="$2"; shift 2 ;;
    --warmup) WARMUP="$2"; shift 2 ;; --hog) HOG="$2"; shift 2 ;;
    --cycles) CYCLES="$2"; shift 2 ;; --port) PORT="$2"; shift 2 ;;
    --fold) FOLD="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done
case "$FOLD" in
  tree|sequential) ;;
  *) echo "--fold must be tree or sequential, got: $FOLD" >&2; exit 1 ;;
esac

mkdir -p results/adversaries
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="results/adversaries/$STAMP"
mkdir -p "$OUT"

kill_provers() { pkill -f prover_cuda >/dev/null 2>&1; pkill -f cheating_prover_cuda >/dev/null 2>&1; sleep 1; }
trap kill_provers EXIT

wait_for_port() {
  local waited=0
  while ! (echo > "/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; do
    sleep 0.2; waited=$((waited + 1))
    [ "$waited" -gt 300 ] && return 1
  done
  return 0
}

# One (mode, cycle) trial. Writes the verifier JSON and the prover's event log.
run_one() {
  local mode="$1" cycle="$2"
  local tag="${mode}-c${cycle}"
  kill_provers

  if [ "$mode" = "honest" ]; then
    nohup ./prover_cuda "$PORT" --trace 10 --lane-cap "$LANES" \
      > "$OUT/prover-$tag.log" 2>&1 &
  else
    local hog=0
    # Only the migrating and host-mapped cheats care about HBM pressure; fake
    # allocates device memory outright and holds less of it by design.
    [ "$mode" = "uvm" ] && hog="$HOG"
    nohup ./cheating_prover_cuda --port "$PORT" --mode "$mode" --frac 0.5 \
      --hog "$hog" --trace 10 --lane-cap "$LANES" \
      > "$OUT/prover-$tag.log" 2>&1 &
  fi

  if ! wait_for_port; then
    echo "  $tag: prover never listened — see $OUT/prover-$tag.log"
    return 1
  fi

  # The card's clock wanders and cannot be locked here, so record it beside the
  # trial rather than pretending it was constant.
  nvidia-smi --query-gpu=clocks.sm,clocks.mem,temperature.gpu,clocks_throttle_reasons.active \
             --format=csv,noheader > "$OUT/clocks-$tag.txt" 2>/dev/null

  timeout 900 ./verifier --host 127.0.0.1 --port "$PORT" \
      --mb "$MB" --k "$K" --lanes "$LANES" --n "$N" --warmup "$WARMUP" \
      --fold "$FOLD" \
      --label "$tag" --csv /dev/null --json "$OUT/$tag.json" \
      > "$OUT/verifier-$tag.log" 2>&1
  local rc=$?
  kill_provers
  if [ "$rc" -ne 0 ]; then
    echo "  $tag: verifier exited $rc (900s timeout is a result, not a crash — see the log)"
    return 1
  fi
  return 0
}

echo "geometry: ${MB} MiB buffer, k=$K, lanes=$LANES, depth=$((K / LANES)), n=$N, fold=$FOLD"
echo "output:   $OUT"
echo

for c in $(seq 1 "$CYCLES"); do
  echo "--- cycle $c of $CYCLES ---"
  for m in honest fake uvm zerocopy; do
    printf '  %-9s ' "$m"
    if run_one "$m" "$c"; then
      read -r p50 err < <(python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))['result']
print(d['percentiles_us']['p50']['value'], d['errors'])" "$OUT/$m-c$c.json" 2>/dev/null)
      echo "p50=${p50:-?} us  errors=${err:-?}"
    fi
  done
done

echo
echo "=== summary (within this session only) ==="
python3 - "$OUT" <<'PY'
import json, sys, glob, os, statistics
out = sys.argv[1]
by = {}
for f in sorted(glob.glob(os.path.join(out, "*-c*.json"))):
    mode = os.path.basename(f).rsplit("-c", 1)[0]
    try:
        d = json.load(open(f))
    except Exception:
        continue
    r = d.get("result") or {}
    p50 = ((r.get("percentiles_us") or {}).get("p50") or {}).get("value")
    err = r.get("errors")
    if p50 is None:
        continue
    by.setdefault(mode, {"p50": [], "err": 0})
    by[mode]["p50"].append(p50)
    by[mode]["err"] += int(err or 0)

base = statistics.median(by["honest"]["p50"]) if "honest" in by else None
print(f"{'mode':<10} {'p50 us':>10} {'x honest':>10} {'errors':>8}")
for m in ("honest", "fake", "uvm", "zerocopy"):
    if m not in by:
        print(f"{m:<10} {'did not run':>10}")
        continue
    p = statistics.median(by[m]["p50"])
    r = f"{p/base:.2f}x" if base else "-"
    print(f"{m:<10} {p:>10.1f} {r:>10} {by[m]['err']:>8}")
print()
print("fake is meant to be FAST and WRONG: judge it by the error column, not p50.")
print("uvm with HBM taken away, and zerocopy, are meant to be SLOW and RIGHT.")
PY
