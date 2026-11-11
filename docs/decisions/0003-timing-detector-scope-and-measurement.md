# ADR 0003: What timing shows, and how to measure it

**Status:** accepted

## Context

The first prototype said a slow answer meant the prover read from storage
instead of RAM, and it reported single absolute times. Two questions followed:
what does a slow answer actually mean, and how far can one measured time be
trusted?

Experiments on a CPU, in Docker on Apple Silicon, answered both.

## What the measurements showed

- **The gap came from system calls, not storage.** A prover that reads each
  word with a `pread` call was as slow reading a file in RAM (`tmpfs`) as
  reading one on disk: 94% to 100% of the disk time. The disk file was read
  from the drive once, then served from the operating system's page cache. A
  prover that maps the same file into memory (`mmapdisk`) answers at about the
  honest prover's speed.
- **Timing tracks whether the data is in fast memory, not which device stores
  it.** The `mmapdisk` prover is fast because the page cache keeps its file in
  RAM. Given a memory limit smaller than the buffer, it slowed to about 840
  times the honest prover, with about half its reads waiting on storage. A
  `pread` prover under the same limit was within 2% of it. Two caveats: the
  limit stands in for physical RAM, and the Mac caches the container's disk, so
  840x understates the real gap.
- **A read costs about half hashing and half waiting for memory,** on top of a
  fixed overhead for sending and receiving each challenge. That overhead was
  about 40% of the honest prover's total time.
- **Absolute times don't carry over between sessions.** The container's
  virtual CPUs land on the Mac's fast or slow cores, fixed for a session, so
  the same run can be up to 1.6 times slower in one session than another.
  Within one session, times are steady.

## Decision

1. **Scope the claim.** A slow answer shows that the data the challenge read
   wasn't in fast memory at that moment. It doesn't show which device held the
   data, and it doesn't tell RAM from the page cache, since both are fast
   memory. A correct answer still shows the prover had the data.
2. **Measure within one session.** Run every prover in the same session,
   alternating in a random order that is logged. Include a run that reads
   nothing (`--samples 0`) to measure the fixed overhead. Report ratios to the
   honest prover, and the honest time minus the overhead, with their error.
   Don't report a percentile the number of answers can't support. Never
   present an absolute time as portable, and label every result with its
   session.

## Consequences

- The README's results compare provers within one session, and say so.
- `mmapdisk` is a cheating prover in its own right: it shows the page cache
  counts as fast memory, and that it stops counting once memory runs out.
- `scripts/run_trials.py` runs every mode alternately in one session.
- These numbers come from a CPU. GPU timing has to be measured on a GPU, which
  is where the check is meant to run.
