#!/usr/bin/env bash
# gpu_weights_session.sh — the weights challenge on one GPU, inside vLLM (ADR 0009).
#
# Runs ON a rented box, from the tree gpu_weights_up.sh shipped, detached so a
# closed laptop doesn't stop it. Everything lands in results/weights-<stamp>/,
# which `gpu_demo_down.sh --state .gpu_weights_state` packs and copies home.
#
# The steps, each of which makes the next one worth paying for:
#
#   1. The gate: record the machine, build, and pass the GPU known-answer test,
#      which now covers attached pieces on a stream of their own and the tree
#      fold. A box that fails it measures nothing.
#   2. vLLM 0.20.2 and this repo's plugin in a venv, the stock int8 checkpoint,
#      and its manifest.
#   3. Baseline: the weights in memchal's own allocations (prover_cuda
#      --weights), no engine involved.
#   4. vLLM with the plugin, idle. The first start also compares every tensor
#      in GPU memory with the checkpoint, byte for byte.
#   5. vLLM under a serving load, with and without challenges arriving at
#      random intervals: what the challenge costs inference, and what
#      inference does to the honest timing.
#   6. vLLM's own --cpu-offload-gb as the cheat: part of the weights in host
#      memory, read over PCIe. Once with the honest self-check (it must
#      refuse), then served anyway at three sizes, with an honest baseline
#      measured between them.
#
# Settings (environment): MODEL_ID, MODEL_DIR, N (challenges per point),
# OFFLOADS (GB to offload, space-separated), SKIP_INSTALL=1, ENFORCE_EAGER=1.
# Exit: 0 done, 1 the gate or setup failed, 2 a measurement step failed.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
export PATH="/usr/local/cuda/bin:$HOME/.local/bin:$PATH"

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="results/weights-$STAMP"
mkdir -p "$OUT"
MODEL_ID="${MODEL_ID:-RedHatAI/Meta-Llama-3.1-8B-Instruct-quantized.w8a8}"
MODEL_DIR="${MODEL_DIR:-/root/model}"
VENV=/root/venv
N="${N:-400}"
OFFLOADS="${OFFLOADS:-0.33 0.65 1.3}"
MC_PORT=9100
VLLM_PORT=8000
GEOM=(--k 262144 --lanes 16384 --fold tree)
MANIFEST="$OUT/model.weights"

say()  { printf '\n== %s ==  %s\n' "$1" "$(date -u +%T)"; }
fail() { printf '\nFAILED: %s\n' "$1" >&2; echo "$1" > "$OUT/FAILED"; exit "${2:-2}"; }
wait_port() { for _ in $(seq 1 "${2:-600}"); do (echo > "/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 1; done; return 1; }

# --- 1. the gate --------------------------------------------------------------
say "gate"
make -j8 > "$OUT/make.log" 2>&1 || fail "the CPU build failed (see make.log)" 1
bash scripts/gpu_pod_setup.sh > "$OUT/gate.log" 2>&1
tail -3 "$OUT/gate.log"
./tests/known_answer_gpu > "$OUT/known_answer_gpu.log" 2>&1
grep -q "all vectors PASS" "$OUT/known_answer_gpu.log" \
  || fail "the GPU known-answer test did not pass — nothing this box measures can be trusted" 1
grep -c "PASS" "$OUT/known_answer_gpu.log" | sed 's/^/known-answer checks passed: /'
[ -f libmemchal_weights.so ] || fail "libmemchal_weights.so was not built" 1
cp results/machine.json "$OUT/" 2>/dev/null

# --- 2. vLLM, the plugin, the model -------------------------------------------
say "vLLM and the model"
if [ "${SKIP_INSTALL:-0}" != 1 ]; then
  command -v uv >/dev/null || curl -LsSf https://astral.sh/uv/install.sh | sh > "$OUT/uv.log" 2>&1
  uv venv -q -p 3.12 "$VENV" > "$OUT/install.log" 2>&1 || fail "could not make the venv" 1
  UV_TORCH_BACKEND=cu130 uv pip install -p "$VENV" "vllm==0.20.2" "huggingface_hub[cli]" >> "$OUT/install.log" 2>&1 \
    || fail "could not install vLLM 0.20.2 (see install.log)" 1
  uv pip install -p "$VENV" ./vllm_plugin >> "$OUT/install.log" 2>&1 || fail "could not install the plugin" 1
fi
# shellcheck disable=SC1091
. "$VENV/bin/activate"
python -c "import torch, vllm; print('torch', torch.__version__, 'cuda', torch.version.cuda, '| vllm', vllm.__version__)" \
  | tee "$OUT/versions.txt"
python -c "import importlib.metadata as m; print([e.name for e in m.entry_points(group='vllm.general_plugins')])" \
  | tee -a "$OUT/versions.txt"
if [ ! -f "$MODEL_DIR/config.json" ]; then
  hf download "$MODEL_ID" --local-dir "$MODEL_DIR" > "$OUT/download.log" 2>&1 || fail "could not download $MODEL_ID" 1
fi
python3 scripts/weights_manifest.py "$MANIFEST" "$MODEL_DIR"/*.safetensors | tee "$OUT/manifest.txt" \
  || fail "could not write the manifest" 1

# challenge <name> [verifier args...]: one verifier run against whatever serves on MC_PORT.
challenge() {
  local name="$1"; shift
  ./verifier --port "$MC_PORT" --weights "$MANIFEST" --upload no "${GEOM[@]}" \
    --n "$N" --warmup 50 --csv "$OUT/$name.csv" --json "$OUT/$name.json" --label "$name" "$@" \
    > "$OUT/$name.verifier.log" 2>&1
  local rc=$?
  python3 - "$OUT/$name.json" "$name" "$rc" <<'EOF'
import json, sys
path, name, rc = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    r = json.load(open(path))["result"]
    p = r["percentiles_us"]
    print(f"{name:<22} p50 {p['p50']['value']:9.1f} us   p99 {p['p99']['value']:9.1f} us   "
          f"errors {r['errors']}/{r['samples']}")
except Exception:
    print(f"{name:<22} no result (verifier exit {rc})")
EOF
  return $rc
}

# --- 3. baseline: memchal's own allocations -----------------------------------
say "baseline: prover_cuda --weights"
./prover_cuda --weights "$MANIFEST" --trace 50 "$MC_PORT" > "$OUT/prover_cuda.log" 2>&1 &
PC=$!
wait_port "$MC_PORT" 300 || fail "prover_cuda never listened (see prover_cuda.log)"
challenge standalone
kill "$PC" 2>/dev/null; wait "$PC" 2>/dev/null

# --- vLLM helpers -------------------------------------------------------------
VLLM_PID=""
start_vllm() {  # start_vllm <name> "<extra plugin env, or empty>" [extra vllm args...]
  local name="$1" extra_env="$2"; shift 2
  local eager=(); [ "${ENFORCE_EAGER:-0}" = 1 ] && eager=(--enforce-eager)
  # shellcheck disable=SC2086  # extra_env is a list of NAME=value words
  env MEMCHAL_WEIGHTS_PORT="$MC_PORT" MEMCHAL_WEIGHTS_MANIFEST="$MANIFEST" MEMCHAL_LIB="$ROOT/libmemchal_weights.so" \
      MEMCHAL_TRACE_EVERY=50 MEMCHAL_TRACE="$OUT/$name.trace" $extra_env \
    vllm serve "$MODEL_DIR" --port "$VLLM_PORT" --max-model-len 4096 --gpu-memory-utilization 0.85 \
      "${eager[@]}" "$@" > "$OUT/$name.vllm.log" 2>&1 &
  VLLM_PID=$!
  for _ in $(seq 1 900); do
    curl -sf "localhost:$VLLM_PORT/health" >/dev/null && return 0
    kill -0 "$VLLM_PID" 2>/dev/null || return 1
    sleep 1
  done
  return 1
}
stop_vllm() {
  [ -n "$VLLM_PID" ] || return 0
  kill "$VLLM_PID" 2>/dev/null
  for _ in $(seq 1 60); do kill -0 "$VLLM_PID" 2>/dev/null || break; sleep 1; done
  kill -9 "$VLLM_PID" 2>/dev/null; pkill -9 -f "vllm serve" 2>/dev/null
  wait "$VLLM_PID" 2>/dev/null; VLLM_PID=""
  # The card must be empty before the next start, or its numbers mix with this one's.
  for _ in $(seq 1 60); do
    [ -z "$(nvidia-smi --query-compute-apps=pid --format=csv,noheader 2>/dev/null)" ] && return 0
    sleep 1
  done
  echo "WARNING: the card still has a process on it"
}
trap stop_vllm EXIT
memchal_lines() { grep -h "memchal" "$OUT/$1.vllm.log" | sed 's/^/    /' | tail -8; }
bench() {  # bench <name>: the serving load pouw measured, 600 prompts of 2048 tokens at 3/s
  mkdir -p "$OUT/bench-$1"
  vllm bench serve --model "$MODEL_DIR" --dataset-name random \
    --random-input-len 2048 --random-output-len 64 --num-prompts 600 --request-rate 3 \
    --save-result --result-dir "$OUT/bench-$1" > "$OUT/$1.bench.log" 2>&1
  grep -E "Request throughput|Mean TTFT|Mean TPOT" "$OUT/$1.bench.log" | sed "s/^/    $1: /"
}

# --- 4. vLLM with the plugin, idle, and the byte check -------------------------
say "vLLM with the plugin, idle"
start_vllm honest-1 "MEMCHAL_CHECK_BYTES=1" || { memchal_lines honest-1; fail "vLLM did not start (see honest-1.vllm.log)"; }
memchal_lines honest-1
grep -q "every tensor holds the checkpoint's bytes" "$OUT/honest-1.vllm.log" \
  || fail "the byte check did not pass, so the manifest does not describe what vLLM holds"
wait_port "$MC_PORT" 60 || fail "the plugin is not listening (see honest-1.vllm.log)"
challenge vllm-idle

# --- 5. under load ------------------------------------------------------------
say "vLLM under load"
bench load-alone
bench load-challenged &
B=$!
sleep 20
# Challenges at random times, about 10 a second: the defense against a
# prover that swaps the weights out between challenges (gpu-port, --gap-dist).
challenge vllm-load --n $(( N * 2 )) --gap-us 100000 --gap-dist exp
wait "$B"
challenge vllm-idle-after
stop_vllm

# --- 6. vLLM's own CPU offload as the cheat -----------------------------------
say "the self-check refuses offloaded weights"
FIRST=${OFFLOADS%% *}
start_vllm refuse "" --cpu-offload-gb "$FIRST" || fail "vLLM did not start with --cpu-offload-gb $FIRST"
memchal_lines refuse
if wait_port "$MC_PORT" 30; then
  echo "    NOTE: the plugin served offloaded weights without being told to — check refuse.vllm.log"
else
  echo "    refused, as it should: nothing listens on $MC_PORT"
fi
stop_vllm

i=1
for GB in $OFFLOADS; do
  say "offload $GB GB, served anyway"
  start_vllm "offload-$GB" "MEMCHAL_ALLOW_UNLOCKED=1" --cpu-offload-gb "$GB" \
    || fail "vLLM did not start with --cpu-offload-gb $GB"
  memchal_lines "offload-$GB"
  wait_port "$MC_PORT" 60 || fail "the plugin is not listening (see offload-$GB.vllm.log)"
  challenge "offload-$GB"
  stop_vllm
  i=$((i + 1))
  say "honest baseline $i"
  start_vllm "honest-$i" "" || fail "vLLM did not start (honest-$i)"
  wait_port "$MC_PORT" 60 || fail "the plugin is not listening (see honest-$i.vllm.log)"
  challenge "vllm-honest-$i"
  stop_vllm
done

# --- summary -------------------------------------------------------------------
say "summary"
python3 - "$OUT" <<'EOF' | tee "$OUT/summary.txt"
import glob, json, os, statistics, sys
out = sys.argv[1]
rows = {}
for p in sorted(glob.glob(os.path.join(out, "*.json"))):
    try:
        d = json.load(open(p))
        r = d["result"]
        rows[d["label"]] = (r["percentiles_us"]["p50"]["value"], r["errors"], r["samples"])
    except Exception:
        pass
honest = [v[0] for k, v in rows.items() if k.startswith("vllm-honest") or k == "vllm-idle"]
base = statistics.median(honest) if honest else None
spread = statistics.pstdev(honest) if len(honest) > 1 else 0.0
print(f"honest vLLM baselines: {len(honest)}, median p50 {base:.1f} us, spread {spread:.1f} us" if base else "no honest baseline")
for k, (p50, e, s) in rows.items():
    ratio = f"{p50 / base:5.2f}x" if base else "  n/a"
    print(f"  {k:<22} p50 {p50:9.1f} us  {ratio}  errors {e}/{s}")
for name in ("load-alone", "load-challenged"):
    for p in glob.glob(os.path.join(out, f"bench-{name}", "*.json")):
        b = json.load(open(p))
        print(f"  {name:<22} {b.get('request_throughput', 0):.2f} req/s, mean TTFT {b.get('mean_ttft_ms', 0):.0f} ms")
EOF
touch "$OUT/DONE"
echo "done: $OUT"
