#!/bin/bash
# latency-decomposition — where does a challenge's time actually go?
#
# Splits per-challenge latency into: transport floor (k=0 round trip) + per-probe
# hash-chain ALU + per-probe DRAM random-access stall. Method: sweep k on a big
# (64 MiB, DRAM-bound) buffer and on a tiny (64 KiB, L1-resident) buffer in ONE
# session. floor = p50 at k=0; ALU/probe = (tiny − floor)/k; total/probe =
# (big − floor)/k; memory/probe = total − ALU. Also compares honest vs
# fake --frac 0 (all fabricated, zero memory traffic) to show why fake is faster.
#
# Run from repo root:  ./run.sh bash docs/investigations/latency-decomposition/decompose.sh
# Absolute us are within-session only (see docs/decisions/0003-timing-detector-scope-and-measurement.md).
set -u; cd /work
make >/dev/null 2>&1
PORT=9810
./prover $PORT >/tmp/dp.log 2>&1 & PP=$!; sleep 0.7
p50(){ ./verifier --port $PORT --mb "$1" --k "$2" --lanes 32 --n 2000 --warmup 300 --csv /dev/null 2>/dev/null | awk '/p50/{print $2}'; }

echo "# k-sweep: big=64MiB (DRAM) vs tiny=64KiB (L1-resident), p50 us"
echo "k,big_us,tiny_us"
for k in 0 1 1024 4096 16384 65536; do
  echo "$k,$(p50 64 $k),$(p50 0.0625 $k)"
done | tee /tmp/sweep.csv
kill -9 $PP 2>/dev/null; sleep 0.3

echo
echo "# decomposition (from k=65536 row):"
awk -F, '$1==65536{ big=$2; tiny=$3 } END{
  # floor is the k=0 row; re-read it
}' /tmp/sweep.csv >/dev/null
FLOOR=$(awk -F, '$1==0{print $2}' /tmp/sweep.csv)
awk -F, -v floor="$FLOOR" '$1==65536{
  k=$1; big=$2; tiny=$3;
  alu=(tiny-floor)/k*1000;      # ns/probe
  tot=(big-floor)/k*1000;
  mem=tot-alu;
  printf "  transport floor (k=0) = %.1f us\n", floor;
  printf "  ALU per probe         = %.2f ns  (tiny buffer, L1 hits)\n", alu;
  printf "  total per probe       = %.2f ns  (big buffer)\n", tot;
  printf "  => DRAM stall/probe   = %.2f ns  (total - ALU)\n", mem;
  printf "  memory is %.0f%% of per-probe cost; ALU is the rest\n", 100*mem/tot;
}' /tmp/sweep.csv

echo
echo "# why is fake faster than honest? honest (all DRAM reads) vs fake --frac 0 (all fabricated, no memory)"
./prover $PORT >/tmp/dp.log 2>&1 & PP=$!; sleep 0.7
H=$(p50 64 4096); kill -9 $PP 2>/dev/null; sleep 0.3
./cheating_prover --port $PORT --mode fake --frac 0 >/tmp/df.log 2>&1 & FP=$!; sleep 0.7
F=$(p50 64 4096); kill -9 $FP 2>/dev/null
echo "  honest p50=${H}us   fake(frac0) p50=${F}us   (k=4096; fake does pure ALU, no memory reads)"
