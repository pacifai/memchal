#!/usr/bin/env bash
# Find where the UVM cheat stops being indistinguishable from honest: raise the
# HBM held hostage until the managed buffer can no longer fit on the card.
set -uo pipefail
cd /root/memchal
PORT=9099; MB=4096; K=262144; LANES=16384; N=30; W=5
OUT=results/uvm-sweep; mkdir -p $OUT
kill_p() { pkill -f cheating_prover_cuda >/dev/null 2>&1; sleep 1; }
trap kill_p EXIT
TOTAL=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits | head -1)
echo "card total: ${TOTAL} MiB, buffer: ${MB} MiB"
printf '%10s %10s %12s %10s %8s\n' "hog MiB" "free MiB" "p50 us" "x honest" "errors"
BASE=629.0
for HOG in 0 36000 39000 40000 41000 42000 42500 43000; do
  kill_p
  nohup ./cheating_prover_cuda --port $PORT --mode uvm --hog $HOG --trace 0 \
        --lane-cap $LANES > $OUT/p-$HOG.log 2>&1 &
  waited=0
  while ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; do
    sleep 0.2; waited=$((waited+1)); [ $waited -gt 300 ] && break
  done
  if [ $waited -gt 300 ]; then printf '%10s %10s %12s\n' "$HOG" "-" "prover failed"; continue; fi
  timeout 600 ./verifier --host 127.0.0.1 --port $PORT --mb $MB --k $K --lanes $LANES \
      --n $N --warmup $W --label uvm-hog$HOG --csv /dev/null \
      --json $OUT/uvm-$HOG.json > $OUT/v-$HOG.log 2>&1
  rc=$?
  FREE=$((TOTAL - HOG))
  if [ $rc -ne 0 ]; then printf '%10s %10s %12s\n' "$HOG" "$FREE" "failed/timeout"; continue; fi
  python3 -c "
import json
d=json.load(open('$OUT/uvm-$HOG.json'))['result']
p=d['percentiles_us']['p50']['value']
print('%10s %10s %12.1f %9.2fx %8d' % ('$HOG','$FREE',p,p/$BASE,d['errors']))"
done
