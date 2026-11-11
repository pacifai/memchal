#!/usr/bin/env bash
# Terminates this pod after IDLE_MIN minutes with no verifier running and no change under results/,
# or MAX_HOURS after it starts.
# Usage: setsid bash runpod/gpu_watchdog.sh IDLE_MIN MAX_HOURS > /root/watchdog.log 2>&1 &
#        bash runpod/gpu_watchdog.sh --check   (exit 0 if the pod id and pod-scoped key are present)
set -uo pipefail
cd "$(dirname "$0")/.."

# RunPod sets these for the container's first process, not for ssh sessions.
pid1_env() { tr '\0' '\n' < /proc/1/environ 2>/dev/null | sed -n "s/^$1=//p" | head -1; }
POD_ID=$(pid1_env RUNPOD_POD_ID)
export RUNPOD_API_KEY=$(pid1_env RUNPOD_API_KEY)

if [ "${1:-}" = "--check" ]; then
  [ -n "$POD_ID" ] && [ -n "$RUNPOD_API_KEY" ] && command -v runpodctl >/dev/null
  exit
fi

IDLE_MIN="$1"
DEADLINE=$(( $(date +%s) + $(awk "BEGIN { printf \"%d\", $2 * 3600 }") ))
echo "$(date -u +%FT%TZ) watching $POD_ID: idle limit $IDLE_MIN min, hard cap $2 h"

last=$(date +%s)
while :; do
  now=$(date +%s)
  if pgrep -x verifier >/dev/null; then
    last=$now
  else
    newest=$(find results -type f -printf '%T@\n' 2>/dev/null | sort -n | tail -1 | cut -d. -f1)
    [ -n "$newest" ] && [ "$newest" -gt "$last" ] && last=$newest
  fi
  idle=$(( (now - last) / 60 ))
  if [ "$idle" -ge "$IDLE_MIN" ] || [ "$now" -ge "$DEADLINE" ]; then
    echo "$(date -u +%FT%TZ) terminating: idle $idle min"
    runpodctl remove pod "$POD_ID"
  fi
  sleep 60
done
