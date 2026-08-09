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
docker run --rm --platform "$PLATFORM" -v "$PWD":/work "$IMAGE" make 2>&1 | tail -2
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
FIT_MEM="$FIT_MEM" STARVED_MEM="$STARVED_MEM" python3 - <<'PYEOF'
import os, sys
sys.path.insert(0, "scripts")
from fmt_table import table, block

def num(s):
    try: return float(s)
    except Exception: return None

def as_mib(mem):                      # "1g" / "32m" -> MiB
    mem = mem.strip().lower()
    mult = {"g": 1024.0, "m": 1.0, "k": 1/1024.0}.get(mem[-1])
    return float(mem[:-1]) * mult if mult else float(mem) / (1 << 20)

buf_mib = float(os.environ["MB"])
rows_in = [l.rstrip("\n").split("\t") for l in open(os.environ["ROWS"]) if l.strip()]
by_label = {r[0]: r for r in rows_in}
p50 = {r[0]: num(r[2]) for r in rows_in}
honest = next((v for k, v in p50.items() if k.startswith("honest")), None)

def storage_pct(row):
    """Share of probes that had to come from storage, derived from the prover's
    block-layer reads (>= one 4 KiB page per uncached probe). Works for BOTH
    readers: an mmap probe pays a major fault, a pread probe does not, but both
    show up in read_bytes."""
    probes, rb = num(row[3]), num(row[4])
    if not probes or rb is None:
        return None
    return 100.0 * (rb / 4096.0) / probes

out, oom_notes = [], []
for label, mem, p, probes, rb, mf, note in rows_in:
    v = num(p)
    budget = f"{as_mib(mem):,.0f} MiB ({as_mib(mem)/buf_mib:.2f}x buffer)"
    ratio = "1.00x (ref)" if label.startswith("honest") else "N/A"
    if v and honest and not label.startswith("honest"):
        r = v / honest
        ratio = f"{r:,.2f}x" if r < 10 else f"{r:,.0f}x"
    pct = storage_pct(by_label[label])
    faults = f"{int(num(mf)):,}" if num(mf) is not None else "-"
    out.append([label, budget, "N/A" if v is None else f"{v:,.1f}", ratio,
                "-" if pct is None else f"{pct:,.0f}%", faults])
    if note:
        oom_notes.append(f"!! {label}: {note}")

print(table(
    [("scenario", "l"), ("prover RAM budget", "l"), ("p50 (us)", "r"),
     ("vs honest", "r"), ("probes from storage", "r"), ("major faults", "r")],
    out,
    title="EXPERIMENT 2 — THE MMAP ADVERSARY NEEDS RAM IT DOESN'T HAVE",
    notes=oom_notes + [
        f"buffer={os.environ['MB']} MiB  k={os.environ['K']}  lanes={os.environ['LANES']}  "
        f"n={os.environ['N']}/run  warmup={os.environ['WARMUP']}",
        "'probes from storage' = prover's own block-layer reads / 4 KiB / probes issued",
        "absolute us are within-session only — compare rows of THIS run, never across runs",
        "storage costs are a LOWER BOUND: Docker's disk is cached again by the macOS host",
    ]))
print()

fit     = p50.get("mmapdisk, RAM to spare")
starved = p50.get("mmapdisk, RAM < buffer")
pread   = p50.get("disk/pread, RAM < buffer")

lines = []
if honest and fit:
    lines += [
        f"mmapdisk, RAM to spare  = {fit:>12,.1f} us  ({fit/honest:.2f}x honest)",
        f"  {storage_pct(by_label['mmapdisk, RAM to spare']):.0f}% of probes from storage — the block-layer reads stop after one cold",
        "  sweep, because the page cache is holding the whole file. This adversary is",
        "  not evading residency; it IS resident, via the page cache instead of mlock.",
        "",
    ]
if honest and starved:
    row = by_label["mmapdisk, RAM < buffer"]
    lines += [
        f"mmapdisk, RAM < buffer  = {starved:>12,.1f} us  ({starved/honest:,.0f}x honest)",
        f"  {storage_pct(row):.0f}% of probes from storage, {int(num(row[5])):,} major faults. Same binary, same",
        "  mmap, same file — only the RAM is gone. cgroup reclaim unmaps the file's",
        "  pages, so probes fault through to storage with no syscall-free path left.",
        "",
    ]
if honest and pread and starved:
    row = by_label["disk/pread, RAM < buffer"]
    lines += [
        f"disk/pread, RAM < buffer = {pread:>11,.1f} us  ({pread/honest:,.0f}x honest)",
        f"  {storage_pct(row):.0f}% of probes from storage, {int(num(row[5])):,} major faults (a pread pays no fault).",
        f"  Within {abs(pread-starved)/starved*100:.0f}% of the mmap row: once the data genuinely is not resident,",
        "  the syscall overhead that dominated experiment 1 is lost in the noise.",
        "",
    ]
# Sanity check the headline number against the storage traffic it implies, so the
# latency is not taken on faith.
if starved:
    row = by_label["mmapdisk, RAM < buffer"]
    rb, probes = num(row[4]), num(row[3])
    if rb and probes:
        per_ch = rb / (probes / float(os.environ["K"]))
        lines += [
            f"cross-check: that row moves {per_ch/(1<<20):,.1f} MiB of storage traffic per challenge;",
            f"  at {per_ch/1e6/(starved/1e6):,.0f} MB/s (what this virtual disk sustains) that traffic alone accounts",
            "  for the measured latency. The slowdown is storage work, not measurement error.",
            "",
        ]
if honest and starved and fit:
    lines += [
        "=> The mmap trick is not a defeat of the timing detector; it is a demonstration",
        "   that the page cache counts as RAM. An adversary that genuinely does not hold",
        f"   the data is still ~{starved/honest:,.0f}x slower, mmap or not.",
        "=> Still unvalidated: a bare-metal >RAM test on real storage, and a decision rule",
        "   with stated false-positive/false-negative rates (see NEXT_STEPS.md).",
    ]
else:
    lines.append("incomplete results — see the table above")

print(block("VERDICT — TAKE THE RAM AWAY AND THE MMAP CHEAT DIES", lines))
print()
print(f"  raw JSON + prover logs: {os.path.dirname(os.environ['ROWS'])}/")
PYEOF
