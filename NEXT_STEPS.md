# Roadmap

Pending work, grouped by phase and ordered by what unblocks what. Each item is
actionable and carries a "done when" line so it's clear when to check it off.

Three kinds of thing live in different places:

- **Work to do** — this file.
- **Decisions to agree** — an [ADR](docs/decisions/) with `Status: proposed`,
  promoted to `accepted` once agreed.
- **Analyses** — the [investigations](docs/investigations/).

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

- [ ] **Design the generator.** The construction and the decision fork are written up in
  [ADR 0007](docs/decisions/0007-buffer-generator-memory-hard-function.md) (proposed):
  a memory-hard function (lean: Argon2d, scrypt fallback), why the recompute defense
  migrates from correctness to timing/TMTO, and the effect on the protocol and each
  prover. *Done when:* ADR 0007 is accepted and its cost parameters (`m`, `t`, `p`) and
  VRAM footprint are fixed against a real inference workload — measured with the
  prototype below, not asserted.
- [ ] **Prototype it on CPU.** *Done when:* a `cheating_prover` attack that answers
  without genuinely holding the data (regenerate, or hold only part — a new
  `partial-recompute` mode) loses against it, pinned by a known-answer test and an
  invariant, and its TMTO curve backs the cost parameters chosen in ADR 0007.

> Not in scope, noted for the future: **challenging the weights directly** (probe a
> function of the weights the prover already holds for inference) would avoid a
> separate buffer entirely, but it needs the provider to expose or cooperate on the
> weights — the kind of friction we're trying to avoid. Revisit only if that
> constraint changes.

## Now — what the first GPU session surfaced

The port is correct and runs: the known-answer gate passed on an A40, and the
honest prover plus all three adversaries ran end to end
([uvm-needs-hbm](docs/investigations/uvm-needs-hbm/README.md), and the table in
`README.md`). That session raised three problems, in the order to fix them.

- [ ] **Collapse the tree fold's per-level launches.** The tree fixed the
  scaling — 8x the lanes cost it 1.24x against the sequential fold's 8.1x — but
  it is now launch-bound: ~0.09 ms at 16k lanes and ~0.11 ms at 131k is 15 and 18
  kernel launches at roughly 5 us each, not work. Doing the first several levels
  in shared memory inside one kernel would take the fold well under the kernel's
  own time. It is still the largest single term at 131k lanes. *Done when:* the
  fold is smaller than the kernel in an honest response, and the vectors still
  pass.
- [ ] **Push the resolution below 5%.** The partial-residency sweep separated at
  5% offload and not at 2%, but the limit is the honest distribution's width on a
  card whose clocks cannot be locked — 11.6 us of spread on a 188.8 us median —
  not anything physical. More samples per point, more probes per challenge, or a
  provider that permits clock locking should all move it. *Done when:* the same
  sweep runs with a narrowed baseline and the resolution is restated.
- [ ] **Sweep resolution at deployment scale.** 5% was measured on a 4096 MiB
  buffer. The case that matters is tens of gigabytes, where both the absolute
  hideable bytes and the probe coverage per challenge change. *Done when:* the
  curve exists at a buffer size representative of a real workload.
- [ ] **Give the partial cheat a brain.** It offloads a contiguous tail. A real
  adversary would pick *which* bytes to offload — coldest first, or whatever the
  probe distribution touches least. 5% is therefore an upper bound on the
  detector's resolution against a thoughtful opponent, not a guarantee. *Done
  when:* an offload policy that chooses its bytes is measured against the naive
  one.
> The H100 run is **done**: the gate passed on sm_90, the fold A/B and the
> resolution sweep both ran, and their numbers are in the two investigations
> above. The A40 numbers stand only as the first-light record.

> Two provider questions are now **settled** for RunPod, from the machine record:
> clock locking is **denied**, and `/dev/infiniband` is **absent**, so the RDMA
> experiments need a different provider. Neither blocks the port.

## Later — the decision rule

- [ ] **A decision rule with error rates.** Turn the harness's separations into an
  accept/reject classifier. *Done when:* it outputs a verdict given a response and a
  claimed configuration, with measured false-positive and false-negative rates.
  Depends on the resolution sweep above: a cliff with no gradient gives a
  classifier nothing to set a threshold on.
- [ ] **Open a random subset of lanes.** The tree fold produces a Merkle root, so
  the structure for opening lanes is now there, but nothing opens any: the
  response is still one 64-bit value and the verifier still recomputes every
  probe, which costs as much as proving. Returning the root plus a sampled set
  of lane openings makes verification sublinear in `k` and takes the response
  past 64 bits at the same time. *Done when:* the verifier checks a subset, and
  soundness is published as a function of the number of lanes opened.

## Backlog

- [ ] **Optional: a bare-metal CPU cold read** (buffer > RAM and/or `O_DIRECT`,
  pinned with `taskset`). Not on the path — the real timing check happens on the GPU
  — but it's a cheap one-command early signal of whether the timing gap survives real
  storage. Run it only if you want that signal before the port.
- [ ] **Wire byte-order** is native and assumes same-endian peers; make it explicit
  if the protocol ever leaves loopback.
