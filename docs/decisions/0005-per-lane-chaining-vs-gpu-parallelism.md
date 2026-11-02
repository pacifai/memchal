# ADR 0005 — Per-lane chaining: latency-hardness vs GPU parallelism

**Status:** accepted

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
- **On the GPU, `k` grows with the lane count.** A GPU wants many thousands of
  lanes, and at a fixed `k` that makes each chain shallow. The verifier refuses
  any challenge with `k / lanes` below 16, so the GPU runs use 16,384 lanes with
  `k = 262,144`, a chain depth of 16.
- **Orthogonal to build-buffer.** This is *latency*-hardness (can't prefetch a predictable
  pattern). It does nothing for *capacity*-hardness / the recompute regime, which
  needs a memory-hard **generator** — a separate mechanism
  ([ADR 0007](0007-buffer-generator-memory-hard-function.md)).

## Related
[ADR 0003](0003-timing-detector-scope-and-measurement.md) (chain depth = k/lanes),
[ADR 0004](0004-adversary-taxonomy-and-defenses.md) (the defense in the taxonomy),
`prefetch-needs-pattern` (measured cost).
