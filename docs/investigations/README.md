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
| [disk-is-syscalls](disk-is-syscalls/) | Is the "disk" gap storage distance or syscalls? | Syscall overhead; the mmap adversary looks honest because the page cache holds its file |
| [mmap-needs-ram](mmap-needs-ram/) | Does the mmap adversary survive without RAM to cache the file? | No — starved below the buffer it's ~840× honest; timing tracks residency, not device |
| [latency-decomposition](latency-decomposition/) | Where does a challenge's time go? Why is `fake` faster? | ~45 µs transport floor + ~half ALU / ~half DRAM per probe; `fake` is genuinely cheaper (no memory reads) |
| [session-variance](session-variance/) | How much of absolute latency is the environment? | Within-session stable; between-session P/E-core swing up to ~1.6× (load-dependent) → compare within-session only |

Run from the repo root via `./run.sh bash <path>` — except session-variance and
mmap-needs-ram, which launch their own containers, so run those as `bash <path>`.
