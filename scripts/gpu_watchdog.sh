#!/usr/bin/env bash
# gpu_watchdog.sh — terminate this pod when nobody is using it.
#
# Runs ON the pod, detached, and needs nothing from the laptop that rented it.
# A pod left up with its tunnel open once billed for 27 hours with no runs, so
# an open SSH session is deliberately not counted as use.
#
# The pod counts as busy while either is true:
#   - a verifier process is running (a demo run or a sweep), or
#   - a file under results/ changed in the last IDLE_MIN minutes.
# After IDLE_MIN minutes with neither, it terminates the pod. Anything still on
# the pod is lost with it; by then the last run is at least IDLE_MIN old.
#
# It uses the pod-scoped API key RunPod puts in every pod's environment. SSH
# sessions do not inherit that environment, so it is read from PID 1. The key
# is never printed.
#
# The hard cap (MAX_HOURS from boot) is enforced separately by the pod's boot
# command, so it holds even if this script never starts.
#
# Usage: setsid bash scripts/gpu_watchdog.sh [IDLE_MIN] > /root/watchdog.log 2>&1 &
#   --check   confirm the key and pod id are present, then exit (0 ok, 1 not)
set -uo pipefail
cd "$(dirname "$0")/.."

envof() { tr '\0' '\n' < /proc/1/environ 2>/dev/null | sed -n "s/^$1=//p" | head -1; }
POD_ID=$(envof RUNPOD_POD_ID)
KEY=$(envof RUNPOD_API_KEY)

if [ "${1:-}" = "--check" ]; then
  [ -n "$POD_ID" ] || { echo "no RUNPOD_POD_ID in the pod environment"; exit 1; }
  [ -n "$KEY" ]    || { echo "no RUNPOD_API_KEY in the pod environment"; exit 1; }
  echo "pod id and pod-scoped key present"
  exit 0
fi

IDLE_MIN="${1:-90}"
STATUS=/root/watchdog.status

# Try every way RunPod offers, because which one a pod-scoped key may use has
# not been confirmed. Success means the pod is going away, so the loop below
# just keeps retrying until it does.
terminate() {
  echo "$(date -u +%FT%TZ) terminating $POD_ID: $1"
  if command -v runpodctl >/dev/null; then
    RUNPOD_API_KEY="$KEY" runpodctl pod delete "$POD_ID" 2>&1 | tail -1
    RUNPOD_API_KEY="$KEY" runpodctl remove pod "$POD_ID" 2>&1 | tail -1
  fi
  for url in "https://rest.runpod.io/v1/pods/$POD_ID" "https://api.runpod.io/v2/pods/$POD_ID"; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$url" \
             -H "Authorization: Bearer $KEY" 2>/dev/null)
    echo "  DELETE $url -> $code"
  done
}

[ -n "$POD_ID" ] && [ -n "$KEY" ] || { echo "no pod id or key; watchdog cannot act"; exit 1; }
echo "$(date -u +%FT%TZ) watching $POD_ID; idle limit ${IDLE_MIN} min"

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
  echo "idle ${idle} of ${IDLE_MIN} min" > "$STATUS"
  if [ "$idle" -ge "$IDLE_MIN" ]; then
    terminate "idle for ${idle} min"
  fi
  sleep 60
done
