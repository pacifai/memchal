#!/usr/bin/env bash
# Restarts the demo bridge on the GPU pod, detached.
# Usage: bash runpod/gpu_bridge_start.sh   (exit 0 running, 1 it did not start)
set -uo pipefail
cd "$(dirname "$0")/.."

pkill -f "demo/bridge_gpu.py" 2>/dev/null
sleep 1
setsid python3 demo/bridge_gpu.py > /root/bridge.log 2>&1 < /dev/null &
disown

for _ in $(seq 1 20); do
  if (echo > /dev/tcp/127.0.0.1/8777) 2>/dev/null; then
    echo "bridge running (pid $(pgrep -f demo/bridge_gpu.py | head -1)), listening on 127.0.0.1:8777"
    exit 0
  fi
  sleep 0.5
done
echo "bridge did not come up — last lines of /root/bridge.log:"
tail -8 /root/bridge.log 2>/dev/null
exit 1
