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

The weights themselves are the other half: memory the workload already uses, which
filler then doesn't have to cover.
[ADR 0009](docs/decisions/0009-challenge-the-weights-in-place.md) (proposed) decides
to challenge them where the engine holds them. The CPU path ships (manifest, segment
table, no-upload handshake); the rest needs a GPU.

- [ ] **The GPU prover inside vLLM.** A plugin package of its own, with its own
  `vllm.general_plugins` entry point. It wraps `Worker.load_model` to collect the weight
  tensors, refuses any that aren't device memory, builds a `Segments` table on the
  device, and serves challenges from a thread on its own stream, starting only after
  warm-up. The kernel is `k_lane` with a `SegLoad` loader. *Done when:* the GPU
  known-answer test passes over a segmented buffer, and on one H100 a vLLM serving the
  int8 Llama 3.1 8B answers a `--weights ... --upload no` verifier with zero errors.
- [ ] **Check that vLLM holds the checkpoint's bytes.** The manifest assumes each int8
  tensor sits in memory exactly as stored, stacked as vLLM stacks it. *Done when:* a
  start-up check compares every collected tensor with its manifest pieces and reports
  each mismatch by name, and the int8 tensors all match. Widen `--dtype` only for the
  dtypes that pass.
- [ ] **Honest timing while vLLM serves.** The challenge kernel shares the card with
  inference, so the honest distribution under load is what a threshold must clear.
  *Done when:* the resolution sweep's offload points are measured against an honest
  baseline taken under a serving load, and vLLM's own `--cpu-offload-gb` is measured as
  a cheat.
- [ ] **How compressible the weights are.** A prover that stores the weights compressed
  or sparse and rebuilds a probed word fast enough passes the challenge with less memory
  than the dense size. Filler is incompressible; weights aren't. *Done when:* the saving
  a prover can get this way, and the cost of rebuilding a probed word, are measured for
  the int8 Llama, and ADR 0009 states the bound.

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

- [ ] **Understand offload to a neighbor GPU.** Every measurement so far ran on
  one GPU, where the only place to hide data was host memory over PCIe. On a
  multi-GPU server the GPUs are linked to each other directly (NVLink, through
  NVSwitches), so a prover could keep part of the buffer in another GPU's
  memory instead, which is much closer to local speed than host memory. Nothing in the
  adversary taxonomy covers this yet. This is a direction to understand, not a
  design: what the threat model should say about the other GPUs in the box,
  and whether timing can separate it at all. *Done when:* the question is
  written up well enough to decide whether it needs a decision record, an
  experiment, or both.

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
