#!/usr/bin/env python3
"""report.py — render the disk-is-syscalls experiment table + verdict.

Invoked by disk_is_syscalls.sh after the runs finish. Reads the row file and
parameters from the environment (ROWS, K, LANES, MB, N, WARMUP) and prints the
formatted table and VERDICT block via scripts/fmt_table.py. Kept as its own file
(rather than an inline heredoc) so it can be linted/edited like normal Python.
"""
import os, sys

# Import fmt_table from the repo's scripts/ regardless of the caller's cwd.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
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
