#!/usr/bin/env bash
# Stops the GPU processes on a pod, packs results/ into ${TEARDOWN_ARCHIVE_DIR:-/root}, and reports whether the card is idle.
# Usage: bash runpod/gpu_teardown.sh [--keep-running]
# Exit: 0 quiet and packed, 1 something still holds the card, 2 no results to pack.
set -uo pipefail
cd "$(dirname "$0")/.."

KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --keep-running) KEEP=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

say() { printf '\n== %s ==\n' "$1"; }
STATUS=0

say "processes"
if [ "$KEEP" -eq 1 ]; then
  echo "--keep-running: leaving the bridge and any provers alone"
else
  for pat in bridge_gpu.py build/gpu/prover build/attacks/gpu/cheating_prover; do
    if pgrep -f "$pat" >/dev/null 2>&1; then
      pkill -f "$pat" >/dev/null 2>&1
      echo "stopped: $pat"
    fi
  done
  sleep 2
  for pat in bridge_gpu.py build/gpu/prover build/attacks/gpu/cheating_prover; do
    if pgrep -f "$pat" >/dev/null 2>&1; then
      pkill -9 -f "$pat" >/dev/null 2>&1
      echo "killed:  $pat"
    fi
  done
  sleep 1
fi

LEFT=$(pgrep -af 'bridge_gpu.py|build/gpu/prover|build/attacks/gpu/cheating_prover' 2>/dev/null | grep -v pgrep)
if [ -n "$LEFT" ]; then
  echo "STILL RUNNING (a process stuck in the GPU driver cannot be killed):"; echo "$LEFT"
  STATUS=1
else
  echo "none of ours left running"
fi

say "results"
if [ ! -d results ] || [ -z "$(ls -A results 2>/dev/null)" ]; then
  echo "results/ is empty — nothing to pack"
  [ "$STATUS" -eq 0 ] && STATUS=2
else
  du -sh results 2>/dev/null
  find results -maxdepth 2 -type d -newermt '-1 day' 2>/dev/null | head -20
  STAMP=$(date -u +%Y%m%dT%H%M%SZ)
  HOST=$(hostname -s 2>/dev/null || echo pod)
  ARCHIVE="${TEARDOWN_ARCHIVE_DIR:-/root}/memchal-results-$HOST-$STAMP.tar.gz"
  if tar czf "$ARCHIVE" results 2>/dev/null; then
    echo
    echo "packed: $ARCHIVE  ($(du -h "$ARCHIVE" | cut -f1))"
    echo
    echo "pull it back with:"
    echo "  scp <pod>:$ARCHIVE ./"
  else
    echo "could not pack results/"
    STATUS=1
  fi
fi

say "card"
# Prints nvidia-smi's output, or returns 1 after 15 s and abandons the call instead of waiting on it.
smi() {
  local out; out=$(mktemp)
  nvidia-smi "$@" > "$out" 2>/dev/null &
  local pid=$! waited=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 1; waited=$((waited + 1))
    if [ "$waited" -ge 15 ]; then rm -f "$out"; return 1; fi
  done
  wait "$pid"; local rc=$?
  cat "$out"; rm -f "$out"; return "$rc"
}
if command -v nvidia-smi >/dev/null 2>&1; then
  if ! smi --query-gpu=name,memory.used,memory.total,utilization.gpu,temperature.gpu \
           --format=csv,noheader; then
    echo "WARNING: nvidia-smi did not answer in 15 s — the driver is likely wedged."
    echo "         Nothing in the container can reset it; release the pod."
    STATUS=1
  else
    USED=$(smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1)
    if [ -n "$USED" ] && [ "$USED" -gt 512 ] 2>/dev/null; then
      echo "WARNING: ${USED} MiB still allocated — another process holds a context"
      smi --query-compute-apps=pid,process_name,used_memory --format=csv
      STATUS=1
    fi
  fi
else
  echo "nvidia-smi not found"
fi

say "verdict"
case "$STATUS" in
  0) echo "the box is quiet and results are packed — safe to stop or terminate the pod" ;;
  1) echo "something still holds the card — look above before releasing the pod" ;;
  2) echo "the box is quiet, but there is nothing to pull back" ;;
esac
exit "$STATUS"
