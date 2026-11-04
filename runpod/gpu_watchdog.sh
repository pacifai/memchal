#!/usr/bin/env bash
# Terminates this pod after IDLE_MIN minutes (default 90) with no verifier running and no change under results/.
# Usage: setsid bash runpod/gpu_watchdog.sh [IDLE_MIN] > /root/watchdog.log 2>&1 &
#        bash runpod/gpu_watchdog.sh --check   (exit 0 if the pod id and pod-scoped key are present)
set -uo pipefail
cd "$(dirname "$0")/.."

pid1_env() { tr '\0' '\n' < /proc/1/environ 2>/dev/null | sed -n "s/^$1=//p" | head -1; }
POD_ID=$(pid1_env RUNPOD_POD_ID)
KEY=$(pid1_env RUNPOD_API_KEY)

if [ "${1:-}" = "--check" ]; then
  [ -n "$POD_ID" ] || { echo "no RUNPOD_POD_ID in the pod environment"; exit 1; }
  [ -n "$KEY" ]    || { echo "no RUNPOD_API_KEY in the pod environment"; exit 1; }
  echo "pod id and pod-scoped key present"
  exit 0
fi

IDLE_MIN="${1:-90}"
STATUS=/root/watchdog.status

# Asks RunPod to delete this pod through each API it offers.
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
