#!/usr/bin/env bash
# Runs gpu_teardown.sh on the pod in .gpu_demo_state, pulls its results back, closes the tunnel, and releases the pod.
# Usage: bash runpod/gpu_demo_down.sh [--stop | --keep-pod] [--force]
#   --stop      stop instead of terminate; a stopped pod still bills for disk and may not get its GPU back
#   --keep-pod  pull results and leave the pod running
#   --force     release the pod even when its results cannot be pulled
# Requires RUNPOD_API_KEY. Exit: 0 everything is down, 1 no demo recorded, 2 the pod is still up.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
STATE="$ROOT/.gpu_demo_state"
. runpod/env.sh

ACTION=terminate
FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --stop) ACTION=stop; shift ;;
    --keep-pod) ACTION=keep; shift ;;
    --force) FORCE=1; shift ;;
    -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

say() { printf '\n== %s ==\n' "$1"; }
API="https://api.runpod.io/v2"
# Runs a command and kills it with SIGALRM after N seconds: deadline N CMD...
deadline() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }

if [ ! -f "$STATE" ]; then
  echo "No demo recorded in .gpu_demo_state — nothing of mine is up."
  echo "If you rented something by hand, release it in the RunPod console."
  exit 1
fi
# shellcheck disable=SC1090
. "$STATE"
: "${POD_ID:?state file has no POD_ID}"
SSH_KEY="${SSH_KEY:-${MEMCHAL_SSH_KEY:-}}"
echo "pod:        $POD_ID"
echo "created:    ${CREATED:-unknown}"
echo "local port: ${LPORT:-8777}"

if [ -n "${RUNPOD_API_KEY:-}" ]; then
  GONE=$(curl -sS -o /dev/null -w '%{http_code}' "$API/pods/$POD_ID" \
           -H "Authorization: Bearer $RUNPOD_API_KEY" 2>/dev/null)
  if [ "$GONE" = "404" ]; then
    say "pod"
    echo "  $POD_ID no longer exists — it terminated itself or was released elsewhere"
    PIDS=$(lsof -ti:"${LPORT:-8777}" 2>/dev/null)
    [ -n "$PIDS" ] && echo "$PIDS" | xargs kill 2>/dev/null
    rm -f "$STATE"
    echo "  tunnel closed, state cleared. Results not pulled earlier are gone."
    exit 0
  fi
fi

SSH=""
if [ -n "${SSH_HOST:-}" ] && [ -n "${SSH_PORT:-}" ]; then
  SSH="ssh -i $SSH_KEY -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 -p $SSH_PORT root@$SSH_HOST"
fi

REACHED=0
if [ -n "$SSH" ] && deadline 30 $SSH true 2>/dev/null; then
  REACHED=1
  say "teardown on the pod"
  ( deadline 180 $SSH 'cd /root/memchal && bash runpod/gpu_teardown.sh' 2>&1 ) 2>/dev/null | sed 's/^/  /'
  TD_RC=${PIPESTATUS[0]}
  [ "$TD_RC" -eq 142 ] && echo "  teardown did not finish in 180 s — the card is probably wedged"

  say "pulling results back"
  ARCHIVE=$(deadline 30 $SSH 'ls -t /root/memchal-results-*.tar.gz 2>/dev/null | head -1' 2>/dev/null | tr -d '\r')
  if [ -n "$ARCHIVE" ]; then
    mkdir -p "$ROOT/results"
    if deadline 300 scp -q -i "$SSH_KEY" -o IdentitiesOnly=yes \
           -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           -P "$SSH_PORT" "root@$SSH_HOST:$ARCHIVE" "$ROOT/results/"; then
      LOCAL="$ROOT/results/$(basename "$ARCHIVE")"
      echo "  saved: results/$(basename "$ARCHIVE")  ($(du -h "$LOCAL" | cut -f1))"
      tar xzf "$LOCAL" -C "$ROOT/results" --strip-components=1 2>/dev/null \
        && echo "  unpacked into results/"
    else
      echo "  COULD NOT COPY THE ARCHIVE — not releasing the pod."
      echo "  Pull it by hand, then rerun:"
      echo "    scp -P $SSH_PORT root@$SSH_HOST:$ARCHIVE results/"
      exit 2
    fi
  else
    echo "  no results archive on the pod — copying results/ directly"
    DEST="$ROOT/results/pod-$POD_ID"
    mkdir -p "$DEST"
    if deadline 300 rsync -az -e "ssh -i $SSH_KEY -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p $SSH_PORT" \
         "root@$SSH_HOST:/root/memchal/results/" "$DEST/" 2>/dev/null; then
      echo "  saved: results/pod-$POD_ID/  ($(find "$DEST" -type f | wc -l | tr -d ' ') files)"
    else
      echo "  COULD NOT COPY results/ — not releasing the pod."
      echo "  Rerun with --force to release it and lose what is on it."
      [ "$FORCE" -eq 1 ] || exit 2
    fi
  fi
else
  say "pod unreachable"
  echo "  cannot reach ${SSH_HOST:-?}:${SSH_PORT:-?} over ssh."
  if [ "$FORCE" -ne 1 ]; then
    echo "  Anything still on it will be lost when it is released."
    echo "  Rerun with --force to release it anyway."
    exit 2
  fi
  echo "  --force: releasing without pulling results"
fi

say "local port"
PORT_TO_FREE="${LPORT:-8777}"
PIDS=$(lsof -ti:"$PORT_TO_FREE" 2>/dev/null)
if [ -n "$PIDS" ]; then
  echo "$PIDS" | xargs kill 2>/dev/null
  sleep 1
  STILL=$(lsof -ti:"$PORT_TO_FREE" 2>/dev/null)
  if [ -n "$STILL" ]; then echo "$STILL" | xargs kill -9 2>/dev/null; sleep 1; fi
fi
if lsof -ti:"$PORT_TO_FREE" >/dev/null 2>&1; then
  echo "  port $PORT_TO_FREE still has a listener — not one of ours?"
else
  echo "  port $PORT_TO_FREE is free"
fi

say "pod"
if [ "$ACTION" = keep ]; then
  echo "  --keep-pod: $POD_ID is still RUNNING and still billing"
  echo "  release it later with: bash runpod/gpu_demo_down.sh"
  exit 0
fi
if [ -z "${RUNPOD_API_KEY:-}" ]; then
  echo "  RUNPOD_API_KEY is not set, so I cannot release $POD_ID."
  echo "  Set it and rerun, or $ACTION the pod in the RunPod console."
  exit 2
fi
if [ "$ACTION" = terminate ]; then
  curl -sS -o /dev/null -X DELETE "$API/pods/$POD_ID" -H "Authorization: Bearer $RUNPOD_API_KEY"
else
  curl -sS -o /dev/null -X POST "$API/pods/$POD_ID/action" \
       -H "Authorization: Bearer $RUNPOD_API_KEY" -H "Content-Type: application/json" \
       -d '{"action":"stop"}'
fi
sleep 3
CODE=$(curl -sS -o /tmp/gpu_demo_pod.json -w '%{http_code}' "$API/pods/$POD_ID" \
            -H "Authorization: Bearer $RUNPOD_API_KEY" 2>/dev/null)
if [ "$CODE" = "404" ]; then
  STATUS=GONE
else
  STATUS=$(python3 runpod/runpod_json.py get status < /tmp/gpu_demo_pod.json 2>/dev/null)
  STATUS=${STATUS:-UNKNOWN}
fi
rm -f /tmp/gpu_demo_pod.json
case "$ACTION:$STATUS" in
  terminate:GONE|terminate:TERMINATED)
    echo "  terminated — $POD_ID no longer exists"; rm -f "$STATE" ;;
  stop:EXITED|stop:STOPPED)
    echo "  stopped — disk kept, GPU released (may not restart)"; rm -f "$STATE" ;;
  *) echo "  pod reports '$STATUS' (HTTP $CODE) after $ACTION — check the console"
     echo "  keeping .gpu_demo_state so you can rerun"; exit 2 ;;
esac

say "down"
echo "nothing of mine is running. Results, if any, are in results/."
[ "$REACHED" -eq 0 ] && echo "NOTE: the pod was unreachable, so its results were not rescued."
exit 0
