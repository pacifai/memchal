# Roadmap

Pending work, grouped by phase and ordered by what unblocks what. Each item is
actionable and carries a "done when" line so it's clear when to check it off.

Three kinds of thing live in different places:

- **Work to do** — this file.
- **Decisions to agree** — an [ADR](docs/decisions/) with `Status: proposed`,
  promoted to `accepted` once agreed.
- **Analyses** — the [investigations](docs/investigations/).

## Now — decisions and near-term build

- [ ] **Agree the cipher choice.** Vendored ChaCha20 vs a dependency
  ([ADR 0006](docs/decisions/0006-chacha20-vendored-vs-dependency.md), proposed).
  *Done when:* the ADR is accepted and the code matches it.
- [ ] **Make the defenses safe by default.** The send-buffer defenses ship off
  (`--gen splitmix64`, `--chain independent`, `--nonce sequential`) so the attacks
  are demonstrable; any adversarial use needs all three on. Flip the defaults for
  non-benchmark use and keep a documented benchmark opt-out. *Done when:* defaults
  changed, tests updated, and the README says which mode is which.

## Next — proving residency of the real workload

Today the challenge proves you're holding a specific random buffer the verifier sent.
The goal is to prove you're holding something tied to the actual workload — not
generic filler — while keeping friction with model providers low.
[ADR 0004](docs/decisions/0004-adversary-taxonomy-and-defenses.md) explains why the
send-buffer design doesn't get there.

The plan is a **memory-hard, weight-bound generator** (the build-buffer design): ship
a short recipe, and both sides build a dataset from it that is expensive to regenerate
on demand (so the prover must store it) and tied to the model. The memory-hardness
falls on the one-time *generation*, not the per-challenge answer, so it does **not**
multiply the per-challenge cost — that cost is inescapable and accepted either way.

- [ ] **Design the generator.** *Done when:* a proposed ADR gives the construction
  (Argon2- or scrypt-style), the threat model, its cost parameters, and the VRAM
  footprint of the generated buffer against a real inference workload.
- [ ] **Prototype it on CPU.** *Done when:* a `cheating_prover` attack that answers
  without genuinely holding the data (regenerate, or hold only part) loses against it,
  pinned by a known-answer test and an invariant.

> Not in scope, noted for the future: **challenging the weights directly** (probe a
> function of the weights the prover already holds for inference) would avoid a
> separate buffer entirely, but it needs the provider to expose or cooperate on the
> weights — the kind of friction we're trying to avoid. Revisit only if that
> constraint changes.

## Later — GPU port and decision rule

Timing is a core signal (it's what proves the data is in *fast* memory, not just
stored somewhere), but the CPU-in-Docker environment can't measure it faithfully and
CPU-DRAM latencies don't transfer to GPU-VRAM. So the authoritative timing/residency
validation rides on the deployment target — the GPU port — rather than a bare-metal
CPU run. The CPU version's job is to prove the design: the challenge math, the
correctness detector, and the *shapes* of the defenses (chaining defeats prefetch,
resident-vs-not is distinguishable in principle).

- [ ] **The GPU port**, with timing validation built in. *Done when:* the answer path
  runs as a CUDA kernel with the buffer in VRAM (`cudaMalloc`) and `map_index`'s
  128-bit multiply replaced by `__umul64hi`, **and** a cold read on real hardware
  shows the timing gap separates VRAM-resident from host-RAM or disk with the true
  per-access cost. Settle the adversaries and the design on CPU first.
- [ ] **A decision rule with error rates.** Turn the harness's separations into an
  accept/reject classifier. *Done when:* it outputs a verdict given a response and a
  claimed configuration, with measured false-positive and false-negative rates.
  Depends on the timing regime being settled on real hardware (the GPU validation
  above).

## Backlog

- [ ] **Responses ≥128 bits with a cryptographic hash.** 64 bits is guessable or
  precomputable against a real adversary. *Done when:* the response width grows, the
  known-answer test is updated, and the wire version bumps.
- [ ] **Optional: a bare-metal CPU cold read** (buffer > RAM and/or `O_DIRECT`,
  pinned with `taskset`). Not on the path — the real timing check happens on the GPU
  — but it's a cheap one-command early signal of whether the timing gap survives real
  storage. Run it only if you want that signal before the port.
- [ ] **Wire byte-order** is native and assumes same-endian peers; make it explicit
  if the protocol ever leaves loopback.
