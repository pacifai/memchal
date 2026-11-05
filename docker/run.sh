#!/usr/bin/env bash
# Builds the toolchain image and runs a command in it, with the repo mounted at /work.
# Usage: docker/run.sh [command...]   (default: build and run a one-challenge smoke test)
# Env: PLATFORM (default linux/arm64), CPUS (default 1), MEM (default 2g).
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE="${IMAGE:-memchal}"
PLATFORM="${PLATFORM:-linux/arm64}"
CPUS="${CPUS:-1}"
MEM="${MEM:-2g}"

echo "[run] building $IMAGE ($PLATFORM)..."
docker build --platform "$PLATFORM" -t "$IMAGE" -f docker/Dockerfile docker >/dev/null
mkdir -p results

if [ "$#" -eq 0 ]; then
  set -- bash -c '
    set -e
    make
    build/cpu/prover --port 9099 >/tmp/smoke_prover.log 2>&1 &
    PP=$!; sleep 0.5
    build/verifier --port 9099 --mb 8 --samples 1024 --lanes 8 --challenges 50 --warmup 10 --csv /dev/null
    kill -9 $PP 2>/dev/null || true
    echo "[run] smoke test OK"'
fi

exec docker run --rm --platform "$PLATFORM" \
  --cap-add=IPC_LOCK --ulimit memlock=-1 \
  --cpus="$CPUS" --memory="$MEM" \
  --tmpfs /ram:size=256m \
  -v "$PWD":/work \
  "$IMAGE" "$@"
