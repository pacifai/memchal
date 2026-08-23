# Investigation: does the mmap adversary survive without RAM to cache the file?

## Question
[disk-is-syscalls](../disk-is-syscalls/) showed the `mmapdisk` adversary — data on
disk, read through a memory-mapped view, no per-probe syscall — answers at ~honest
speed. That looked like a defeat of the timing detector. But it only works because
the container had RAM to spare, so the page cache held the whole file: the
adversary *was* resident, just via the cache instead of `mlock`. So the real
question is: **what happens when the cheater doesn't have the RAM to hold what it
claims?**

## Method
`run_mmap_needs_ram.sh` (runs on the **host** — it launches its own containers).
The prover runs in a container with a **memory budget smaller than the buffer** it
claims to hold (`--memory` = `--memory-swap`, so no swap); the verifier runs in a
**second container sharing the prover's network namespace** — same loopback path as
experiment 1, but **separate memory accounting** so the verifier's own copy doesn't
count against the prover's budget. The prover reports its own block-layer
`read_bytes` and major-fault counters, so "the probes went to storage" is measured,
not inferred. Two code changes made this possible (see the commit): the cheating
prover **streams the upload to disk in flushed chunks** instead of a full-size
`malloc` (so it can run under a budget smaller than the buffer without being
OOM-killed), and reports `major_faults()`.

## Finding
Take the RAM away and the mmap cheat dies. Representative run (`results/run.txt`,
illustrative absolute µs — within-run only):

| scenario | prover budget | p50 | vs honest | probes from storage | major faults |
|---|--:|--:|--:|--:|--:|
| honest (mlock'd RAM) | 1024 MiB (16× buffer) | 96.7 µs | 1.00× | – | – |
| mmapdisk, RAM to spare | 1024 MiB (16× buffer) | 109.6 µs | 1.13× | 2% | 16,384 |
| mmapdisk, RAM < buffer | 32 MiB (0.5× buffer) | 81,313 µs | **841×** | 52% | 471,363 |
| disk/pread, RAM < buffer | 32 MiB (0.5× buffer) | 79,471 µs | 822× | 52% | 0 |

Same binary, same mmap, same file — only the RAM is gone. cgroup reclaim unmaps the
file's pages, so probes fault through to storage with no syscall-free path left. A
`pread` prover on the same budget lands within ~2%: once the data genuinely isn't
resident, the syscall overhead that dominated experiment 1 is noise. Cross-check:
that row moves ~8.4 MiB of storage traffic per challenge; at the ~108 MB/s this
virtual disk sustains, that traffic alone accounts for the latency.

**So the mmap trick is not a defeat — it's a demonstration that the page cache
counts as RAM. Timing tracks residency; an adversary that genuinely doesn't hold
the data is ~840× slower, mmap or not.**

## Limits
The budget is a **cgroup limit standing in for physical RAM**, and Docker's disk is
cached again by the macOS host — so the storage penalty here is a **lower bound**.
The settling experiment is still a bare-metal >RAM run on real storage (NEXT_STEPS).

## Reproduce
```bash
bash docs/investigations/mmap-needs-ram/run_mmap_needs_ram.sh   # from repo root; runs on host
# knobs: MB K LANES N WARMUP FIT_MEM STARVED_MEM CPUS
```

## Consequence
Reframed the timing claim from "syscalls not storage, defeated by mmap" to "timing
tracks residency; the mmap defeat was the adversary being resident via page cache."
See [ADR 0003](../../decisions/0003-timing-detector-scope-and-measurement.md).
