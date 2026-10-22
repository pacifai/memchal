#!/usr/bin/env bash
# gpu_demo_up.sh — rent a GPU, put the demo on it, and open it in your browser.
#
# One command from nothing to a browsable demo. It rents a card, ships this
# working tree to it, builds, runs the known-answer gate, starts the bridge,
# and forwards the port. If any step fails it stops rather than leaving you a
# pod you forgot about, and it records what it created in a state file so
# gpu_demo_down.sh can undo exactly that and nothing else.
#
# It never reuses a pod it did not create. A pod you started by hand is yours
# to manage.
#
# Requires: RUNPOD_API_KEY in the environment (get one from
#   https://console.runpod.io/user/settings) and an SSH key registered on that
#   account. Set the key in your shell or a .env you source; this script only
#   ever reads it by name.
#
# Usage:
#   bash scripts/gpu_demo_up.sh                      # H100 SXM, 60 GB disk
#   bash scripts/gpu_demo_up.sh --gpu "NVIDIA A40"   # a cheaper card
#   bash scripts/gpu_demo_up.sh --port 8888          # a different local port
#   bash scripts/gpu_demo_up.sh --yes                # skip the price prompt
#   bash scripts/gpu_demo_up.sh --idle-min 30 --max-hours 3
#
# The pod guards its own bill, so a closed laptop cannot leave it running:
#   --max-hours H  the pod terminates itself H hours after boot, whatever it is
#                  doing (default 6). Armed in the boot command, before any
#                  other step, so it holds even if this script dies halfway.
#   --idle-min M   the pod terminates itself after M minutes with no demo run
#                  and no verifier running (default 90). An open tunnel does
#                  not count as use. See scripts/gpu_watchdog.sh.
#
# Exit: 0 the demo is up, 1 a prerequisite is missing, 2 renting failed,
#       3 the box is unusable or the gate failed.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
STATE="$ROOT/.gpu_demo_state"

GPU="NVIDIA H100 80GB HBM3"
DISK=60
LPORT=8777
CLOUD=SECURE
IMAGE="nvidia/cuda:12.8.0-devel-ubuntu22.04"
ASSUME_YES=0
MAX_HOURS=6
IDLE_MIN=90
SSH_KEY="${MEMCHAL_SSH_KEY:-$HOME/.ssh/neta-pacifai-runpod}"

while [ $# -gt 0 ]; do
  case "$1" in
    --gpu)  GPU="$2"; shift 2 ;;
    --disk) DISK="$2"; shift 2 ;;
    --port) LPORT="$2"; shift 2 ;;
    --cloud) CLOUD="$2"; shift 2 ;;
    --image) IMAGE="$2"; shift 2 ;;
    --key)  SSH_KEY="$2"; shift 2 ;;
    --yes|-y) ASSUME_YES=1; shift ;;
    --max-hours) MAX_HOURS="$2"; shift 2 ;;
    --idle-min)  IDLE_MIN="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

API="https://api.runpod.io/v2"
say()  { printf '\n== %s ==\n' "$1"; }
die()  { printf '\nFAILED: %s\n' "$1" >&2; exit "${2:-1}"; }
api()  { # api METHOD PATH [BODY]
  local m="$1" p="$2" b="${3:-}"
  if [ -n "$b" ]; then
    curl -sS -X "$m" "$API$p" \
      -H "Authorization: Bearer $RUNPOD_API_KEY" \
      -H "Content-Type: application/json" -d "$b"
  else
    curl -sS -X "$m" "$API$p" -H "Authorization: Bearer $RUNPOD_API_KEY"
  fi
}
jget() { python3 scripts/runpod_json.py get "$1" 2>/dev/null; }

# --- 0. prerequisites -------------------------------------------------------
say "prerequisites"
[ -n "${RUNPOD_API_KEY:-}" ] || die "RUNPOD_API_KEY is not set. Export it, or source the .env that holds it." 1
command -v curl   >/dev/null || die "curl is not installed" 1
command -v python3>/dev/null || die "python3 is not installed" 1
command -v rsync  >/dev/null || die "rsync is not installed" 1
[ -f "$SSH_KEY" ] || die "no SSH private key at $SSH_KEY (set MEMCHAL_SSH_KEY or pass --key)" 1
if [ -f "$STATE" ]; then
  # shellcheck disable=SC1090
  . "$STATE"
  die "a demo is already up (pod ${POD_ID:-?}). Run scripts/gpu_demo_down.sh first." 1
fi
if lsof -ti:"$LPORT" >/dev/null 2>&1; then
  die "local port $LPORT is already in use. Free it, or pass --port." 1
fi
echo "ssh key:    $SSH_KEY"
echo "local port: $LPORT"

# --- 1. price, and consent --------------------------------------------------
say "price"
# The catalog ignores a gpuTypeIds filter and returns every card, so find ours
# by id. include=AVAILABILITY&product=POD is what adds the stock field. An id
# that is not in the list is a typo in --gpu, and renting would fail anyway.
CAT=$(curl -sS "$API/catalog/gpus?include=AVAILABILITY&product=POD" \
        -H "Authorization: Bearer $RUNPOD_API_KEY")
INFO=$(printf '%s' "$CAT" | python3 scripts/runpod_json.py catalog "$GPU" "$CLOUD" 2>&1)
case "$INFO" in ERR\|*) die "${INFO#ERR|}" 1 ;; esac
IFS='|' read -r GPU_NAME PRICE STOCK <<< "$INFO"
if [ -z "${PRICE:-}" ] || [ "$PRICE" = "?" ]; then
  printf '%s\n' "$INFO" | tail -3
  die "could not read a price for $GPU on $CLOUD cloud — not renting blind" 2
fi
echo "$GPU_NAME ($GPU), $CLOUD cloud — \$$PRICE/hr, stock: $STOCK"
case "$STOCK" in
  NONE|UNAVAILABLE|none|unavailable) die "no stock right now. Try --gpu with another card." 2 ;;
esac
if [ "$ASSUME_YES" -ne 1 ]; then
  printf '\nRent it? This bills by the second until you run gpu_demo_down.sh. [y/N] '
  read -r ans
  case "$ans" in y|Y|yes|YES) ;; *) echo "nothing rented."; exit 0 ;; esac
fi

# --- 2. rent ----------------------------------------------------------------
say "renting"
case "$MAX_HOURS" in ''|*[!0-9.]*) die "--max-hours takes a number of hours, such as 6 or 0.5" 1 ;; esac
CAP_S=$(awk -v h="$MAX_HOURS" 'BEGIN { printf "%d", h * 3600 }')
case "$IDLE_MIN"  in ''|*[!0-9]*) die "--idle-min takes a whole number of minutes" 1 ;; esac
# The hard cap runs first in the boot command, so it is armed before anything
# else can fail. It uses the pod-scoped key and pod id RunPod puts in every
# pod's environment; the single quotes keep them unexpanded until the pod.
# runpodctl is RunPod's documented way for a pod to act on itself; the curl
# calls are a fallback for an image where it is missing.
CAP='(sleep '"$CAP_S"'; echo "hard cap reached" > /root/watchdog.cap; runpodctl pod delete $RUNPOD_POD_ID; for u in https://rest.runpod.io/v1/pods/$RUNPOD_POD_ID https://api.runpod.io/v2/pods/$RUNPOD_POD_ID; do curl -sS -o /dev/null -X DELETE "$u" -H "Authorization: Bearer $RUNPOD_API_KEY"; done) > /dev/null 2>&1 &'
BOOT=$CAP' apt-get update -q >/dev/null 2>&1; DEBIAN_FRONTEND=noninteractive apt-get install -y -q openssh-server rsync git python3 build-essential curl >/dev/null 2>&1; mkdir -p /run/sshd /root/.ssh; echo "$PUBLIC_KEY" > /root/.ssh/authorized_keys; chmod 700 /root/.ssh; chmod 600 /root/.ssh/authorized_keys; echo "export PATH=/usr/local/cuda/bin:\$PATH" >> /root/.bashrc; touch /root/.ssh_ready; /usr/sbin/sshd -D -p 22'
BODY=$(python3 scripts/runpod_json.py pod-body "$IMAGE" "$GPU" "$DISK" "$CLOUD" "$BOOT")
RESP=$(api POST "/pods" "$BODY")
POD_ID=$(printf '%s' "$RESP" | jget id)
if [ -z "$POD_ID" ]; then
  printf '%s\n' "$RESP" | head -c 600; echo
  die "could not rent a $GPU. Try --gpu with another card, or --cloud COMMUNITY." 2
fi
# Record it NOW, before anything else can fail, so teardown can always find it.
{ echo "POD_ID=$POD_ID"; echo "LPORT=$LPORT"; echo "SSH_KEY=$SSH_KEY";
  echo "CREATED=$(date -u +%Y-%m-%dT%H:%M:%SZ)"; } > "$STATE"
echo "pod $POD_ID  (recorded in .gpu_demo_state)"
trap 'echo; echo "Bring-up failed. The pod is still rented — run scripts/gpu_demo_down.sh"; ' ERR

# --- 3. wait for ssh --------------------------------------------------------
say "waiting for ssh"
HOST=""; PORT=""
for i in $(seq 1 90); do
  P=$(api GET "/pods/$POD_ID")
  HOST=$(printf '%s' "$P" | jget ssh.direct.host)
  PORT=$(printf '%s' "$P" | jget ssh.direct.port)
  [ -n "$HOST" ] && [ -n "$PORT" ] && break
  printf '.'; sleep 10
done
echo
[ -n "$HOST" ] || die "the pod never published a direct SSH endpoint" 2
SSH="ssh -i $SSH_KEY -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 -p $PORT root@$HOST"
{ echo "SSH_HOST=$HOST"; echo "SSH_PORT=$PORT"; } >> "$STATE"
for i in $(seq 1 60); do
  $SSH 'test -f /root/.ssh_ready' 2>/dev/null && break
  printf '.'; sleep 10
done
echo
$SSH 'test -f /root/.ssh_ready' 2>/dev/null || die "ssh never came up on $HOST:$PORT" 2
echo "ssh: root@$HOST -p $PORT"

# --- 4. ship, build, gate ---------------------------------------------------
say "shipping this working tree"
rsync -az --delete \
  --exclude .git --exclude results --exclude __pycache__ --exclude '*.o' \
  --exclude prover --exclude verifier --exclude cheating_prover \
  --exclude prover_cuda --exclude cheating_prover_cuda \
  -e "ssh -i $SSH_KEY -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $PORT" \
  ./ "root@$HOST:/root/memchal/" || die "could not copy the repo to the pod" 3

say "building and gating"
# gpu_pod_setup.sh records the machine, refuses an unusable box, builds the GPU
# targets, and runs the known-answer gate. `make` on its own builds the CPU
# verifier, which the demo also needs.
$SSH 'export PATH=/usr/local/cuda/bin:$PATH; cd /root/memchal && make -j8 >/dev/null 2>&1 && bash scripts/gpu_pod_setup.sh 2>&1 | tail -4'
$SSH 'export PATH=/usr/local/cuda/bin:$PATH; cd /root/memchal && ./tests/known_answer_gpu 2>&1 | tail -1' | grep -q "all vectors PASS" \
  || die "the known-answer gate did not pass on this card — do not trust anything it measures" 3

# --- 5. start the bridge ----------------------------------------------------
say "starting the demo"
# A script file, not a one-liner: see the header of gpu_bridge_start.sh for why
# stopping and starting the bridge in one ssh command kills itself.
$SSH 'bash /root/memchal/scripts/gpu_bridge_start.sh' || die "the bridge did not start" 3

say "arming the idle watchdog"
# One ssh call, and setsid -f so the watchdog leaves this session at once.
# Backgrounding it with `&` instead left a subshell holding the ssh channel,
# and bring-up hung on this step.
WD=$($SSH "cd /root/memchal && bash scripts/gpu_watchdog.sh --check && setsid -f bash scripts/gpu_watchdog.sh $IDLE_MIN > /root/watchdog.log 2>&1 < /dev/null" 2>/dev/null)
if [ $? -eq 0 ]; then
  echo "$WD; terminates after $IDLE_MIN idle minutes, or $MAX_HOURS h after boot"
else
  echo "WARNING: ${WD:-the watchdog check did not answer}"
  echo "The pod cannot terminate itself. Run gpu_demo_down.sh when you are done."
fi

ssh -i "$SSH_KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=30 -o ServerAliveCountMax=4 \
    -p "$PORT" -f -N -L "$LPORT:127.0.0.1:8777" "root@$HOST" || die "could not forward port $LPORT" 3
sleep 3
CODE=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$LPORT/")
[ "$CODE" = "200" ] || die "the page did not answer on http://127.0.0.1:$LPORT (got $CODE)" 3

trap - ERR
say "up"
$SSH 'nvidia-smi --query-gpu=name,memory.total,compute_cap --format=csv,noheader' 2>/dev/null
cat <<EOF

  http://127.0.0.1:$LPORT

  pod $POD_ID is billing until you run:
      bash scripts/gpu_demo_down.sh
  or until it terminates itself: $IDLE_MIN idle minutes, or $MAX_HOURS h after boot.
EOF
