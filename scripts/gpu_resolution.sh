#!/usr/bin/env bash
# gpu_resolution.sh — how little can a prover offload before the detector stops
# seeing it?
#
# The first GPU session moved the whole buffer on or off the card and found a
# cliff: indistinguishable from honest until nothing fit, then 4,600x. That
# answers "can you catch a prover holding none of it" and says nothing about the
# question a decision rule actually needs, which is how small a lie is still
# visible.
#
# This sweeps the `partial` cheat, which holds every byte and answers correctly
# but keeps a fraction of them in host memory reached over PCIe. Offload 0 is
# the honest prover by construction, so the sweep carries its own baseline.
#
# Honest is re-measured between every offload point rather than once at the
# start: the card's clocks cannot be locked on a container provider, so a
# baseline taken half an hour earlier is not a baseline.
#
# Usage: bash scripts/gpu_resolution.sh [--mb 4096] [--k 262144] [--lanes 16384]
#                                       [--n 40] [--fold tree|sequential]
set -uo pipefail
cd "$(dirname "$0")/.."

MB=4096; K=262144; LANES=16384; N=40; WARMUP=10; PORT=9099; FOLD=tree
OFFLOADS="0.000 0.001 0.002 0.005 0.010 0.020 0.050 0.100 0.250 0.500"
while [ $# -gt 0 ]; do
  case "$1" in
    --mb) MB="$2"; shift 2 ;; --k) K="$2"; shift 2 ;;
    --lanes) LANES="$2"; shift 2 ;; --n) N="$2"; shift 2 ;;
    --warmup) WARMUP="$2"; shift 2 ;; --port) PORT="$2"; shift 2 ;;
    --fold) FOLD="$2"; shift 2 ;;
    --offloads) OFFLOADS="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="results/resolution/$STAMP"
mkdir -p "$OUT"

kill_p() { pkill -f cheating_prover_cuda >/dev/null 2>&1; pkill -f prover_cuda >/dev/null 2>&1; sleep 1; }
trap kill_p EXIT

wait_port() {
  local w=0
  while ! (echo > "/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; do
    sleep 0.2; w=$((w+1)); [ "$w" -gt 300 ] && return 1
  done
  return 0
}

# One trial. mode=honest runs the honest binary so the baseline really is the
# verify-and-refuse path, not the cheat binary being polite.
trial() {
  local tag="$1" mode="$2" offload="$3"
  kill_p
  if [ "$mode" = "honest" ]; then
    nohup ./prover_cuda "$PORT" --trace 0 --lane-cap "$LANES" > "$OUT/p-$tag.log" 2>&1 &
  else
    nohup ./cheating_prover_cuda --port "$PORT" --mode partial --offload "$offload" \
          --trace 0 --lane-cap "$LANES" > "$OUT/p-$tag.log" 2>&1 &
  fi
  wait_port || { echo "  $tag: prover never listened"; return 1; }
  nvidia-smi --query-gpu=clocks.sm,temperature.gpu,clocks_throttle_reasons.active \
             --format=csv,noheader > "$OUT/clk-$tag.txt" 2>/dev/null
  timeout 900 ./verifier --host 127.0.0.1 --port "$PORT" --mb "$MB" --k "$K" \
      --lanes "$LANES" --n "$N" --warmup "$WARMUP" --fold "$FOLD" --label "$tag" \
      --csv /dev/null --json "$OUT/$tag.json" > "$OUT/v-$tag.log" 2>&1
  local rc=$?
  kill_p
  [ "$rc" -eq 0 ] || { echo "  $tag: verifier exited $rc"; return 1; }
  return 0
}

echo "geometry: ${MB} MiB, k=$K, lanes=$LANES, fold=$FOLD, n=$N"
echo "output:   $OUT"
echo

i=0
for f in $OFFLOADS; do
  i=$((i+1))
  printf '  baseline %-2d ' "$i"; trial "honest-$i" honest 0 && echo ok
  printf '  offload %-6s ' "$f"; trial "off-$f" partial "$f" && echo ok
done

echo
echo "=== resolution (within this session only) ==="
python3 - "$OUT" <<'PY'
import json, sys, glob, os, statistics
out = sys.argv[1]

def p50(path):
    try:
        r = json.load(open(path))["result"]
        return r["percentiles_us"]["p50"]["value"], r["errors"]
    except Exception:
        return None, None

base = [p50(f)[0] for f in sorted(glob.glob(os.path.join(out, "honest-*.json")))]
base = [b for b in base if b]
if not base:
    print("no honest baseline — nothing to compare against")
    raise SystemExit(0)

bmed = statistics.median(base)
bsd = statistics.pstdev(base) if len(base) > 1 else 0.0
# A point counts as separated only if it clears the honest spread, not just the
# honest median. With clocks unlockable that spread is the real noise floor.
thresh = bmed + 3 * bsd
print(f"honest p50 median {bmed:.1f} us, spread (sd) {bsd:.1f} us over {len(base)} baselines")
print(f"separation threshold (median + 3sd): {thresh:.1f} us\n")
print(f"{'offload':>9} {'p50 us':>10} {'x honest':>10} {'errors':>8}  separated?")

rows = []
for f in sorted(glob.glob(os.path.join(out, "off-*.json"))):
    frac = os.path.basename(f)[4:-5]
    v, err = p50(f)
    if v is None:
        continue
    rows.append((float(frac), v, err))
rows.sort()
smallest = None
for frac, v, err in rows:
    sep = v > thresh
    if sep and smallest is None and frac > 0:
        smallest = frac
    print(f"{frac:>9.3f} {v:>10.1f} {v/bmed:>9.2f}x {err:>8}  {'yes' if sep else 'no'}")
print()
if smallest is not None:
    print(f"Smallest offload fraction this detector separated: {smallest:.3f} "
          f"({smallest*100:.1f}% of the buffer off the card).")
    print("That is the detector's resolution at this geometry, on this card, in")
    print("this session. It is not a portable number.")
else:
    print("No offload fraction in this sweep cleared the honest spread. Either the")
    print("detector's resolution is coarser than the largest fraction tried, or the")
    print("noise floor is too wide at this geometry. Report it as measured.")
PY
