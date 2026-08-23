# ADR 0004 — Adversary taxonomy and defenses (the send-buffer design)

**Status:** accepted (implemented as toggles; all attacks are `cheating_prover` modes, all defenses are verifier flags)

## Context
The stabilized detectors prove "you can't answer without *reading* the data." That is
weaker than proof-of-retention: there are several ways to answer fast without
genuinely holding the data, and each needs its own defense. Rather than assert the
gap, we built each attack as a mode and each defense as a toggle (defaulting to the
vulnerable behavior so the existing KAT/measurements are unchanged and the demo can
flip a defense on and watch the attack get caught). This ADR records the taxonomy;
the numbers live in the investigations, the deeper framing in
[NEXT_STEPS](../../NEXT_STEPS.md) and [ADR 0003](0003-timing-detector-scope-and-measurement.md).

All of this is the **send-buffer** design (the prover receives the bytes; the
verifier keeps the seed). The **build-buffer** design (the verifier ships a seed,
which needs a *memory-hard* generator) is out of scope and remains an open research
problem.

## The three escape hatches and their defenses

| Attack (`--mode`) | How it answers without retaining | Defeated by (verifier flag) | Why |
|---|---|---|---|
| **recompute** | Recovers the buffer's PRNG seed from one word (`splitmix64` is invertible) and regenerates every probe in O(1); holds nothing. | `--gen chacha` | ChaCha20 keyed by a **random 256-bit key** is not invertible → the seed can't be recovered → regenerated data is wrong → correctness catches it. (Capacity-hardness.) |
| **prefetch** | Data on disk; enumerates the independent challenge's offsets up front and `MADV_WILLNEED`-batches the pages, dodging serial faults. Real **only under memory pressure**. | `--chain dependent` | Each next index depends on the last-read word (pointer-chasing) → offsets can't be enumerated/prefetched. Strength = chain depth = k/lanes; floor `k/lanes ≥ 16` enforced. (Latency-hardness.) |
| **precompute** | Nonces are predictable (`nonce_base+i`); a background thread precomputes answers for future nonces from on-disk data and serves from cache — instant, zero residency. Chaining/ChaCha don't help (done offline). | `--nonce random` | Each nonce is drawn from a ChaCha20 CSPRNG (`random_device`-keyed) → unpredictable → nothing to precompute → must read during the timed window. |

The **correctness detector** (`fake` always fails) is unaffected by all three and
remains the one claim that survives regardless.

## Safety: the defaults are the *vulnerable* settings
`--gen splitmix64`, `--chain independent`, and `--nonce sequential` are the defaults,
so that the existing KAT/measurements stay byte-identical and each attack is
demonstrable out of the box. **This means the tool ships with every defense OFF.** Any
adversarial or production use must (a) enable all three (`--gen chacha --chain
dependent --nonce random`) **and** (b) *validate* the configuration — a config can
look defended while being toothless (e.g. `dependent` with `k = lanes` gives chain
depth 1; the verifier now rejects `k/lanes < 16`, but the demo/UI can still present
"dependent" as on). Treat "a defense is selected" and "a defense is effective" as
separate facts. See [ADR 0005](0005-per-lane-chaining-vs-gpu-parallelism.md) for the
chain-depth caveat specifically.

## Consequences and caveats
- **recompute** is cheap and devastating on `splitmix64` (~0.8× honest, 0 bytes held);
  `--gen chacha` fully closes it for send-buffer. It is O(1) random-access, *not*
  memory-hard, so it cannot carry the build-buffer design.
- **prefetch** is a **strawman in the cached regime** (its `madvise`-storm overhead
  loses to a plain mmap read) and a **real ~4× attack when starved**; the defense
  removes the edge only where the attack exists. See
  [prefetch-needs-pattern](../investigations/prefetch-needs-pattern/). The chain floor
  is a floor, not a cure (an attacker can still pipeline `k/lanes` probes across lanes),
  and chaining costs the honest prover ~3–4× (pointer-chasing kills MLP).
- **precompute** needs the verifier to give it offline time; our tight benchmark loop
  masks it, so `--gap-us` exists to model a realistic periodic verifier. Unpredictable
  nonces are cheap and close it outright.
- All three (and all defenses) are measurable by the harness
  (`run_trials.py --gen/--chain/--nonce/--gap-us`), not only visible in the demo.

## Related
Investigations: [disk-is-syscalls](../investigations/disk-is-syscalls/),
[mmap-needs-ram](../investigations/mmap-needs-ram/),
[prefetch-needs-pattern](../investigations/prefetch-needs-pattern/). The send-buffer
vs build-buffer rationale and the weight-bound goal are in the NEXT_STEPS roadmap
("Next — proving residency of the real workload").
