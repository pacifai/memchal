# ADR 0005 — Per-lane chaining: latency-hardness vs GPU parallelism

**Status:** accepted for CPU (the send-buffer design); **explicitly to be revisited at the GPU port**

## Context
The latency-hardness defense (`--chain dependent`, [ADR 0004](0004-adversary-taxonomy-and-defenses.md))
defeats the prefetch adversary by making each probe's index depend on the *previously
read word* — pointer-chasing, so the offsets can't be enumerated or prefetched ahead
of the reads. But pointer-chasing is inherently **sequential**: you can't issue read
`i+1` until read `i` returns. That is in direct tension with the parent goal, a **GPU**
port, which is valuable precisely because it runs tens of thousands of threads in
parallel. Maximum latency-hardness and maximum parallelism pull in opposite directions.

## Decision
Structure the challenge as **many independent lanes, each a sequential chain**:
- *within* a lane, indices chain (latency-hard, depth = `k/lanes`);
- *across* lanes, work is embarrassingly parallel (maps to GPU threads).

Enforce a floor `k/lanes ≥ 16` (`kMinChainDepth`) so "dependent" can't silently
degrade to no hardness.

## Alternatives considered
| Shape | Latency-hardness | Parallelism | Verdict |
|---|---|---|---|
| One **global** chain (a single k-deep pointer-chase) | maximal | **none** | Unusable on GPU; even on CPU it serializes the whole challenge. Rejected. |
| **Fully independent** (today's default) | none (prefetchable) | maximal | The vulnerable baseline; rejected as a defense. |
| **Per-lane chains × many lanes** (chosen) | = chain depth `k/lanes` | = `lanes` | The middle ground: parallel across lanes, hard within a lane. |

## Consequences
- **Hardness is capped at `k/lanes`, not `k`.** The first index of each lane is
  nonce-only (predictable), and an attacker can pipeline `chain_depth` probes across
  `lanes` lanes. The floor mitigates but does not cure this (see ADR 0004 / the
  prefetch investigation). Getting more hardness means *deeper* chains → *fewer*
  lanes → less parallelism.
- **Honest cost ~3–4×.** Pointer-chasing kills memory-level parallelism (the hardware
  can't prefetch the next address until the current read completes). That is the
  intrinsic price of latency-hardness and is paid by the honest prover too.
- **The GPU port will re-open this.** A GPU wants many thousands of lanes; at fixed
  `k` that forces shallow chains (weak). To keep both, the port likely needs `k` to
  grow with the thread count (so `k = lanes × depth` stays large with a healthy
  `depth`), or a different memory-hard construction. This tradeoff — thread count vs
  chain depth vs total probes — should be decided deliberately when the kernel is
  written, not inherited by accident.
- **Orthogonal to build-buffer.** This is *latency*-hardness (can't prefetch a predictable
  pattern). It does nothing for *capacity*-hardness / the recompute regime, which
  needs a memory-hard **generator** (Argon2-style) — a separate mechanism (NEXT_STEPS).

## Related
[ADR 0003](0003-timing-detector-scope-and-measurement.md) (chain depth = k/lanes),
[ADR 0004](0004-adversary-taxonomy-and-defenses.md) (the defense in the taxonomy),
[prefetch-needs-pattern](../investigations/prefetch-needs-pattern/) (measured cost).
