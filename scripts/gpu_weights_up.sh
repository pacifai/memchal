#!/usr/bin/env bash
# gpu_weights_up.sh — rent one H100 for the weights session, and nothing else.
#
# The weights challenge (ADR 0009) runs inside vLLM, and vLLM 0.20.2's default
# wheel is built for CUDA 13.0, so the host's driver must support CUDA 13. This
# asks RunPod for exactly that (allowedCudaVersions, REST API v1), ships this
# working tree, arms the pod's self-termination, and with --run starts
# scripts/gpu_weights_session.sh detached on the pod.
#
# Nothing is rented without a yes at the price prompt. Before it asks, it shows
# the account balance, the card's price per hour, and the worst case: the price
# times the hard cap.
#
# Requires: RUNPOD_API_KEY, from the environment or from a .env file at the
#   repo root (gitignored; this script only reads the key by name and never
#   prints it), and an SSH key pair. The public half is handed to the pod
#   directly, so it does not have to be registered on the RunPod account.
#
# The pod guards its own bill:
#   --max-hours H  terminates itself H hours after boot, whatever it is doing
#                  (default 2). Armed first in the boot command.
#   --idle-min M   terminates itself after M minutes with no verifier running
#                  and nothing new under results/ (default 30).
#
# Usage:
#   bash scripts/gpu_weights_up.sh              # rent, ship, arm; then ask before --run
#   bash scripts/gpu_weights_up.sh --run        # and start the session
#   bash scripts/gpu_weights_up.sh --dry-run    # balance, price, and the request; rent nothing
#
# Then: bash scripts/gpu_demo_down.sh --state .gpu_weights_state  (pull results, terminate)
#
# Exit: 0 up, 1 a prerequisite is missing or you said no, 2 renting failed,
#       3 the pod came up but could not be prepared.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
STATE="$ROOT/.gpu_weights_state"

GPU="NVIDIA H100 80GB HBM3"
CLOUD=SECURE
IMAGE="nvidia/cuda:13.0.3-devel-ubuntu24.04"
DISK=120
MAX_HOURS=2
IDLE_MIN=30
RUN=0
DRY=0
ASSUME_YES=0
SSH_KEY="${MEMCHAL_SSH_KEY:-$HOME/.ssh/id_ed25519}"
MIN_BALANCE=15

while [ $# -gt 0 ]; do
  case "$1" in
    --gpu) GPU="$2"; shift 2 ;;
    --cloud) CLOUD="$2"; shift 2 ;;
    --disk) DISK="$2"; shift 2 ;;
    --key) SSH_KEY="$2"; shift 2 ;;
    --max-hours) MAX_HOURS="$2"; shift 2 ;;
    --idle-min) IDLE_MIN="$2"; shift 2 ;;
    --run) RUN=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --yes|-y) ASSUME_YES=1; shift ;;
    -h|--help) sed -n '2,36p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

REST="https://rest.runpod.io/v1"
V2="https://api.runpod.io/v2"
say()  { printf '\n== %s ==\n' "$1"; }
die()  { printf '\nFAILED: %s\n' "$1" >&2; exit "${2:-1}"; }
jget() { python3 -c 'import json,sys
d=json.load(sys.stdin)
for k in sys.argv[1].split("."):
    if d is None: break
    d = d.get(k) if isinstance(d,dict) else None
print("" if d is None else d)' "$1" 2>/dev/null; }

# --- 0. prerequisites -------------------------------------------------------
say "prerequisites"
if [ -z "${RUNPOD_API_KEY:-}" ] && [ -f "$ROOT/.env" ]; then
  RUNPOD_API_KEY=$(sed -n 's/^\(export \)\{0,1\}RUNPOD_API_KEY=//p' "$ROOT/.env" | tail -1 | tr -d "\"' ")
fi
[ -n "${RUNPOD_API_KEY:-}" ] || die "RUNPOD_API_KEY is not set, in the environment or in $ROOT/.env" 1
for c in curl python3 rsync ssh; do command -v "$c" >/dev/null || die "$c is not installed" 1; done
[ -f "$SSH_KEY" ] && [ -f "$SSH_KEY.pub" ] || die "no SSH key pair at $SSH_KEY (set MEMCHAL_SSH_KEY or pass --key)" 1
[ -f "$STATE" ] && die "a weights pod is already recorded in .gpu_weights_state. Release it first:
    bash scripts/gpu_demo_down.sh --state .gpu_weights_state" 1
case "$MAX_HOURS" in ''|*[!0-9.]*) die "--max-hours takes a number of hours" 1 ;; esac
case "$IDLE_MIN" in ''|*[!0-9]*) die "--idle-min takes a whole number of minutes" 1 ;; esac
echo "ssh key: $SSH_KEY"

# --- 1. what it would cost --------------------------------------------------
say "balance and price"
BAL=$(curl -sS -m 30 "https://api.runpod.io/graphql" -H "Authorization: Bearer $RUNPOD_API_KEY" \
        -H "Content-Type: application/json" \
        -d '{"query":"query { myself { clientBalance currentSpendPerHr } }"}' 2>/dev/null)
BALANCE=$(printf '%s' "$BAL" | jget data.myself.clientBalance)
if [ -z "$BALANCE" ]; then
  # Older accounts' GraphQL access takes the key as a query parameter instead.
  BAL=$(curl -sS -m 30 "https://api.runpod.io/graphql?api_key=$RUNPOD_API_KEY" \
          -H "Content-Type: application/json" \
          -d '{"query":"query { myself { clientBalance currentSpendPerHr } }"}' 2>/dev/null)
  BALANCE=$(printf '%s' "$BAL" | jget data.myself.clientBalance)
fi
SPEND=$(printf '%s' "$BAL" | jget data.myself.currentSpendPerHr)
[ -n "$BALANCE" ] || { printf '%s\n' "$BAL" | head -c 300; echo; die "could not read the account balance — is the key right?" 1; }
printf 'balance: $%.2f   already spending: $%s/hr\n' "$BALANCE" "${SPEND:-0}"
awk -v b="$BALANCE" -v m="$MIN_BALANCE" 'BEGIN { exit !(b >= m) }' \
  || die "balance is below \$$MIN_BALANCE; not renting" 1

CAT=$(curl -sS -m 30 "$V2/catalog/gpus?include=AVAILABILITY&product=POD" -H "Authorization: Bearer $RUNPOD_API_KEY")
INFO=$(printf '%s' "$CAT" | python3 -c '
import json, sys
gpu, cloud = sys.argv[1], sys.argv[2].lower()
try:
    gpus = json.load(sys.stdin).get("gpus", [])
except Exception:
    print("ERR|the catalog did not answer with JSON"); raise SystemExit
g = next((x for x in gpus if x.get("id") == gpu), None)
if g is None:
    print("ERR|no card with id " + repr(gpu))
else:
    price = (g.get("price") or {}).get(cloud)
    print("|".join([g.get("name", gpu), "?" if price is None else str(price), str(g.get("availability", "?"))]))
' "$GPU" "$CLOUD" 2>&1)
case "$INFO" in ERR\|*) die "${INFO#ERR|}" 1 ;; esac
IFS='|' read -r GPU_NAME PRICE STOCK <<< "$INFO"
[ -n "${PRICE:-}" ] && [ "$PRICE" != "?" ] || die "could not read a price for $GPU on $CLOUD cloud — not renting blind" 2
WORST=$(awk -v p="$PRICE" -v h="$MAX_HOURS" 'BEGIN { printf "%.2f", p * h }')
echo "$GPU_NAME, $CLOUD cloud: \$$PRICE/hr, stock: $STOCK"
echo "hard cap: $MAX_HOURS h after boot, so at most \$$WORST; idle cap: $IDLE_MIN min"
case "$STOCK" in NONE|UNAVAILABLE|none|unavailable) die "no stock right now" 2 ;; esac

# --- 2. the request ---------------------------------------------------------
CAP_S=$(awk -v h="$MAX_HOURS" 'BEGIN { printf "%d", h * 3600 }')
# The hard cap runs first, so it is armed before anything else can fail. It
# uses the pod-scoped key and pod id RunPod puts in every pod's environment;
# single quotes keep them unexpanded until the pod.
CAP='(sleep '"$CAP_S"'; echo "hard cap reached" > /root/watchdog.cap; runpodctl pod delete $RUNPOD_POD_ID; for u in https://rest.runpod.io/v1/pods/$RUNPOD_POD_ID https://api.runpod.io/v2/pods/$RUNPOD_POD_ID; do curl -sS -o /dev/null -X DELETE "$u" -H "Authorization: Bearer $RUNPOD_API_KEY"; done) > /dev/null 2>&1 &'
BOOT=$CAP' apt-get update -q >/dev/null 2>&1; DEBIAN_FRONTEND=noninteractive apt-get install -y -q openssh-server rsync git python3 build-essential curl >/dev/null 2>&1; mkdir -p /run/sshd /root/.ssh; printf "%s\n%s\n" "$PUBLIC_KEY" "$MEMCHAL_PUBKEY" > /root/.ssh/authorized_keys; chmod 700 /root/.ssh; chmod 600 /root/.ssh/authorized_keys; echo "export PATH=/usr/local/cuda/bin:\$PATH" >> /root/.bashrc; touch /root/.ssh_ready; /usr/sbin/sshd -D -p 22'
BODY=$(python3 -c 'import json, sys
print(json.dumps({
  "name": "memchal-weights", "imageName": sys.argv[1], "gpuTypeIds": [sys.argv[2]], "gpuCount": 1,
  "cloudType": sys.argv[3], "computeType": "GPU", "containerDiskInGb": int(sys.argv[4]),
  "ports": ["22/tcp"], "allowedCudaVersions": ["13.0"],
  "dockerEntrypoint": ["bash", "-lc"], "dockerStartCmd": [sys.argv[5]],
  "env": {"MEMCHAL_PUBKEY": open(sys.argv[6]).read().strip()}}))' \
  "$IMAGE" "$GPU" "$CLOUD" "$DISK" "$BOOT" "$SSH_KEY.pub")
if [ "$DRY" -eq 1 ]; then
  say "dry run: the request it would send (POST $REST/pods)"
  printf '%s' "$BODY" | python3 -c 'import json,sys; d=json.load(sys.stdin); d["dockerStartCmd"]=["<boot: hard cap, sshd>"]; d["env"]={"MEMCHAL_PUBKEY":"<your public key>"}; print(json.dumps(d, indent=2))'
  echo "nothing rented."
  exit 0
fi
if [ "$ASSUME_YES" -ne 1 ]; then
  printf '\nRent it? It bills by the second until it is released or terminates itself. [y/N] '
  read -r ans
  case "$ans" in y|Y|yes|YES) ;; *) echo "nothing rented."; exit 1 ;; esac
fi

# --- 3. rent ----------------------------------------------------------------
say "renting"
RESP=$(curl -sS -m 60 -X POST "$REST/pods" -H "Authorization: Bearer $RUNPOD_API_KEY" \
         -H "Content-Type: application/json" -d "$BODY")
POD_ID=$(printf '%s' "$RESP" | jget id)
if [ -z "$POD_ID" ]; then
  printf '%s\n' "$RESP" | head -c 600; echo
  die "could not rent a $GPU with a CUDA 13 driver. Try --cloud COMMUNITY, or later." 2
fi
# Record it NOW, so teardown can always find it.
{ echo "POD_ID=$POD_ID"; echo "SSH_KEY=$SSH_KEY"; echo "LPORT=0";
  echo "CREATED=$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "PRICE=$PRICE"; } > "$STATE"
echo "pod $POD_ID  (recorded in .gpu_weights_state)"
trap 'echo; echo "Bring-up failed. The pod is still rented — run: bash scripts/gpu_demo_down.sh --state .gpu_weights_state"' ERR

# --- 4. wait for ssh --------------------------------------------------------
say "waiting for ssh"
HOST=""; PORT=""
for _ in $(seq 1 90); do
  P=$(curl -sS -m 30 "$REST/pods/$POD_ID" -H "Authorization: Bearer $RUNPOD_API_KEY")
  HOST=$(printf '%s' "$P" | jget publicIp)
  PORT=$(printf '%s' "$P" | jget portMappings.22)
  [ -n "$HOST" ] && [ -n "$PORT" ] && break
  printf '.'; sleep 10
done
echo
[ -n "$HOST" ] && [ -n "$PORT" ] || die "the pod never published a public IP and port for ssh" 2
{ echo "SSH_HOST=$HOST"; echo "SSH_PORT=$PORT"; } >> "$STATE"
SSHO="-i $SSH_KEY -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 -o LogLevel=ERROR"
SSH="ssh $SSHO -p $PORT root@$HOST"
for _ in $(seq 1 60); do
  $SSH 'test -f /root/.ssh_ready' 2>/dev/null && break
  printf '.'; sleep 10
done
echo
$SSH 'test -f /root/.ssh_ready' 2>/dev/null || die "ssh never came up on $HOST:$PORT" 2
echo "ssh: root@$HOST -p $PORT"
$SSH 'nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader; nvidia-smi | grep -o "CUDA Version: [0-9.]*"' 2>/dev/null

# --- 5. ship, and arm the idle watchdog -------------------------------------
say "shipping this working tree"
rsync -az --delete \
  --exclude .git --exclude results --exclude __pycache__ --exclude '*.o' --exclude '*.so' \
  --exclude .env --exclude '.gpu_*_state' \
  --exclude prover --exclude verifier --exclude cheating_prover \
  --exclude prover_cuda --exclude cheating_prover_cuda \
  -e "ssh $SSHO -p $PORT" ./ "root@$HOST:/root/memchal/" || die "could not copy the repo to the pod" 3

say "arming the idle watchdog"
WD=$($SSH "cd /root/memchal && bash scripts/gpu_watchdog.sh --check && setsid -f bash scripts/gpu_watchdog.sh $IDLE_MIN > /root/watchdog.log 2>&1 < /dev/null" 2>/dev/null)
if [ $? -eq 0 ]; then
  echo "$WD; terminates after $IDLE_MIN idle minutes, or $MAX_HOURS h after boot"
else
  echo "WARNING: ${WD:-the watchdog check did not answer}. Only the hard cap is armed."
fi

trap - ERR
if [ "$RUN" -eq 1 ]; then
  say "starting the session"
  $SSH 'cd /root/memchal && mkdir -p results && setsid -f bash scripts/gpu_weights_session.sh > results/session.log 2>&1 < /dev/null' \
    || die "could not start the session" 3
  echo "started. Follow it with:"
else
  echo
  echo "Up, nothing running. Start the session with:"
  echo "  $SSH 'cd /root/memchal && mkdir -p results && setsid -f bash scripts/gpu_weights_session.sh > results/session.log 2>&1 < /dev/null'"
  echo "and follow it with:"
fi
echo "  $SSH 'tail -f /root/memchal/results/session.log'"
cat <<EOF

  pod $POD_ID bills \$$PRICE/hr until released:
      bash scripts/gpu_demo_down.sh --state .gpu_weights_state
  or until it terminates itself: $IDLE_MIN idle minutes, or $MAX_HOURS h after boot.
EOF
