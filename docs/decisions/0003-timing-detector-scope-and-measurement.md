# ADR 0003 — What the timing detector establishes, and how it must be measured

**Status:** accepted (this reframes a headline claim — read before trusting timing)

## Context
The prototype presented timing as proof of memory residency ("`disk`/`partial` are
10–19× slower because probes hit storage instead of RAM") and reported single-shot
absolute latencies. Two investigations tested both the *meaning* of the signal and
the *trustworthiness* of the numbers:
[disk-is-syscalls](../investigations/disk-is-syscalls/),
[latency-decomposition](../investigations/latency-decomposition/), and
[session-variance](../investigations/session-variance/).

## What the measurements showed
- **The gap is syscall overhead, not storage distance.** The same 4096-`pread`/
  challenge loop over **tmpfs** (guaranteed RAM, `read_bytes = 0`) costs ~94–100%
  of the overlay-**disk** latency; the disk file is fetched from the block layer
  exactly once, then served from page cache. An **mmap adversary** (`mmapdisk`:
  data on disk, read via a memory-mapped view, no per-probe syscall) answers at
  **~honest speed** and passes correctness — **for a buffer that fits in RAM**.
- **Per-probe cost is ~half hash-chain ALU, ~half DRAM stall**, riding on a large
  transport floor (~40% of the honest number). "Memory-bound" was an overstatement.
- **Absolute latencies are not portable across sessions.** The VM's vCPUs land on
  Apple-Silicon performance vs efficiency cores, fixed per session, so the absolute
  level can swing up to ~1.6× between sessions (load-dependent; one later capture
  showed only 1.05×). Within a session the level is stable (no mid-run step).

## Decision
1. **Claim scope.** State plainly that the timing detector distinguishes *per-probe
   syscalls from in-process loads*, **not** RAM-vs-storage residency, and that it is
   **defeated by an mmap adversary for buffers that fit in RAM** (above RAM size is
   unmeasured — always keep that caveat so the negative result isn't overclaimed).
   The **correctness detector remains robust** and is the claim that survives.
2. **Measurement discipline.** Measure every mode in ONE session, **interleaved** in
   randomized (logged) order, with the `k=0` transport **floor as a mode in the
   rotation**. Report **within-session ratios** and **paired differences**
   (`honest − floor`) **with propagated error**; withhold percentiles the sample
   size can't support; flag within-session drift. Never present absolute µs as
   portable — stamp every artifact with its `session_id`, environment, and this
   caveat.

## Consequences
- README results rewritten; `mmapdisk` promoted to a first-class cheat mode; the
  harness (`scripts/run_trials.py`) is built around interleaved single-session
  measurement.
- The genuinely-cold-read test on bare metal (buffer > RAM and/or `O_DIRECT`) and a
  `taskset`-pinned rerun are now *load-bearing, not optional* — they are the only
  things that would validate any timing teeth and give portable absolute numbers.
  See NEXT_STEPS.md.
