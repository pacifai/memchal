#!/usr/bin/env bash
# mmap_needs_ram.sh — discriminator experiment #2: the mmap adversary only works
# while it has the RAM to cache what it claims to hold. Take the RAM away and it
# collapses by ~3 orders of magnitude.
#
# WHY THIS EXPERIMENT EXISTS. Experiment 1 (disk_is_syscalls.sh) showed the
# "disk" prover's penalty is pread() syscall overhead, and that an mmap adversary
# — data on disk, read through a memory-mapped view, no per-probe syscall —
# answers at ~honest speed. That looked like a defeat for the timing detector.
# It isn't, and this experiment shows why: in a container with GiBs to spare the
# page cache holds the entire file after one cold sweep, so that adversary really
# is holding the data in RAM. It just did it via the page cache instead of
# mlock(), which is not cheating — it's residency by another name.
#
# THE MEASUREMENT. Run the same mmap adversary in a container whose memory budget
# is SMALLER than the buffer it claims to hold. Now the page cache cannot keep
# the file: cgroup reclaim unmaps its pages, every probe that lands on an evicted
# page takes a major fault to storage, and there is no syscall-free trick left.
# The prover reports its own major-fault and block-layer-read counters, so the
# table shows the mechanism, not just the latency.
#
# HOW TO RUN — on the HOST (this script drives docker; it cannot run inside the
# measurement container):
#   bash docs/investigations/mmap-needs-ram/run_mmap_needs_ram.sh
#
# Env overrides: MB K LANES N WARMUP FIT_MEM STARVED_MEM CPUS IMAGE PLATFORM
#
# CAVEATS, stated up front:
#   * The cgroup memory budget stands in for physical RAM. It constrains the page
#     cache the same way a small machine would, which is the point, but it is not
#     a bare-metal >RAM test.
#   * Docker Desktop's storage is virtualized through the macOS host, which has
#     its own cache below us. The fault costs here are therefore a LOWER BOUND on
#     what real storage would charge.
#   * Absolute microseconds are within-session only (P/E-core placement swings
#     ~1.6x between sessions). Compare rows of one run.
#   * STARVED_MEM=32m (half the buffer) is the smallest budget that reliably works
#     here; at 16m the prover is OOM-killed before it finishes streaming the
#     buffer. The table flags an OOM-killed row rather than reporting its latency,
#     so if you shrink the budget and a row disappears, that is why.

set -euo pipefail
cd "$(dirname "$0")/../../.."
export PYTHONPATH="$PWD/scripts"

IMAGE="${IMAGE:-memchal}"
PLATFORM="${PLATFORM:-linux/arm64}"
CPUS="${CPUS:-1}"

MB="${MB:-64}"                       # buffer the prover claims to hold, MiB
K="${K:-4096}"
LANES="${LANES:-32}"
N="${N:-200}"                        # samples/run: starved rows cost ~60 ms each
WARMUP="${WARMUP:-20}"               # enough to absorb the mmap prover's cold sweep
FIT_MEM="${FIT_MEM:-1g}"             # budget >> buffer: page cache can hold it all
STARVED_MEM="${STARVED_MEM:-32m}"    # budget < buffer: it cannot

command -v docker >/dev/null || { echo "ERROR: needs the docker CLI — run this on the host, not inside run.sh" >&2; exit 1; }

OUTDIR="docs/investigations/mmap-needs-ram/results"
ROWS="$OUTDIR/rows.tsv"              # label \t mem \t p50 \t probes \t read_bytes \t majflt \t note
CNAME="mc_cheat_$$"

mkdir -p "$OUTDIR"
: >"$ROWS"
trap 'docker rm -f "$CNAME" >/dev/null 2>&1 || true' EXIT

echo "=== mmap_needs_ram.sh: building image + binaries ==="
docker build --platform "$PLATFORM" -t "$IMAGE" . >/dev/null
# Drop blank lines so a clean/up-to-date build doesn't pad the output with them.
docker run --rm --platform "$PLATFORM" -v "$PWD":/work "$IMAGE" make 2>&1 \
    | grep -v '^[[:space:]]*$' | tail -2
echo ""
echo "  buffer claimed:      ${MB} MiB      k=$K lanes=$LANES n=$N warmup=$WARMUP"
echo "  cheater budgets:     fits=$FIT_MEM   starved=$STARVED_MEM"
echo ""

# ── run one scenario ──────────────────────────────────────────────────────────
# run_scenario LABEL MODE CHEATER_MEM
# The prover runs in its own container with its own memory budget; the verifier
# runs in a second container sharing the prover's network namespace, so the
# transport path stays plain loopback (comparable to experiment 1) while the two
# processes keep separate memory accounting.
run_scenario() {
    local label="$1" mode="$2" mem="$3"
    local slug="${label//[^a-zA-Z0-9]/_}"
    local json="$OUTDIR/$slug.json"
    local log="$OUTDIR/$slug.prover.log"
    local port=9700

    echo "--- $label  (prover budget $mem) ---"
    rm -f "$json"
    docker rm -f "$CNAME" >/dev/null 2>&1 || true
    docker run -d --name "$CNAME" --platform "$PLATFORM" \
        --cpus="$CPUS" --cap-add=IPC_LOCK --ulimit memlock=-1 \
        --memory="$mem" --memory-swap="$mem" \
        -v "$PWD":/work "$IMAGE" \
        ./cheating_prover --port "$port" --mode "$mode" --datafile /var/tmp/x.bin >/dev/null

    # Wait for the listener (the prover prints it before accepting).
    local waited=0
    until docker logs "$CNAME" 2>&1 | grep -q "listening on"; do
        sleep 0.2; waited=$((waited+1))
        [ $waited -gt 100 ] && { echo "  ERROR: prover did not come up"; break; }
    done

    docker run --rm --network "container:$CNAME" --platform "$PLATFORM" \
        --cpus="$CPUS" --memory=1g -v "$PWD":/work "$IMAGE" \
        ./verifier --host 127.0.0.1 --port "$port" \
        --mb "$MB" --k "$K" --lanes "$LANES" --n "$N" --warmup "$WARMUP" \
        --label "$slug" --csv /dev/null --json "/work/$json" >/dev/null 2>&1 || true

    docker logs "$CNAME" >"$log" 2>&1 || true
    local oom
    oom=$(docker inspect "$CNAME" --format '{{.State.OOMKilled}}' 2>/dev/null || echo unknown)
    docker rm -f "$CNAME" >/dev/null 2>&1 || true

    local p50 probes rb mf note
    p50=$(python3 -c "
import json
try:    print(json.load(open('$json'))['result']['percentiles_us']['p50']['value'])
except Exception: print('N/A')
")
    probes=$(sed -n 's/.*probes=\([0-9]*\).*/\1/p' "$log" | tail -1)
    rb=$(sed -n 's/.*read_bytes_delta=\([0-9]*\).*/\1/p' "$log" | tail -1)
    mf=$(sed -n 's/.*majflt_delta=\([0-9]*\).*/\1/p' "$log" | tail -1)
    note=""
    [ "$oom" = "true" ] && note="OOM-KILLED: budget too small to even stream the buffer"

    echo "    p50=${p50:-N/A} us   majflt=${mf:--}   oom=$oom"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$mem" "${p50:-N/A}" "${probes:--}" "${rb:--}" "${mf:--}" "$note" >>"$ROWS"
}

run_scenario "honest (mlock'd RAM)"      honest   "$FIT_MEM"
run_scenario "mmapdisk, RAM to spare"    mmapdisk "$FIT_MEM"
run_scenario "mmapdisk, RAM < buffer"    mmapdisk "$STARVED_MEM"
run_scenario "disk/pread, RAM < buffer"  disk     "$STARVED_MEM"
echo ""

# ── report ────────────────────────────────────────────────────────────────────
ROWS="$ROWS" MB="$MB" K="$K" LANES="$LANES" N="$N" WARMUP="$WARMUP" \
FIT_MEM="$FIT_MEM" STARVED_MEM="$STARVED_MEM" \
    python3 docs/investigations/mmap-needs-ram/report.py
