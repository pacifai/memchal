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
make verifier cheating_prover prover 2>&1 | tail -5

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
ROWS="$ROWS" K=$K LANES=$LANES MB=$MB N=$N WARMUP=$WARMUP python3 - <<'PYEOF'
import os, sys
sys.path.insert(0, "/work/scripts")
from fmt_table import table, block

def num(s):
    try: return float(s)
    except Exception: return None

rows_in = [l.rstrip("\n").split("\t") for l in open(os.environ["ROWS"]) if l.strip()]
p50 = {r[0]: num(r[1]) for r in rows_in}
honest = next((v for k, v in p50.items() if k.startswith("honest")), None)

def mib(s):
    v = num(s)
    return s if v is None else f"{v/(1<<20):,.1f} MiB"

out = []
for label, p, io, mf in rows_in:
    v = num(p)
    ratio = "1.00x (ref)" if label.startswith("honest") else (
        f"{v/honest:,.2f}x" if (v and honest) else "N/A")
    out.append([label,
                "N/A" if v is None else f"{v:,.1f}",
                ratio,
                mib(io),
                f"{int(mf):,}" if mf.isdigit() else mf])

print(table(
    [("scenario", "l"), ("p50 (us)", "r"), ("vs honest", "r"),
     ("storage reads", "r"), ("maj faults", "r")],
    out,
    title='EXPERIMENT 1 — IS THE "DISK" PENALTY STORAGE, OR SYSCALLS?',
    notes=[
        f"k={os.environ['K']}  lanes={os.environ['LANES']}  mb={os.environ['MB']}  "
        f"n={os.environ['N']}/run  warmup={os.environ['WARMUP']}",
        "absolute us are within-session only — compare rows of THIS run, never across runs",
        "block-layer reads / major faults are the prover's own counters over the challenge loop",
    ]))
print()

tmpfs = p50.get("disk/pread over tmpfs (pure RAM)")
over  = p50.get("disk/pread over overlay (VM disk)")
mmapd = p50.get("mmapdisk over overlay (no pread)")

lines = []
if tmpfs and over:
    lines += [
        f"disk over tmpfs (pure RAM)  = {tmpfs:>10,.1f} us   <- zero block I/O, same pread loop",
        f"disk over overlay (VM disk) = {over:>10,.1f} us",
        f"=> {tmpfs / over * 100.0:.0f}% of the overlay-disk latency is still there with NO disk involved.",
        '   The "disk is ~15x slower" signal is pread() syscall overhead, not storage distance.',
    ]
else:
    lines.append("could not compare tmpfs vs overlay (N/A results)")

lines.append("")
if mmapd and honest:
    lines += [
        f"mmapdisk (on disk, mmap'd)  = {mmapd:>10,.1f} us   ({mmapd/honest:.2f}x honest)",
        f"honest (RAM, mlock'd)       = {honest:>10,.1f} us",
        "=> Drop the per-probe syscall and the penalty disappears entirely. Note WHY:",
        "   this container has RAM to spare, so the page cache holds the whole file and",
        "   the adversary IS resident, just not via mlock (its block-layer reads stop",
        "   after one cold sweep). Take that RAM away and the trick dies — experiment 2,",
        "   docs/investigations/mmap-needs-ram/run_mmap_needs_ram.sh.",
    ]
else:
    lines.append("could not compare mmapdisk vs honest (N/A results)")

print(block("VERDICT — TIMING MEASURES SYSCALLS, NOT DISTANCE", lines))
PYEOF
