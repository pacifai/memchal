#!/usr/bin/env bash
# Records the card in results/machine.json, builds everything, and runs the known-answer gate.
# Usage: bash runpod/gpu_pod_setup.sh [--device N]
# Exit: 0 everything passed, 1 the box is unusable, 2 the build or gate failed.
set -uo pipefail
cd "$(dirname "$0")/.."
export PATH=/usr/local/cuda/bin:$PATH

DEVICE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

fatal() { printf '\nFATAL: %s\n' "$1" >&2; exit 1; }
say() { printf '\n== %s ==\n' "$1"; }
smi() { nvidia-smi -i "$DEVICE" --query-gpu="$1" --format=csv,noheader,nounits 2>/dev/null; }

say "card"
command -v nvidia-smi >/dev/null 2>&1 || fatal "nvidia-smi not found; no driver is visible in this container"
NAME=$(smi name)
[ -n "$NAME" ] || fatal "device $DEVICE not present"
mkdir -p results
cat > results/machine.json <<JSON
{
  "recorded_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "gpu": "$NAME",
  "gpu_memory_mib": "$(smi memory.total)",
  "driver": "$(smi driver_version)"
}
JSON
cat results/machine.json

say "build"
for tool in nvcc make g++; do
  command -v "$tool" >/dev/null 2>&1 || fatal "$tool not found; the image needs the CUDA compiler and a C++ toolchain"
done
make -j"$(nproc)" all gpu || { echo "BUILD FAILED" >&2; exit 2; }

say "known-answer gate"
if build/tests/gpu/answer_kat --device "$DEVICE"; then
  echo "GATE PASSED"
else
  echo "GATE FAILED: do not take any timing number from this build." >&2
  exit 2
fi
