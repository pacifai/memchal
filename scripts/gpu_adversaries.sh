#!/usr/bin/env bash
# gpu_adversaries.sh — run the honest GPU prover and each GPU cheat against the
# same verifier settings, in one session, and print the p50 ratios.
#
# Absolute times do not transfer between cards or sessions, so compare ratios
# within one run. Modes are interleaved so thermal drift spreads across all of
# them. uvm runs with --leave-free MiB of HBM free, and partial runs with
# --offload of the buffer in host memory.
#
# Usage: bash scripts/gpu_adversaries.sh [--mb 4096] [--k 262144] [--lanes 16384]
#                                        [--n 50] [--leave-free 2048] [--offload 1.0]
#                                        [--cycles 2] [--fold tree|sequential]
set -uo pipefail
cd "$(dirname "$0")/.."

MB=4096; K=262144; LANES=16384; N=50; WARMUP=10; LEAVE_FREE=2048; OFFLOAD=1.0
CYCLES=2; PORT=9099
FOLD=tree
while [ $# -gt 0 ]; do
  case "$1" in
    --mb) MB="$2"; shift 2 ;; --k) K="$2"; shift 2 ;;
    --lanes) LANES="$2"; shift 2 ;; --n) N="$2"; shift 2 ;;
    --warmup) WARMUP="$2"; shift 2 ;; --leave-free) LEAVE_FREE="$2"; shift 2 ;;
    --offload) OFFLOAD="$2"; shift 2 ;;
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
    nohup ./prover_cuda --port "$PORT" --trace 10 --lane-cap "$LANES" \
      > "$OUT/prover-$tag.log" 2>&1 &
  else
    local extra=()
    case "$mode" in
      fake)    extra=(--frac 0.5) ;;
      uvm)     extra=(--leave-free "$LEAVE_FREE") ;;
      partial) extra=(--offload "$OFFLOAD") ;;
    esac
    nohup ./cheating_prover_cuda --port "$PORT" --mode "$mode" "${extra[@]}" \
      --trace 10 --lane-cap "$LANES" \
      > "$OUT/prover-$tag.log" 2>&1 &
  fi

  if ! wait_for_port; then
    echo "  $tag: prover never listened — see $OUT/prover-$tag.log"
    return 1
  fi

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
    echo "  $tag: verifier exited $rc (a 900 s timeout counts as a result; see the log)"
    return 1
  fi
  return 0
}

echo "geometry: ${MB} MiB buffer, k=$K, lanes=$LANES, depth=$((K / LANES)), n=$N, fold=$FOLD"
echo "uvm leaves ${LEAVE_FREE} MiB free; partial offloads ${OFFLOAD}"
echo "output:   $OUT"
echo

for c in $(seq 1 "$CYCLES"); do
  echo "--- cycle $c of $CYCLES ---"
  for m in honest fake uvm partial; do
    printf '  %-9s ' "$m"
    if run_one "$m" "$c"; then
      read -r p50 err < <(python3 scripts/gpu_adversaries.py p50 "$OUT/$m-c$c.json" 2>/dev/null)
      echo "p50=${p50:-?} us  errors=${err:-?}"
    fi
  done
done

echo
echo "=== summary (within this session only) ==="
python3 scripts/gpu_adversaries.py summary "$OUT"
