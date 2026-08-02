#!/bin/bash
# compare_modes.sh — run the verifier against an honest prover and each cheat,
# with identical challenge parameters, and tabulate the contrast.
set -u
cd "$(dirname "$0")"

PORT=9109
MB=64; K=4096; LANES=32; N=300; WARMUP=30; FRAC=0.5

pkill -9 -f '/memchal/cheating_prover' 2>/dev/null
sleep 1

run_mode () {
  local mode="$1" extra="$2" label="$3"
  setsid ./cheating_prover --port $PORT --mode "$mode" $extra \
      --datafile /tmp/memchal_$mode.bin > /tmp/cp_$mode.log 2>&1 < /dev/null &
  local pid=$!
  sleep 0.8
  local out
  out=$(timeout 120 ./verifier --host 127.0.0.1 --port $PORT --mb $MB --k $K \
        --lanes $LANES --n $N --warmup $WARMUP --sweep 0 \
        --csv latency_$mode.csv 2>/dev/null)
  kill -9 $pid 2>/dev/null
  local p50 p99 p999 mx err lock
  p50=$(echo "$out"  | awk '/p50/{print $2}')
  p99=$(echo "$out"  | awk '/p99  /{print $2}')
  p999=$(echo "$out" | awk '/p99.9/{print $2}')
  mx=$(echo "$out"   | awk '/max/{print $2}')
  err=$(echo "$out"  | sed -n 's/.*profile (.* samples, \([0-9]*\) errors).*/\1/p')
  lock=$(echo "$out" | awk '/prover memory/{print $4}')
  printf "  %-9s %-30s p50=%8s  p99=%8s  p99.9=%9s  max=%9s  errors=%-4s\n" \
         "$mode" "$label" "$p50" "$p99" "$p999" "$mx" "$err"
}

echo "buffer=${MB}MiB  k=${K}  lanes=${LANES}  n=${N}  frac=${FRAC}   (latency in us)"
echo "-------------------------------------------------------------------------------"
run_mode honest  ""              "full buffer pinned in RAM"
run_mode disk    ""              "0% resident, all probes -> disk"
run_mode partial "--frac $FRAC"  "50% RAM / 50% disk (correct)"
run_mode fake    "--frac $FRAC"  "50% RAM, misses fabricated"
echo "-------------------------------------------------------------------------------"
echo "CSV per mode: latency_{honest,disk,partial,fake}.csv"
pkill -9 -f '/memchal/cheating_prover' 2>/dev/null
