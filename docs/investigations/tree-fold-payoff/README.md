# Did moving the cross-lane fold onto the card actually pay off?

**Answer: yes, and the reason is the scaling, not the constant.** Eight times the
lanes made the old sequential fold 8.1x more expensive; the tree fold barely
moved. At 131,072 lanes the whole response got 8.9x faster. **[MEASURED]** on one
H100, one session.

## The question

The cross-lane fold combines one hash per lane into a single answer. It was a
strict sequential chain, so it could not be parallelised and had to run on the
host after the kernel. The first GPU session measured it at ~312 us against the
kernel's ~86 us on an A40 — over half the response, and structurally worse as
lanes grow, because the fold is linear in lanes while the kernel is not.

`tree_fold()` replaces the chain with a binary Merkle tree, which is
log2(lanes) parallel passes on the device and returns eight bytes instead of one
hash per lane. It is a different answer, so it is a protocol change with its own
pinned vectors, not a silent optimisation.

This measures whether it was worth it.

## Method

One H100 SXM 80GB, 8192 MiB buffer, chain depth 16, defenses on, n=60 after 15
warmup. Both folds run back to back in the same session at the same geometry,
twice: once at 16,384 lanes and once at 131,072. Prover-reported kernel, PCIe
copy, and fold costs are medians over the traced challenges.

Script: [`run_fold_ab.sh`](run_fold_ab.sh).

## Finding

| fold | lanes | round trip p50 | kernel | PCIe copy | fold |
|---|---:|---:|---:|---:|---:|
| sequential | 16,384 | 414.5 us | 0.029 ms | 0.031 ms | 0.267 ms |
| **tree** | 16,384 | **196.1 us** | 0.025 ms | 0.016 ms | **0.089 ms** |
| sequential | 131,072 | 2489.0 us | 0.094 ms | 0.057 ms | **2.166 ms** |
| **tree** | 131,072 | **278.2 us** | 0.088 ms | 0.019 ms | **0.111 ms** |

Zero verify errors throughout, both folds.

**The scaling is the result.** Going from 16,384 to 131,072 lanes multiplied the
sequential fold by 8.1x (0.267 -> 2.166 ms), tracking lane count exactly as a
linear chain must. The tree fold went up 1.24x (0.089 -> 0.111 ms). At the higher
lane count the sequential fold was 87% of the entire response; the tree fold is
40%, and the response is 8.9x faster.

**The tree fold is now launch-bound, not work-bound.** 0.089 ms at 16,384 lanes
and 0.111 ms at 131,072 is not the shape of something doing eight times the work.
It is 15 and 18 kernel launches respectively, at roughly 5 us each. The fold is
paying for launch overhead, which is why more lanes barely cost more.

That points at the next optimisation, which this change does not make: collapse
the per-level launches into one kernel doing the first several levels in shared
memory. It would take the fold well under the kernel's own time. It is not done,
and the fold is still the largest single term at 131,072 lanes (0.111 ms against
the kernel's 0.088 ms) — better than 87%, not yet negligible.

## What this does not establish

One card, one session, clocks unlockable. The ratios between the two folds in
the same session are the claim; the absolute microseconds are not portable.

Nothing here says the tree fold is *sound*. It says it is fast. Its correctness
rests on the pinned vectors in `tests/known_answer.cpp`, including that sibling
order changes the root, which is what stops a prover permuting lanes after
seeing the challenge.

## Reproduce

```bash
bash scripts/gpu_pod_setup.sh
bash docs/investigations/tree-fold-payoff/run_fold_ab.sh
```
