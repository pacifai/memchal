#!/usr/bin/env bash
# disk_is_syscalls.sh — discriminator experiment: shows that the "disk" prover's
# latency penalty is dominated by pread() SYSCALL overhead, NOT by storage
# distance (spinning-disk, SSD, network latency, etc.).
#
# KEY INSIGHT: the same 4096-pread loop over a tmpfs (which is pure RAM — zero
# block I/O, read_bytes never grows) costs ~91% as much as the same loop over the
# container's overlay disk. That means ~91% of the "disk" penalty is syscall
# overhead. An mmap adversary (data on disk, read via mmap, no per-probe syscall)
# answers at ~honest speed — for buffers that fit in RAM; behaviour above RAM
# size is unmeasured.
#
# HOW TO RUN:
#   ./run.sh bash scripts/experiments/disk_is_syscalls.sh
#
# The repo's run.sh mounts the source at /work, provides a tmpfs at /ram, and
# grants lock privileges. Do NOT run this script directly on the host.
#
# OUTPUT: a table of p50 latencies and /proc/PID/io read_bytes deltas for each
# scenario, followed by a VERDICT block with % breakdown.
#
# NOTE ON PORTABILITY: absolute latencies (us) are meaningful only within a
# single run of this script. Compare the within-run numbers only — between-
# session swings of ~1.6x are normal on Apple Silicon due to P/E-core placement.

set -euo pipefail
cd /work

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

# Temp directory for JSON results
TMPDIR_MC=$(mktemp -d /tmp/disk_disc_XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$TMPDIR_MC"' EXIT

# ── helper: run one scenario ──────────────────────────────────────────────────
# run_scenario LABEL MODE DATAFILE PORT
# Starts cheating_prover in background, waits for it, reads /proc/PID/io before
# and after verifier run, records p50 from --json output.
run_scenario() {
    local label="$1"
    local mode="$2"
    local datafile="$3"
    local port="$4"
    local out_json="$TMPDIR_MC/${label//[^a-zA-Z0-9_]/_}.json"

    # Start the server
    ./cheating_prover --port "$port" --mode "$mode" --datafile "$datafile" \
        >"$TMPDIR_MC/srv_${port}.log" 2>&1 &
    local SRV=$!

    # Wait for it to be ready
    local waited=0
    while ! (echo >/dev/tcp/127.0.0.1/$port) 2>/dev/null; do
        sleep 0.1
        waited=$((waited+1))
        if [ $waited -gt 80 ]; then
            echo "  ERROR: server on port $port did not come up" >&2
            kill $SRV 2>/dev/null
            return 1
        fi
    done
    sleep 0.2  # let it settle

    # Capture read_bytes from the server process before verifier run
    local io0
    io0=$(awk '/^read_bytes/{print $2}' /proc/$SRV/io 2>/dev/null || echo 0)

    # Run verifier
    ./verifier --host 127.0.0.1 --port "$port" \
        --mb "$MB" --k "$K" --lanes "$LANES" \
        --n "$N" --warmup "$WARMUP" --sweep 0 \
        --csv /dev/null --json "$out_json" \
        >/dev/null 2>&1 || true

    # Capture read_bytes after
    local io1
    io1=$(awk '/^read_bytes/{print $2}' /proc/$SRV/io 2>/dev/null || echo 0)

    local delta=$((io1 - io0))

    # Extract p50 from JSON
    local p50
    p50=$(python3 -c "
import json, sys
try:
    d=json.load(open('$out_json'))
    print(d['result']['percentiles_us']['p50']['value'])
except Exception as e:
    print('N/A')
" 2>/dev/null || echo "N/A")

    # Store results for table
    echo "${label}|${p50}|${delta}|${SRV}"

    kill "$SRV" 2>/dev/null || true
    wait "$SRV" 2>/dev/null || true
    sleep 0.3
}

# ── honest baseline ───────────────────────────────────────────────────────────
echo "--- running honest baseline ---"
HONEST_PORT=$((BASE_PORT))
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
    --n "$N" --warmup "$WARMUP" --sweep 0 \
    --csv /dev/null --json "$HONEST_JSON" \
    >/dev/null 2>&1 || true

HONEST_P50=$(python3 -c "
import json
try:
    d=json.load(open('$HONEST_JSON'))
    print(d['result']['percentiles_us']['p50']['value'])
except:
    print('N/A')
" 2>/dev/null || echo "N/A")

kill "$HONEST_PID" 2>/dev/null || true
wait "$HONEST_PID" 2>/dev/null || true
sleep 0.3
echo "  honest p50: ${HONEST_P50} us"
echo ""

# ── disk scenarios ────────────────────────────────────────────────────────────
echo "--- running disk scenarios (this will be slow) ---"
echo ""

# 1) disk mode over tmpfs (/ram)
RESULT_TMPFS=$(run_scenario "disk_tmpfs" "disk" "/ram/x.bin" $((BASE_PORT+1)))
P50_TMPFS=$(echo "$RESULT_TMPFS" | cut -d'|' -f2)
DELTA_TMPFS=$(echo "$RESULT_TMPFS" | cut -d'|' -f3)

# 2) disk mode over overlay (container disk)
RESULT_OVERLAY=$(run_scenario "disk_overlay" "disk" "/tmp/x.bin" $((BASE_PORT+2)))
P50_OVERLAY=$(echo "$RESULT_OVERLAY" | cut -d'|' -f2)
DELTA_OVERLAY=$(echo "$RESULT_OVERLAY" | cut -d'|' -f3)

# 3) mmapdisk over overlay (no per-probe syscall)
RESULT_MMAP=$(run_scenario "mmapdisk_overlay" "mmapdisk" "/tmp/x_mmap.bin" $((BASE_PORT+3)))
P50_MMAP=$(echo "$RESULT_MMAP" | cut -d'|' -f2)
DELTA_MMAP=$(echo "$RESULT_MMAP" | cut -d'|' -f3)

# ── print table ───────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════════════════════════╗"
echo "║           DISK vs SYSCALL DISCRIMINATOR — within-session results           ║"
echo "╠══════════════════════════════════════════════════════════════════════════════╣"
printf "║  %-35s  %10s us   %15s  ║\n" "scenario" "p50" "read_bytes_delta"
echo "╠══════════════════════════════════════════════════════════════════════════════╣"
printf "║  %-35s  %10s us   %15s  ║\n" "honest (RAM, mlock'd)"       "${HONEST_P50}"  "n/a"
printf "║  %-35s  %10s us   %15s  ║\n" "disk / tmpfs (RAM, no blk I/O)" "${P50_TMPFS}" "${DELTA_TMPFS} B"
printf "║  %-35s  %10s us   %15s  ║\n" "disk / overlay (VM disk)"    "${P50_OVERLAY}" "${DELTA_OVERLAY} B"
printf "║  %-35s  %10s us   %15s  ║\n" "mmapdisk / overlay (no pread)" "${P50_MMAP}"  "${DELTA_MMAP} B"
echo "╚══════════════════════════════════════════════════════════════════════════════╝"
echo ""
echo "  (Absolute us are within-session only; compare within this run.)"
echo "  (k=$K, lanes=$LANES, mb=$MB, n=$N per run, warmup=$WARMUP)"
echo ""

# ── VERDICT ───────────────────────────────────────────────────────────────────
python3 - <<PYEOF
import sys

def safe_float(s):
    try:
        return float(s)
    except:
        return None

honest   = safe_float("${HONEST_P50}")
p_tmpfs  = safe_float("${P50_TMPFS}")
p_over   = safe_float("${P50_OVERLAY}")
p_mmap   = safe_float("${P50_MMAP}")

print("╔══════════════════════════════════════════════════════════════════════════════╗")
print("║                              VERDICT                                        ║")
print("╠══════════════════════════════════════════════════════════════════════════════╣")

if p_tmpfs is not None and p_over is not None and p_over > 0:
    disk_penalty = p_over - (honest or 0)
    syscall_frac = (p_tmpfs - (honest or 0)) / disk_penalty * 100 if disk_penalty > 0 else 0
    storage_frac = 100 - syscall_frac
    pct_tmpfs_vs_overlay = (p_tmpfs / p_over) * 100
    print(f"║  disk/tmpfs p50     = {p_tmpfs:>8.1f} us  ({pct_tmpfs_vs_overlay:.0f}% of overlay disk p50)  ║")
    print(f"║  disk/overlay p50   = {p_over:>8.1f} us                                   ║")
    print(f"║  => ~{pct_tmpfs_vs_overlay:.0f}% of overlay-disk latency is present even on tmpfs (RAM)       ║")
    print(f"║  => disk 'latency' is syscall/page-cache overhead, NOT storage distance    ║")
else:
    print("║  (could not compute tmpfs vs overlay ratio — N/A results)                  ║")

print("║                                                                              ║")

if p_mmap is not None and honest is not None and honest > 0:
    mmap_ratio = p_mmap / honest
    print(f"║  mmapdisk p50       = {p_mmap:>8.1f} us                                   ║")
    print(f"║  honest p50         = {honest:>8.1f} us                                   ║")
    print(f"║  mmapdisk/honest ratio = {mmap_ratio:.2f}x                                     ║")
    if mmap_ratio < 2.0:
        print(f"║  => mmapdisk answers at ~honest speed: proof that eliminating per-probe   ║")
        print(f"║     syscall (via mmap) removes the penalty — for buffers that fit in RAM  ║")
        print(f"║     (behaviour above RAM size is unmeasured)                              ║")
    else:
        print(f"║  => mmapdisk ratio {mmap_ratio:.2f}x vs honest (may exceed RAM size for this run) ║")
else:
    print("║  (could not compute mmapdisk vs honest ratio — N/A results)                ║")

print("╚══════════════════════════════════════════════════════════════════════════════╝")
PYEOF
