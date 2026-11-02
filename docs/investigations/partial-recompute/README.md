# Partial recompute

## Question

The build-buffer design ([ADR 0007](../../decisions/0007-buffer-generator-memory-hard-function.md))
ships the prover a recipe instead of the buffer, so the prover can always regenerate a
block it threw away. Does Argon2d make that regeneration slow enough for the timing
detector to catch a GPU prover that discards part of the buffer, and how much slower is
the response?

## Method

`cheating_prover_cuda --mode recompute --keep f` builds the 4 GiB Argon2d buffer, keeps
a random fraction `f` of its 1 KiB blocks on the card, and frees the rest. Each
challenge runs on the GPU as usual until a thread reads a discarded block. That thread
pauses, the host rebuilds every requested block on 23 threads, copies the rebuilt blocks
to the card, and the paused threads run again. A rebuild recurses through whatever the
block depends on: the previous block, the block Argon2d references, and, for `t >= 2`,
the block's own value from the previous pass
([`argon2d_rebuild.hpp`](../../../src/argon2d_rebuild.hpp)).
[`tests/rebuild_argon2d`](../../../tests/rebuild_argon2d.cpp) checks that every rebuilt
block matches a full build.

Each challenge has a budget of 65,536 block computations per thread. A cheat that runs
out gives up and sends a wrong answer, as one that ran out of time would.

[`gpu_recompute.sh`](gpu_recompute.sh) swept `f` and the pass count `t`, and measured
the honest prover before every point because RunPod doesn't allow clock locking. The run
used one H100 SXM in one session on 2026-10-22, with a 4096 MiB buffer, `k` = 262,144,
16,384 lanes, the tree fold, and 16 Argon2 lanes. The container had a quota of 23.8
CPUs.

## Finding

[MEASURED] **With one pass (`t=1`), discarding even 0.1% of the blocks is caught by
timing.** The cheat answers correctly but slowly, and the cost grows with each block it
discards. Honest p50 had a median of 185.6 µs and a standard deviation (sd) of 29.4 µs
across 6 baselines. A point counts as separated above median + 3 sd, which is 273.8 µs.

| Kept | HBM saved | p50 | × honest | Wrong answers | Blocks rebuilt per challenge |
|---|---|---|---|---|---|
| 1.000 | 0% | 212.8 µs | 1.15× | 0 / 30 | 0 |
| 0.999 | 0.1% | 704.4 µs | 3.79× | 0 / 30 | 246 |
| 0.990 | 1% | 1.45 ms | 7.83× | 0 / 30 | 2,538 |
| 0.950 | 5% | 3.63 ms | 19.5× | 0 / 30 | 12,658 |
| 0.900 | 10% | 6.32 ms | 34.0× | 0 / 30 | 25,408 |
| 0.500 | 50% | 269 ms | 1,449× | 30 / 30 | 127,078 (out of budget) |

At `t=1` a missing block costs about one block computation, because the blocks it
depends on are usually ones the cheat kept. The extra time therefore scales with the
number of discarded blocks a challenge reads, which is roughly `k × (1 − f)`.

[MEASURED] **With two passes (`t=2`, the default), the cheat can't produce a correct
answer at all.** Every challenge with any block discarded ran out of budget and came
back wrong: 30 of 30 at each kept fraction from 0.999 down to 0.5. A missing block needs
its own first-pass value, which nobody kept, so a single rebuild pulls in most of the
first pass. In a laptop probe at 64 MiB, that was about 38,000–53,000 block computations
per missing block. The verifier saw p50s from 354 ms at keep 0.999 to 31.8 s at keep
0.5, against an honest median of 216.4 µs. Read those latencies as "late and wrong," not
as a rebuild cost: they depend on the budget, and correctness already catches the cheat.

[MEASURED] **The pause-and-resume answer loop costs almost nothing on its own.** At keep
1.0 nothing is discarded, and the cheat ran at 1.15× honest at `t=1` and 1.10× at `t=2`.
Neither point separated.

## Limits

- **One card, one session.** Only the ratios carry. The honest p50 drifted from 212 µs
  to 128 µs across the `t=1` baselines, which is why each point has its own baseline.
- **The measured cost is a lower bound for this design of cheat.** The host rebuild
  reads kept blocks from a host copy, so it never waits on PCIe for one. The cheat also
  keeps a random fraction, where a real one would keep the blocks that are costliest to
  rebuild.
- **A rebuild on the GPU itself wasn't tried.** Each missing block costs one chain of
  dependent block computations, so it's limited by latency rather than throughput. That
  probably favors a fast CPU core, but the comparison is unmeasured.
- **Discarding less than 0.1% wasn't swept.** At `t=1` that's the region that could fall
  inside the honest spread. Raising `k` would widen the gap there.
- **A cheat that runs out of budget frees a large block cache between challenges.**
  That freeing ran while the verifier's next challenge was waiting, so in those rows the
  verifier's p50 is about twice the cheat's own answer time: 354 ms against 159 ms at
  `t=2` keep 0.999, and 269 ms against 107 ms at `t=1` keep 0.5. This doesn't change any
  verdict, because those answers were wrong.
- **The `t=2` keep 0.999 cheat log has no summary line.** The script stops the prover
  before it can print one, so that row of `summary.txt` reads `nan`. Its per-challenge
  lines in `results/p-keep-t2-0.999.events` are complete.

## Reproduce

On a GPU box that passed the gate (see the `gpu-demo` skill):

```bash
bash docs/investigations/partial-recompute/gpu_recompute.sh --threads <allowed CPUs>
```

`results/` holds the verifier JSON for every trial, the cheat's per-challenge event
lines (`*.events`), `session.txt`, and `summary.txt`.
