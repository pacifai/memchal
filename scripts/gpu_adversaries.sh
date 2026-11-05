#!/usr/bin/env bash
# Runs the honest GPU prover and each GPU cheat in interleaved cycles against one verifier setting and prints p50 ratios with their propagated error.
# Usage: bash scripts/gpu_adversaries.sh [--mb 4096] [--samples 262144] [--lanes 16384] [--challenges 50] [--warmup 10]
#          [--leave-free 2048] [--offload 1.0] [--cycles 2] [--port 9099] [--fold tree|sequential]
#   --leave-free  MiB of HBM the uvm cheat leaves free
#   --offload     fraction of the buffer the partial cheat keeps in host memory
set -uo pipefail
cd "$(dirname "$0")/.."

MB=4096; SAMPLES=262144; LANES=16384; CHALLENGES=50; WARMUP=10; LEAVE_FREE=2048; OFFLOAD=1.0
CYCLES=2; PORT=9099
FOLD=tree
while [ $# -gt 0 ]; do
  case "$1" in
    --mb) MB="$2"; shift 2 ;; --samples) SAMPLES="$2"; shift 2 ;;
    --lanes) LANES="$2"; shift 2 ;; --challenges) CHALLENGES="$2"; shift 2 ;;
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

kill_provers() { pkill -f build/gpu/prover >/dev/null 2>&1; pkill -f build/attacks/gpu/cheating_prover >/dev/null 2>&1; sleep 1; }
trap kill_provers EXIT

# Waits up to 60 s for a prover to listen on $PORT.
wait_for_port() {
  local waited=0
  while ! (echo > "/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; do
    sleep 0.2; waited=$((waited + 1))
    [ "$waited" -gt 300 ] && return 1
  done
  return 0
}

# Runs one (mode, cycle) trial and writes the verifier JSON and the prover log.
run_one() {
  local mode="$1" cycle="$2"
  local tag="${mode}-c${cycle}"
  kill_provers

  if [ "$mode" = "honest" ]; then
    nohup build/gpu/prover --port "$PORT" --trace 10 --lane-cap "$LANES" \
      > "$OUT/prover-$tag.log" 2>&1 &
  else
    local extra=()
    case "$mode" in
      fake)    extra=(--frac 0.5) ;;
      uvm)     extra=(--leave-free "$LEAVE_FREE") ;;
      partial) extra=(--offload "$OFFLOAD") ;;
    esac
    nohup build/attacks/gpu/cheating_prover --port "$PORT" --mode "$mode" "${extra[@]}" \
      --trace 10 --lane-cap "$LANES" \
      > "$OUT/prover-$tag.log" 2>&1 &
  fi

  if ! wait_for_port; then
    echo "  $tag: prover never listened — see $OUT/prover-$tag.log"
    return 1
  fi

  nvidia-smi --query-gpu=clocks.sm,clocks.mem,temperature.gpu,clocks_throttle_reasons.active \
             --format=csv,noheader > "$OUT/clocks-$tag.txt" 2>/dev/null

  timeout 900 build/verifier --host 127.0.0.1 --port "$PORT" \
      --mb "$MB" --samples "$SAMPLES" --lanes "$LANES" --challenges "$CHALLENGES" --warmup "$WARMUP" \
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

echo "geometry: ${MB} MiB buffer, samples=$SAMPLES, lanes=$LANES, depth=$((SAMPLES / LANES)), challenges=$CHALLENGES, fold=$FOLD"
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
