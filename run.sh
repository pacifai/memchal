#!/usr/bin/env bash
# run.sh — build the toolchain image and run a command inside a linux/arm64
# container with the privileges the measurements require. Results land in
# ./results on the host.
#
# Usage:
#   ./run.sh                 # build the binaries (default: make) and smoke-test
#   ./run.sh make            # just build
#   ./run.sh bash            # interactive shell in the measurement environment
#   ./run.sh <any command>   # run an arbitrary command in the container
#
# Env overrides: PLATFORM (default linux/arm64), CPUS (1), MEM (2g).
#
# Container privileges, and why each matters:
#   --cap-add=IPC_LOCK --ulimit memlock=-1  : let mlock() actually pin pages, so
#       the honest baseline is genuinely non-swappable (verified in-code, step 5).
#   --tmpfs /ram:size=256m                  : a guaranteed-RAM filesystem, used by
#       the disk-vs-tmpfs discriminator to prove the "disk" cost is syscalls.
#   --cpus / --memory                       : explicit allocation so runs compare.
set -euo pipefail
cd "$(dirname "$0")"

IMAGE="${IMAGE:-memchal}"
PLATFORM="${PLATFORM:-linux/arm64}"
CPUS="${CPUS:-1}"
MEM="${MEM:-2g}"

echo "[run] building $IMAGE ($PLATFORM)..."
docker build --platform "$PLATFORM" -t "$IMAGE" . >/dev/null
mkdir -p results

# Default action: build, then a one-challenge smoke test proving the toolchain
# and the honest path work end to end.
if [ "$#" -eq 0 ]; then
  set -- bash -c '
    set -e
    make
    ./prover 9099 >/tmp/smoke_prover.log 2>&1 &
    PP=$!; sleep 0.5
    ./verifier --port 9099 --mb 8 --k 1024 --lanes 8 --n 50 --warmup 10 --csv /dev/null
    kill -9 $PP 2>/dev/null || true
    echo "[run] smoke test OK"'
fi

exec docker run --rm --platform "$PLATFORM" \
  --cap-add=IPC_LOCK --ulimit memlock=-1 \
  --cpus="$CPUS" --memory="$MEM" \
  --tmpfs /ram:size=256m \
  -v "$PWD":/work \
  "$IMAGE" "$@"
