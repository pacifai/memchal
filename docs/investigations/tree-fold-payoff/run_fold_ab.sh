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
    python3 - "$OUT/$FOLD-$LANES.json" "$OUT/p-$FOLD-$LANES.log" "$FOLD" "$LANES" "$K" <<'PY'
import json,sys,statistics,re
d=json.load(open(sys.argv[1]))['result']
p50=d['percentiles_us']['p50']['value']; err=d['errors']
ks=[];cs=[];fs=[]
for line in open(sys.argv[2]):
    if '"t":"g"' in line:
        e=json.loads(line[1:])
        ks.append(e['kernel_ms']); cs.append(e['copy_ms']); fs.append(e['fold_ms'])
med=lambda a: statistics.median(a) if a else float('nan')
print('%-8s %-10s %10s %10.1f %10.4f %10.4f %10.4f   errors=%d' %
      (sys.argv[3],sys.argv[4],sys.argv[5],p50,med(ks),med(cs),med(fs),err))
PY
  done
done
