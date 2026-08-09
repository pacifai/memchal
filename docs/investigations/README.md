# Investigations

Self-contained analyses that answered a technical question during stabilization.
Each folder has a `README.md` (question → method → finding → reproduce), the
script that produced it, and a `results/` snapshot. These are **supporting
analysis, not part of the core PoC** — they explain *why* the harness and the
claims are what they are.

Absolute microseconds in any result are **within-session illustrative only** and
not comparable across sessions (see [decisions/0003](../decisions/0003-timing-detector-scope-and-measurement.md)).

| Investigation | Question | Headline finding |
|---|---|---|
| [disk-is-syscalls](disk-is-syscalls/) | Is the "disk" gap storage distance or syscalls? | Syscall overhead; defeated by an mmap adversary for fits-in-RAM buffers |
| [latency-decomposition](latency-decomposition/) | Where does a challenge's time go? Why is `fake` faster? | ~45 µs transport floor + ~half ALU / ~half DRAM per probe; `fake` is genuinely cheaper (no memory reads) |
| [session-variance](session-variance/) | How much of absolute latency is the environment? | Within-session stable; between-session P/E-core swing up to ~1.6× (load-dependent) → compare within-session only |

Run any of them from the repo root via `./run.sh bash <path>` (or, for
session-variance, `bash <path>` since it launches its own containers).
