# Roadmap

Pending work, grouped by phase and ordered by what unblocks what. Each item is
actionable and carries a "done when" line so it's clear when to check it off.

Three kinds of thing live in different places:

- **Work to do** — this file.
- **Decisions to agree** — an [ADR](docs/decisions/) with `Status: proposed`,
  promoted to `accepted` once agreed.
- **Analyses** — the [investigations](docs/investigations/).

## Next — the compression frontier

A cheating prover that stores its buffer **compressed** and decompresses on demand when
probed. This is the open question named independently by two external advisors, and it
is not blocked on anything below — it can run against today's upload path.

The thing to understand about this adversary is that it is not trying to beat the honest
prover, and it knows it cannot: decompressing a block is strictly more work than reading
bytes that are already resident. It is trying to stay *close enough* that the gap
disappears into the noise floor, while holding materially fewer bytes. **Its win
condition is indistinguishability, not speed.** What it buys is free memory — floor space
it can rent out while still passing the challenge.

That makes the noise floor the real opponent, and this environment's is wide: about 40%
of an honest response is TCP transport, and absolute latency swings ~1.6x between
sessions ([ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md)). The
cheat only has to hide inside that.

Why this matters more than it first looks: the memory-hard buffer below is Argon2d
output, so it is pseudorandom and compresses to nothing — against the primitive's own
filler this attack buys exactly zero. But what memchal ultimately wants to prove
residency of is **model weights**, which are structured, often quantized, and genuinely
compressible. The attack is dead where it is easiest to measure and live exactly where it
counts. So an experiment has to use a buffer with *tunable, realistic* compressibility;
run it against the generator's output and it returns a reassuring null result that means
nothing.

- [ ] **Measure the frontier.** A `compressed` cheating-prover mode (hold the buffer
  compressed, decompress the containing block per probe, with a small decompressed-block
  cache) plus a verifier knob for buffer compressibility. *Done when:* there is a
  measured curve of **memory saved** against **timing separation from honest**, swept
  over compressibility and cache size, with the indistinguishable region stated plainly
  in ratio terms with propagated error.

The deliverable is **a measured frontier, not a pass/fail.** "Compression is defeated" is
not an available answer. "A prover can hold X% fewer bytes while staying inside the noise
floor" is.

Untested hypotheses for what would move that frontier, to be checked rather than
assumed: raising `k` (decompression cost scales with probes, transport does not),
attacking the transport floor itself (it is the largest noise term), and timing
individual probes rather than whole challenges.

## Then — proving residency of the real workload

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

The generator runs on the wire. `--gen argon2d` sends a 92-byte recipe instead of the
buffer, both sides build it, and the invariant suite covers the path. What remains is
attacking it.

- [ ] **Attack it: a `partial-recompute` mode.** Hold fraction `f` of the blocks and
  genuinely rebuild missing ancestors on demand, with a give-up cap. *Done when:* it
  loses on timing against the defended generator, an invariant pins that, and its TMTO
  curve fixes the cost parameters (`m`, `t`, `p`) that ADR 0007 needs to move from
  proposed to accepted.
- [ ] **Then re-check the cheap wins.** Our cheat keeps a random fraction; a real one
  studies the dependency graph offline and keeps the blocks costliest to rebuild. Add a
  `--keep hot` policy once the random curve exists, and report whether it moves the
  parameters.

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
