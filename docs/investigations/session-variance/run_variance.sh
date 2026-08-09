#!/bin/bash
# session-variance — how much of the absolute latency is the environment?
#
# On Docker-on-Apple-Silicon the VM's vCPUs land on performance vs efficiency
# cores, fixed for a whole container session. This shows:
#   Part 1: within ONE long session the honest baseline is STABLE (no mid-run
#           step) — placement is fixed at session start.
#   Part 2: across INDEPENDENT sessions the baseline swings ~1.6x (P/E cores).
# Conclusion: only within-session ratios/differences are trustworthy.
#
# NOTE: this runs on the HOST (it launches multiple containers via run.sh).
# Run from repo root:  bash docs/investigations/session-variance/run_variance.sh
set -u
cd "$(dirname "$0")/../../.."          # repo root
OUT="docs/investigations/session-variance/results"
mkdir -p "$OUT"

echo "### Part 1: one long session, honest 30x back-to-back (step-change check) ###"
./run.sh bash -c 'cd /work; make >/dev/null 2>&1
  ./prover 9820 >/tmp/p.log 2>&1 & sleep 0.7
  for i in $(seq 1 30); do
    ./verifier --port 9820 --mb 64 --k 4096 --lanes 32 --n 2000 --warmup 300 --csv /dev/null 2>/dev/null \
      | awk -v i=$i "/p50/{print i\",\"\$2}"
  done' 2>/dev/null | grep -E '^[0-9]+,' > "$OUT/longsession.csv"
python3 - "$OUT/longsession.csv" <<'PY'
import sys
xs=[float(l.split(',')[1]) for l in open(sys.argv[1])]
n=len(xs); k=max(1,n//3)
first=sum(xs[:k])/k; last=sum(xs[-k:])/k
mean=sum(xs)/n; sd=(sum((x-mean)**2 for x in xs)/(n-1))**0.5
print(f"  n={n}  mean={mean:.1f}us  sd={sd:.1f}us  cv={100*sd/mean:.1f}%")
print(f"  first-third={first:.1f}us  last-third={last:.1f}us  (a big step would mean mid-session migration)")
PY

echo
echo "### Part 2: 6 independent sessions, honest baseline mean per session ###"
: > "$OUT/between_sessions.csv"; echo "session,honest_p50_us" >> "$OUT/between_sessions.csv"
for s in $(seq 1 6); do
  m=$(./run.sh bash -c 'cd /work; make >/dev/null 2>&1
      ./prover 9821 >/dev/null 2>&1 & sleep 0.7
      for i in 1 2 3 4 5; do
        ./verifier --port 9821 --mb 64 --k 4096 --lanes 32 --n 2000 --warmup 200 --csv /dev/null 2>/dev/null \
          | awk "/p50/{print \$2}"
      done' 2>/dev/null | awk '{s+=$1;n++} END{if(n)printf "%.1f", s/n}')
  echo "  session $s: honest p50 mean = ${m} us"
  echo "$s,$m" >> "$OUT/between_sessions.csv"
done
python3 - "$OUT/between_sessions.csv" <<'PY'
import sys,csv
xs=[float(r["honest_p50_us"]) for r in csv.DictReader(open(sys.argv[1])) if r["honest_p50_us"]]
if len(xs)>1:
    mean=sum(xs)/len(xs); sd=(sum((x-mean)**2 for x in xs)/(len(xs)-1))**0.5
    print(f"  between-session: mean={mean:.1f}us sd={sd:.1f}us  min={min(xs):.1f} max={max(xs):.1f}  spread={max(xs)/min(xs):.2f}x")
PY
