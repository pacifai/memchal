#!/usr/bin/env bash
# gpu_demo_down.sh — close the demo and stop paying for the card.
#
# The reverse of gpu_demo_up.sh, in the order that cannot lose work:
#
#   1. Run gpu_teardown.sh on the pod, which quiets the card and packs results/.
#   2. Pull that archive back, because the pod is about to disappear with it.
#   3. Close the local port forward.
#   4. Release the pod, and only then forget it.
#
# It acts only on the pod recorded in .gpu_demo_state, so it can never release
# something it did not rent. If a step fails it stops with the state file
# intact and tells you what is still up, rather than half-forgetting a pod that
# is still billing.
#
# The default is to TERMINATE. A stopped RunPod pod keeps billing for its disk
# and does not reliably get its GPU back — a stop here has failed to restart
# for want of a free card on the host. If you want the container disk kept
# anyway, pass --stop and know what you are keeping.
#
# Usage:
#   bash scripts/gpu_demo_down.sh              # pull results, terminate
#   bash scripts/gpu_demo_down.sh --stop       # pull results, stop instead
#   bash scripts/gpu_demo_down.sh --keep-pod   # pull results, leave it running
#   bash scripts/gpu_demo_down.sh --force      # release even if the pod is unreachable
#   bash scripts/gpu_demo_down.sh --state .gpu_weights_state   # the weights session's pod
#
# Exit: 0 everything is down, 1 no demo recorded, 2 the pod is still up.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
STATE="$ROOT/.gpu_demo_state"

ACTION=terminate
FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --stop) ACTION=stop; shift ;;
    --keep-pod) ACTION=keep; shift ;;
    --force) FORCE=1; shift ;;
    --state) STATE="$ROOT/$2"; shift 2 ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

say() { printf '\n== %s ==\n' "$1"; }
API="https://api.runpod.io/v2"
# macOS has no coreutils timeout. A wedged pod can hang any ssh session, and
# this script is what releases a wedged pod, so nothing here may wait forever.
deadline() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }

if [ ! -f "$STATE" ]; then
  echo "No demo recorded in .gpu_demo_state — nothing of mine is up."
  echo "If you rented something by hand, release it in the RunPod console."
  exit 1
fi
# shellcheck disable=SC1090
. "$STATE"
: "${POD_ID:?state file has no POD_ID}"
echo "pod:        $POD_ID"
echo "created:    ${CREATED:-unknown}"
echo "local port: ${LPORT:-8777}"

# The pod may already have terminated itself (gpu_watchdog.sh or the boot-time
# hard cap). Then there is nothing to pull or release: close the tunnel and
# forget it, rather than reporting an unreachable pod that no longer bills.
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
  SSH="ssh -i ${SSH_KEY:-$HOME/.ssh/neta-pacifai-runpod} -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 -p $SSH_PORT root@$SSH_HOST"
fi

# --- 1 and 2. quiet the card, then rescue the measurements ------------------
REACHED=0
if [ -n "$SSH" ] && deadline 30 $SSH true 2>/dev/null; then
  REACHED=1
  say "teardown on the pod"
  # The subshell's 2>/dev/null only swallows the shell's own "Alarm clock" notice
  # when the deadline fires; the teardown's output still comes through the pipe.
  ( deadline 180 $SSH 'cd /root/memchal && bash scripts/gpu_teardown.sh' 2>&1 ) 2>/dev/null | sed 's/^/  /'
  TD_RC=${PIPESTATUS[0]}
  [ "$TD_RC" -eq 142 ] && echo "  teardown did not finish in 180 s — the card is probably wedged"


  say "pulling results back"
  ARCHIVE=$(deadline 30 $SSH 'ls -t /root/memchal-results-*.tar.gz 2>/dev/null | head -1' 2>/dev/null | tr -d '\r')
  if [ -n "$ARCHIVE" ]; then
    mkdir -p "$ROOT/results"
    if deadline 300 scp -q -i "${SSH_KEY:-$HOME/.ssh/neta-pacifai-runpod}" -o IdentitiesOnly=yes \
           -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           -P "$SSH_PORT" "root@$SSH_HOST:$ARCHIVE" "$ROOT/results/"; then
      LOCAL="$ROOT/results/$(basename "$ARCHIVE")"
      echo "  saved: results/$(basename "$ARCHIVE")  ($(du -h "$LOCAL" | cut -f1))"
      # Unpack alongside, so the measurements are usable without a second step.
      tar xzf "$LOCAL" -C "$ROOT/results" --strip-components=1 2>/dev/null \
        && echo "  unpacked into results/"
    else
      echo "  COULD NOT COPY THE ARCHIVE — not releasing the pod."
      echo "  Pull it by hand, then rerun:"
      echo "    scp -P $SSH_PORT root@$SSH_HOST:$ARCHIVE results/"
      exit 2
    fi
  else
    # No archive does not mean nothing to rescue: a teardown cut short by a
    # wedged driver never gets to pack. Copy results/ itself — that needs no
    # GPU — and refuse to release the pod if even that fails.
    echo "  no results archive on the pod — copying results/ directly"
    DEST="$ROOT/results/pod-$POD_ID"
    mkdir -p "$DEST"
    if deadline 300 rsync -az -e "ssh -i ${SSH_KEY:-$HOME/.ssh/neta-pacifai-runpod} -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p $SSH_PORT" \
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

# --- 3. close the tunnel ----------------------------------------------------
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

# --- 4. release the pod -----------------------------------------------------
say "pod"
if [ "$ACTION" = keep ]; then
  echo "  --keep-pod: $POD_ID is still RUNNING and still billing"
  echo "  release it later with: bash scripts/gpu_demo_down.sh"
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
# A deleted pod answers 404 rather than a status field, so read the HTTP code
# first and only look at the body when the pod still exists.
CODE=$(curl -sS -o /tmp/gpu_demo_pod.json -w '%{http_code}' "$API/pods/$POD_ID" \
            -H "Authorization: Bearer $RUNPOD_API_KEY" 2>/dev/null)
if [ "$CODE" = "404" ]; then
  STATUS=GONE
else
  STATUS=$(python3 -c 'import json,sys
try: print(json.load(open(sys.argv[1])).get("status") or "UNKNOWN")
except Exception: print("UNKNOWN")' /tmp/gpu_demo_pod.json 2>/dev/null)
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
