#!/usr/bin/env bash
# gpu_recompute.sh — what does it cost a GPU prover to discard part of an
# Argon2d buffer and rebuild the missing blocks when a challenge probes them?
#
# The build-buffer design (ADR 0007) ships a recipe instead of the bytes, so the
# prover can always regenerate what it threw away. Memory-hardness is the claim
# that regenerating costs far more time than holding. This measures that claim
# directly: the `recompute` cheat keeps a random fraction of the blocks on the
# card, rebuilds each missing one on the host when a probe lands on it, and
# answers correctly, just late.
#
# Swept over the kept fraction and over Argon2's pass count t, because t is
# expected to decide the outcome: with t = 1 a missing block depends mostly on
# blocks the cheat kept, and with t >= 2 it depends on earlier-pass values that
# nobody kept, so one rebuild drags in much of the matrix. Keep 1.0 is the
# cheat with nothing discarded, which isolates what its pause-and-resume answer
# loop costs on its own.
#
# Honest is re-measured before every point, with the same recipe settings: the
# card's clocks cannot be locked on a container provider, so a baseline taken
# half an hour earlier is not a baseline.
#
# Usage: bash docs/investigations/partial-recompute/gpu_recompute.sh
#          [--mb 4096] [--k 262144] [--lanes 16384] [--n 30] [--fold tree]
#          [--passes "1 2"] [--keeps "1.0 0.999 0.99 0.95 0.9 0.5"]
#          [--build-p 16] [--budget 65536] [--threads 0]
set -uo pipefail
cd "$(dirname "$0")/../../.."

MB=4096; K=262144; LANES=16384; N=30; WARMUP=5; PORT=9099; FOLD=tree
PASSES="1 2"; KEEPS="1.0 0.999 0.99 0.95 0.9 0.5"; BUILD_P=16; BUDGET=65536; THREADS=0
while [ $# -gt 0 ]; do
  case "$1" in
    --mb) MB="$2"; shift 2 ;; --k) K="$2"; shift 2 ;;
    --lanes) LANES="$2"; shift 2 ;; --n) N="$2"; shift 2 ;;
    --warmup) WARMUP="$2"; shift 2 ;; --port) PORT="$2"; shift 2 ;;
    --fold) FOLD="$2"; shift 2 ;;
    --passes) PASSES="$2"; shift 2 ;; --keeps) KEEPS="$2"; shift 2 ;;
    --build-p) BUILD_P="$2"; shift 2 ;; --budget) BUDGET="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="results/recompute/$STAMP"
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
  local tag="$1" mode="$2" passes="$3" keep="$4"
  kill_p
  if [ "$mode" = "honest" ]; then
    nohup ./prover_cuda "$PORT" --trace 0 --lane-cap "$LANES" > "$OUT/p-$tag.log" 2>&1 &
  else
    nohup ./cheating_prover_cuda --port "$PORT" --mode recompute --keep "$keep" \
          --rebuild-budget "$BUDGET" --rebuild-threads "$THREADS" \
          --lane-cap "$LANES" > "$OUT/p-$tag.log" 2>&1 &
  fi
  wait_port || { echo "  $tag: prover never listened"; return 1; }
  nvidia-smi --query-gpu=clocks.sm,temperature.gpu,clocks_throttle_reasons.active \
             --format=csv,noheader > "$OUT/clk-$tag.txt" 2>/dev/null
  timeout 1800 ./verifier --host 127.0.0.1 --port "$PORT" --mb "$MB" --k "$K" \
      --lanes "$LANES" --n "$N" --warmup "$WARMUP" --fold "$FOLD" \
      --gen argon2d --build-t "$passes" --build-p "$BUILD_P" --label "$tag" \
      --csv /dev/null --json "$OUT/$tag.json" > "$OUT/v-$tag.log" 2>&1
  local rc=$?
  kill_p
  [ "$rc" -eq 0 ] || { echo "  $tag: verifier exited $rc (a timeout is a result — see the log)"; return 1; }
  return 0
}

{
  echo "geometry: ${MB} MiB, k=$K, lanes=$LANES, fold=$FOLD, n=$N, warmup=$WARMUP"
  echo "recipe:   argon2d, build-p=$BUILD_P, passes in {$PASSES}"
  echo "cheat:    keeps in {$KEEPS}, budget=$BUDGET fills/thread/challenge, threads=$THREADS (0 = all allowed CPUs)"
  echo "cpus:     $(nproc) allowed of $(getconf _NPROCESSORS_ONLN) online"
  nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader 2>/dev/null
} | tee "$OUT/session.txt"
echo "output:   $OUT"
echo

for t in $PASSES; do
  i=0
  for f in $KEEPS; do
    i=$((i+1))
    printf '  t=%s baseline %-2d ' "$t" "$i"; trial "honest-t$t-$i" honest "$t" 1 && echo ok
    printf '  t=%s keep %-7s ' "$t" "$f"; trial "keep-t$t-$f" recompute "$t" "$f" && echo ok
  done
done

echo
echo "=== partial recompute (within this session only) ==="
python3 - "$OUT" <<'PY' | tee "$OUT/summary.txt"
import glob, json, os, statistics, sys
out = sys.argv[1]

def result(path):
    try:
        r = json.load(open(path))["result"]
        return r["percentiles_us"]["p50"]["value"], r["errors"], r.get("n")
    except Exception:
        return None, None, None

def prover_line(path, kind):
    try:
        for line in open(path):
            if line.startswith("@") and f'"t":"{kind}"' in line:
                return json.loads(line[1:])
    except Exception:
        pass
    return {}

passes = sorted({os.path.basename(f).split("-")[1] for f in glob.glob(os.path.join(out, "keep-t*.json"))})
for t in passes:
    base = [result(f)[0] for f in sorted(glob.glob(os.path.join(out, f"honest-{t}-*.json")))]
    base = [b for b in base if b]
    if not base:
        print(f"{t}: no honest baseline — nothing to compare against\n")
        continue
    bmed = statistics.median(base)
    bsd = statistics.pstdev(base) if len(base) > 1 else 0.0
    thresh = bmed + 3 * bsd
    print(f"{t}: honest p50 median {bmed:.1f} us, sd {bsd:.1f} us over {len(base)} baselines; "
          f"separation threshold (median + 3sd) {thresh:.1f} us")
    print(f"{'keep':>7} {'HBM saved':>9} {'p50 us':>11} {'x honest':>9} {'errors':>7} "
          f"{'rebuilt/ch':>10} {'rounds/ch':>9} {'rebuild ms/ch':>13} {'gave up':>7}  separated?")
    rows = []
    for f in glob.glob(os.path.join(out, f"keep-{t}-*.json")):
        keep = os.path.basename(f)[len(f"keep-{t}-"):-5]
        tag = os.path.basename(f)[:-5]
        p50, err, n = result(f)
        if p50 is None:
            continue
        info = prover_line(os.path.join(out, f"p-{tag}.log"), "cheatinfo")
        summ = prover_line(os.path.join(out, f"p-{tag}.log"), "rc_summary")
        saved = 1 - info["kept_blocks"] / info["blocks"] if info.get("blocks") else float("nan")
        rows.append((float(keep), saved, p50, err, summ))
    for keep, saved, p50, err, summ in sorted(rows, reverse=True):
        sep = "yes" if p50 > thresh else "no"
        print(f"{keep:>7.3f} {saved*100:>8.2f}% {p50:>11.1f} {p50/bmed:>8.2f}x {err:>7} "
              f"{summ.get('rebuilt_per_ch', float('nan')):>10.1f} "
              f"{summ.get('rounds_per_ch', float('nan')):>9.1f} "
              f"{summ.get('rebuild_ms_per_ch', float('nan')):>13.3f} "
              f"{summ.get('gave_up_challenges', '?'):>7}  {sep}")
    print()
print("Errors are the verifier's count of wrong answers. A cheat that ran out of")
print("rebuild budget answers wrong, so a nonzero count there means it gave up.")
print("Only ratios within this session are quotable; absolute microseconds are not.")
PY
