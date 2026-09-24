#!/usr/bin/env bash
# gpu_bridge_start.sh — (re)start the demo bridge on the GPU box, detached.
#
# This has to be a script file rather than a one-liner sent over ssh. pkill -f
# and pgrep -f match whole command lines, and a one-liner that both stops the
# old bridge and starts a new one has "bridge_gpu.py" in its own command line —
# so pkill kills the shell that was about to start the bridge, and pgrep finds
# that shell and reports a bridge that is not there. Run as `bash
# scripts/gpu_bridge_start.sh`, this process's command line names neither.
#
# Usage: bash scripts/gpu_bridge_start.sh     Exit: 0 running, 1 it did not start.
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
