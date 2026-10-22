# ADR 0004 — Adversary taxonomy and defenses (the send-buffer design)

**Status:** accepted (implemented as toggles; all attacks are `cheating_prover` modes, all defenses are verifier flags)

## Context
The stabilized detectors prove "you can't answer without *reading* the data." That is
weaker than proof-of-retention: there are several ways to answer fast without
genuinely holding the data, and each needs its own defense. Rather than assert the
gap, we built each attack as a mode and each defense as a toggle, so the demo can flip
a defense on and watch the attack get caught. This ADR records the taxonomy;
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

## Safety: the defaults are the *secure* settings
The verifier ships with all three defenses on: `--gen chacha`, `--chain dependent`,
and `--nonce random` are the defaults, so a bare `./verifier` is secure out of the box.
The **benchmark opt-out** is to name the weak setting explicitly — `--gen splitmix64
--chain independent --nonce sequential` — which reproduces the byte-identical
independent/splitmix64/sequential path the known-answer test and the headline measurements are pinned
to. Nothing about the reproducibility relies on the *default* being weak: the
known-answer test computes the math directly, the harness (`run_trials.py`) always
passes the toggles it measures explicitly, and the demo sets its own starting toggles
(vulnerable, so a viewer can turn a defense on and watch the catch). Flipping the
default only changes what an *unflagged* run does.

> Earlier this ADR shipped the defaults the other way — every defense off — so the
> attacks were demonstrable with no flags. That traded a safe out-of-the-box posture
> for demo convenience the demo never needed (it sets its own toggles). The defaults
> are now secure; the weak path is one explicit flag away.

Selecting a defense is still not the same as it being effective: a config can look
defended while being toothless (e.g. `dependent` with `k = lanes` gives chain depth 1).
The verifier rejects `k/lanes < 16` and, because `dependent` is now the default, an
unflagged small-`k` run is refused rather than silently toothless. The demo/UI can
still *present* "dependent" as on without the depth to back it, so treat "a defense is
selected" and "a defense is effective" as separate facts. See
[ADR 0005](0005-per-lane-chaining-vs-gpu-parallelism.md) for the chain-depth caveat
specifically.

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
