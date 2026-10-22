#!/usr/bin/env bash
# A/B the two folds at two lane counts. The fold cost grows with lanes and the
# kernel does not, so the high-lane row is where the claim lives.
set -uo pipefail
cd /root/memchal
PORT=9099; MB=8192; N=60; W=15
OUT=results/fold-ab; mkdir -p $OUT
kp(){ pkill -f prover_cuda >/dev/null 2>&1; sleep 1; }
trap kp EXIT
printf '%-8s %-10s %10s %10s %10s %10s %10s\n' fold lanes "k" "p50 us" "kernel ms" "copy ms" "fold ms"
for LANES in 16384 131072; do
  K=$((LANES * 16))
  for FOLD in sequential tree; do
    kp
    nohup ./prover_cuda $PORT --trace 5 --lane-cap $LANES > $OUT/p-$FOLD-$LANES.log 2>&1 &
    w=0; while ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; do sleep 0.2; w=$((w+1)); [ $w -gt 300 ] && break; done
    timeout 900 ./verifier --host 127.0.0.1 --port $PORT --mb $MB --k $K --lanes $LANES \
        --n $N --warmup $W --fold $FOLD --label fold-$FOLD-$LANES --csv /dev/null \
        --json $OUT/$FOLD-$LANES.json > $OUT/v-$FOLD-$LANES.log 2>&1
    rc=$?
    kp
    if [ $rc -ne 0 ]; then printf '%-8s %-10s %10s   verifier exited %d\n' $FOLD $LANES $K $rc; continue; fi
    python3 docs/investigations/tree-fold-payoff/fold_row.py "$OUT/$FOLD-$LANES.json" "$OUT/p-$FOLD-$LANES.log" "$FOLD" "$LANES" "$K"
  done
done
