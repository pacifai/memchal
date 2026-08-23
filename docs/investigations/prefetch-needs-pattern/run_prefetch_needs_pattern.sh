#!/bin/bash
# prefetch-needs-pattern — is the prefetch adversary real, or does its own overhead
# swamp it? And does the data-dependent (chained) access pattern actually stop it?
#
# The prefetch cheat keeps the data on disk but enumerates the challenge's offsets
# up front and MADV_WILLNEED-batches the pages, dodging the serialized per-probe
# faults that slow mmapdisk. That overhead (dry-run + madvise storm) only pays off
# when there ARE cold faults to hide — i.e. under memory pressure. In the cached
# regime it is pure overhead and loses; that is the regime the first cut measured.
#
# Runs on the HOST (launches its own containers). From repo root:
#   bash docs/investigations/prefetch-needs-pattern/run_prefetch_needs_pattern.sh
set -u
cd "$(dirname "$0")/../../.."
OUT="docs/investigations/prefetch-needs-pattern/results"; mkdir -p "$OUT"
IMAGE=memchal; PLAT=linux/arm64
MB=64; K=2048; LANES=32; N=200; WARMUP=40
docker build --platform "$PLAT" -t "$IMAGE" . >/dev/null 2>&1

# measure one (mode, chain, prover-budget) -> p50 us, prover in its own memory
# cap, verifier in a second container sharing its netns (separate accounting).
measure(){ local mode=$1 chain=$2 mem=$3; local cn="pf_${mode}_${chain}_${mem}_$$"
  docker run -d --name "$cn" --platform "$PLAT" --memory="$mem" --memory-swap="$mem" \
    -v "$PWD":/work -w /work "$IMAGE" \
    bash -c "make >/dev/null 2>&1; ./cheating_prover --port 9600 --mode $mode --datafile /var/tmp/x.bin" >/dev/null 2>&1
  local i; for i in $(seq 1 80); do docker exec "$cn" bash -c 'exec 3<>/dev/tcp/127.0.0.1/9600' 2>/dev/null && break; sleep 0.3; done
  local p; p=$(docker run --rm --network "container:$cn" --platform "$PLAT" --memory=1g \
      -v "$PWD":/work -w /work "$IMAGE" \
      ./verifier --host 127.0.0.1 --port 9600 --mb $MB --k $K --lanes $LANES --n $N --warmup $WARMUP \
        --chain "$chain" --csv /dev/null 2>/dev/null | awk '/p50/{print $2}')
  docker rm -f "$cn" >/dev/null 2>&1
  echo "${p:-NA}"
}

printf "buffer=%s MiB  k=%s lanes=%s n=%s warmup=%s\n" $MB $K $LANES $N $WARMUP | tee "$OUT/run.txt"
printf "%-9s %-12s %-11s %-11s\n" regime chain mmapdisk prefetch | tee -a "$OUT/run.txt"
# cached: budget >> buffer ; starved: budget < buffer (half)
for spec in "cached 1024m" "starved 40m"; do
  regime=${spec%% *}; mem=${spec##* }
  for chain in independent dependent; do
    md=$(measure mmapdisk "$chain" "$mem")
    pf=$(measure prefetch "$chain" "$mem")
    printf "%-9s %-12s %-11s %-11s\n" "$regime" "$chain" "$md" "$pf" | tee -a "$OUT/run.txt"
  done
done
echo "(p50 us; prover held to the budget, verifier in a separate 1g container sharing its netns)" | tee -a "$OUT/run.txt"
