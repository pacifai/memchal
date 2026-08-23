#!/usr/bin/env bash
# disk_is_syscalls.sh — discriminator experiment #1: shows that the "disk"
# prover's latency penalty is dominated by pread() SYSCALL overhead, NOT by
# storage distance (spinning-disk, SSD, network latency, etc.).
#
# KEY INSIGHT: the same 4096-pread loop over a tmpfs (which is pure RAM — zero
# block I/O, read_bytes never grows) costs ~91% as much as the same loop over the
# container's overlay disk. That means ~91% of the "disk" penalty is syscall
# overhead. An mmap adversary (data on disk, read via mmap, no per-probe syscall)
# answers at ~honest speed — because with RAM to spare the page cache holds the
# whole file, i.e. the data really is resident. Experiment #2
# (docs/investigations/mmap-needs-ram/run_mmap_needs_ram.sh) takes that RAM away and shows the mmap
# adversary collapsing; run both, in order, for the full story.
#
# HOW TO RUN:
#   ./run.sh bash docs/investigations/disk-is-syscalls/disk_is_syscalls.sh
#
# The repo's run.sh mounts the source at /work, provides a tmpfs at /ram, and
# grants lock privileges. Do NOT run this script directly on the host.
#
# OUTPUT: a table of p50 latencies, block-layer reads and major faults per
# scenario, followed by a VERDICT block.
#
# NOTE ON PORTABILITY: absolute latencies (us) are meaningful only within a
# single run of this script. Compare the within-run numbers only — between-
# session swings of ~1.6x are normal on Apple Silicon due to P/E-core placement.

set -euo pipefail
cd /work
export PYTHONPATH=/work/scripts

echo "=== disk_is_syscalls.sh: building binaries ==="
# Drop blank lines so a clean/up-to-date build doesn't pad the output with them.
make verifier cheating_prover prover 2>&1 | grep -v '^[[:space:]]*$' | tail -5

echo ""
echo "=== environment ==="
echo "  /ram fstype: $(stat -f -c %T /ram 2>/dev/null || echo 'unknown')"
echo "  /tmp fstype: $(stat -f -c %T /tmp 2>/dev/null || echo 'unknown')"
echo "  /root fstype: $(stat -f -c %T /root 2>/dev/null || echo 'unknown')"
echo ""

# Parameters — match the context: n>=2000, k=4096, mb=64, lanes=32
N=2000
K=4096
MB=64
LANES=32
WARMUP=200
BASE_PORT=9600

TMPDIR_MC=$(mktemp -d /tmp/disk_disc_XXXXXX)
ROWS="$TMPDIR_MC/rows.tsv"       # label \t p50 \t read_bytes_delta \t majflt_delta
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$TMPDIR_MC"' EXIT

# p50 (us) out of a verifier --json summary, or "N/A".
p50_of() {
    python3 -c "
import json
try:
    print(json.load(open('$1'))['result']['percentiles_us']['p50']['value'])
except Exception:
    print('N/A')
" 2>/dev/null || echo "N/A"
}

# ── helper: run one scenario ──────────────────────────────────────────────────
# run_scenario LABEL MODE DATAFILE PORT
# Starts cheating_prover in the background, runs the verifier against it, and
# appends one row (p50 + I/O evidence) to $ROWS.
run_scenario() {
    local label="$1" mode="$2" datafile="$3" port="$4"
    local slug="${label//[^a-zA-Z0-9_]/_}"
    local out_json="$TMPDIR_MC/$slug.json"
    local srv_log="$TMPDIR_MC/srv_$slug.log"

    echo "  - $label"
    ./cheating_prover --port "$port" --mode "$mode" --datafile "$datafile" \
        >"$srv_log" 2>&1 &
    local SRV=$!

    local waited=0
    while ! (echo >/dev/tcp/127.0.0.1/$port) 2>/dev/null; do
        sleep 0.1
        waited=$((waited+1))
        if [ $waited -gt 80 ]; then
            echo "  ERROR: server on port $port did not come up" >&2
            kill $SRV 2>/dev/null
            printf '%s\tN/A\tN/A\tN/A\n' "$label" >>"$ROWS"
            return 0
        fi
    done
    sleep 0.2  # let it settle

    # read_bytes straight from the server's own /proc, bracketing the run.
    local io0 io1
    io0=$(awk '/^read_bytes/{print $2}' /proc/$SRV/io 2>/dev/null || echo 0)

    ./verifier --host 127.0.0.1 --port "$port" \
        --mb "$MB" --k "$K" --lanes "$LANES" \
        --n "$N" --warmup "$WARMUP" \
        --csv /dev/null --json "$out_json" \
        >/dev/null 2>&1 || true

    io1=$(awk '/^read_bytes/{print $2}' /proc/$SRV/io 2>/dev/null || echo 0)

    kill "$SRV" 2>/dev/null || true
    wait "$SRV" 2>/dev/null || true
    sleep 0.3

    # The prover reports its own major-fault delta over the challenge loop.
    local majflt
    majflt=$(sed -n 's/.*majflt_delta=\([0-9]*\).*/\1/p' "$srv_log" | tail -1)
    [ -n "$majflt" ] || majflt="N/A"

    printf '%s\t%s\t%s\t%s\n' "$label" "$(p50_of "$out_json")" "$((io1 - io0))" "$majflt" >>"$ROWS"
}

# ── honest baseline ───────────────────────────────────────────────────────────
echo "--- running honest baseline ---"
HONEST_PORT=$BASE_PORT
./cheating_prover --port "$HONEST_PORT" --mode honest \
    >"$TMPDIR_MC/srv_honest.log" 2>&1 &
HONEST_PID=$!
waited=0
while ! (echo >/dev/tcp/127.0.0.1/$HONEST_PORT) 2>/dev/null; do
    sleep 0.1; waited=$((waited+1))
    [ $waited -gt 80 ] && { echo "honest server didn't start"; kill $HONEST_PID; exit 1; }
done
sleep 0.2

HONEST_JSON="$TMPDIR_MC/honest.json"
./verifier --host 127.0.0.1 --port "$HONEST_PORT" \
    --mb "$MB" --k "$K" --lanes "$LANES" \
    --n "$N" --warmup "$WARMUP" \
    --csv /dev/null --json "$HONEST_JSON" \
    >/dev/null 2>&1 || true

HONEST_P50=$(p50_of "$HONEST_JSON")
kill "$HONEST_PID" 2>/dev/null || true
wait "$HONEST_PID" 2>/dev/null || true
sleep 0.3
echo "  honest p50: ${HONEST_P50} us"

printf '%s\t%s\t%s\t%s\n' "honest (RAM, mlock'd)" "$HONEST_P50" "-" "-" >>"$ROWS"
echo ""

# ── disk scenarios ────────────────────────────────────────────────────────────
echo "--- running disk scenarios (this will be slow) ---"
run_scenario "disk/pread over tmpfs (pure RAM)" disk     /ram/x.bin      $((BASE_PORT+1))
run_scenario "disk/pread over overlay (VM disk)" disk     /tmp/x.bin      $((BASE_PORT+2))
run_scenario "mmapdisk over overlay (no pread)" mmapdisk /tmp/x_mmap.bin $((BASE_PORT+3))
echo ""

# ── report ────────────────────────────────────────────────────────────────────
ROWS="$ROWS" K=$K LANES=$LANES MB=$MB N=$N WARMUP=$WARMUP \
    python3 docs/investigations/disk-is-syscalls/report.py
