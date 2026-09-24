#!/usr/bin/env bash
# gpu_teardown.sh — the last command to run on a rented GPU box, before you
# release it.
#
# A rented pod disappears with everything on it. The point of this script is
# that nothing you paid for is left behind when it does, and that the box is
# quiet before you stop it:
#
#   1. Stop everything holding the card. A prover left listening keeps HBM
#      allocated, and a half-finished run left behind looks like a result.
#   2. Report what is on the card afterwards, so "the box is idle" is something
#      you read rather than assume.
#   3. Pack results/ into one archive and print the exact command to pull it
#      back. Measurements are the only thing on the pod worth money.
#
# It does not terminate the pod. Releasing rented hardware is a decision, not a
# side effect of running a script, and the provider API is the place for it.
#
# Usage:  bash scripts/gpu_teardown.sh [--keep-running]
# Exit:   0 the box is quiet and results are packed, 1 something still holds the
#         card, 2 there were no results to pack.
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

# --- 1. stop everything that holds the card ---------------------------------
say "processes"
if [ "$KEEP" -eq 1 ]; then
  echo "--keep-running: leaving the bridge and any provers alone"
else
  for pat in bridge_gpu.py prover_cuda cheating_prover_cuda; do
    if pgrep -f "$pat" >/dev/null 2>&1; then
      pkill -f "$pat" >/dev/null 2>&1
      echo "stopped: $pat"
    fi
  done
  sleep 2
  # A prover that ignored SIGTERM still holds its allocation, so escalate.
  for pat in bridge_gpu.py prover_cuda cheating_prover_cuda; do
    if pgrep -f "$pat" >/dev/null 2>&1; then
      pkill -9 -f "$pat" >/dev/null 2>&1
      echo "killed:  $pat"
    fi
  done
  sleep 1
fi

LEFT=$(pgrep -af 'bridge_gpu.py|prover_cuda|cheating_prover_cuda' 2>/dev/null | grep -v pgrep)
if [ -n "$LEFT" ]; then
  echo "STILL RUNNING:"; echo "$LEFT"
  STATUS=1
else
  echo "none of ours left running"
fi

# --- 2. is the card actually idle -------------------------------------------
say "card"
if command -v nvidia-smi >/dev/null 2>&1; then
  nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu,temperature.gpu \
             --format=csv,noheader
  USED=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null | head -1)
  # Anything above a few hundred MiB means something still has a context open.
  if [ -n "$USED" ] && [ "$USED" -gt 512 ] 2>/dev/null; then
    echo "WARNING: ${USED} MiB still allocated — another process holds a context"
    nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv 2>/dev/null
    STATUS=1
  fi
else
  echo "nvidia-smi not found"
fi

# --- 3. pack what is worth keeping ------------------------------------------
say "results"
if [ ! -d results ] || [ -z "$(ls -A results 2>/dev/null)" ]; then
  echo "results/ is empty — nothing to pack"
  [ "$STATUS" -eq 0 ] && STATUS=2
else
  du -sh results 2>/dev/null
  find results -maxdepth 2 -type d -newermt '-1 day' 2>/dev/null | head -20
  STAMP=$(date -u +%Y%m%dT%H%M%SZ)
  HOST=$(hostname -s 2>/dev/null || echo pod)
  ARCHIVE="/root/memchal-results-$HOST-$STAMP.tar.gz"
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

say "verdict"
case "$STATUS" in
  0) echo "the box is quiet and results are packed — safe to stop or terminate the pod" ;;
  1) echo "something still holds the card — look above before releasing the pod" ;;
  2) echo "the box is quiet, but there is nothing to pull back" ;;
esac
exit "$STATUS"
