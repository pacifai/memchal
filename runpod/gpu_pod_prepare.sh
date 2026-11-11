#!/usr/bin/env bash
# Prepares a fresh pod: arms the watchdog, builds, and runs the known-answer gate.
# Usage: bash runpod/gpu_pod_prepare.sh IDLE_MIN MAX_HOURS
# Exit: 0 the pod is ready, 1 the card is unusable, 2 the build or gate failed.
set -uo pipefail
cd "$(dirname "$0")/.."

if bash runpod/gpu_watchdog.sh --check; then
  setsid bash runpod/gpu_watchdog.sh "$1" "$2" > /root/watchdog.log 2>&1 < /dev/null &
  echo "watchdog: terminates the pod after $1 idle minutes or $2 hours"
else
  echo "WARNING: the pod cannot terminate itself if this computer loses its connection."
fi

bash runpod/gpu_pod_setup.sh
