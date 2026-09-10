# Open questions

Live threads that are neither work nor decisions you can make alone. Each one is
blocked on another person or on information you don't have yet.

This file is **living**. When a question closes, delete the entry — don't mark it
resolved and leave it here. If closing it settles a design choice, the answer
lands in an [ADR](decisions/); if it creates work, the work lands in
[`../NEXT_STEPS.md`](../NEXT_STEPS.md).

| Where things go | |
|---|---|
| Work to do | [`../NEXT_STEPS.md`](../NEXT_STEPS.md) |
| Decisions you can make and record | [`decisions/`](decisions/) |
| Measurements | [`investigations/`](investigations/) |
| Dated reasoning documents | `analysis/` |

---

## Q1 — Which proof-of-useful-work scheme does the two-challenge architecture assume?

**Owner:** the project manager. **Blocks:** whether the two-challenge
architecture is buildable at all, and therefore how much of the port plan
survives.

The two candidate families differ by orders of magnitude in cost. A VerInf-class
prover is prohibitively slow for continuous operation — the published full-scale
run is 14.3 hours to prove and 17.7 hours to verify for 1,000 tokens.
Komargodski-Weinstein matmul certificates (arXiv:2504.09971) are near-zero
overhead at 1+o(1). If it's the first, "run PoUW continuously alongside memchal"
isn't a design, it's a wish.

The external advisor's four-layer stack already puts the Komargodski-Weinstein
certificates at this layer (`analysis/2026-08-16-advisor-review.md`), so the
cheap option is at least the one an outside collaborator assumes. That is not
the same as the project manager having chosen it.

**Closes when:** the project manager names the scheme. Then either the
architecture proceeds or the memory half stands alone.

## Q2 — Option A or Option B: filler buffer, or workload state plus sampled proof?

**Owner:** the project manager, with the external advisor and the Interlock
authors as input. **Blocks:** the build-buffer work and the whole port plan's
framing.

- **Option A** is what memchal does today: the verifier's own filler buffer, made
  expensive to regenerate by a memory-hard generator (ADR 0007).
- **Option B** is the Interlock/VerInf shape: challenge the workload's own state
  and sample a zero-knowledge proof over it.

Option B dissolves the "we prove residency of filler, not of the model" gap that
is the project's stated frontier, and under hardness training it lets the verifier
hold a Merkle tree over declared public weights — which retires the seed-secrecy
problem (open issue #4) and the predictable-nonce problem (#5) by making both
irrelevant. It also costs the provider cooperation memchal has been designed to
avoid needing.

**Closes when:** the project manager picks, or the two are scoped to different
deployment settings. Until then ADR 0007 stays `proposed`.

## Q3 — Is the NVLink-peer adversary in scope? (the external advisor's C2)

**Owner:** the external advisor. **Blocks:** the break-even value of eta, and
therefore how much project risk the GPU port carries.

The external advisor argues the location lie matters less than assumed: which
GPU on a rack holds a block is unimportant, and CPU-versus-GPU is what matters.
If you accept that and drop the NVLink peer, the surviving adversary is host
DRAM over PCIe at ~125x, and break-even eta falls from 0.134 (H100) to about
0.019. **eta stops being a project risk.** That is a large, free reduction in
scope.

Two things to put to them rather than concede outright. Their alternative route —
inferring HBM residency from the rate at which activations evolve — is an
aggregate argument that says *something* is in HBM but not *which* hundred
gigabytes, and it borrows its soundness from the workload-hardness and
computational-integrity layers, so it is circular outside the full stack. A
simultaneous rack-wide challenge reaches the same place more cleanly: every device
is under challenge at once, so no in-rack target can serve two masters, and
out-of-rack targets are bounded by rack egress.

**Closes when:** the threat model names the adversary set. Then it becomes an ADR.

## Q4 — How empty is dangerous? (the external advisor's A3, reframed)

**Owner:** the external advisor. **Blocks:** the resource-exhaustion argument,
and the target for any honest-fill engineering.

They asked how full HBM can honestly get — 95%, 99%, 99.99999%. The number on its
own means nothing. The question that matters is the complement: the required fill
is set by the smallest covert footprint you care about. On a 180 GB card, 5%
leftover is 9 GB, which hosts a meaningful training run; 0.1% is 180 MB, which
hosts almost nothing at that model scale. That's a threat-model call, not a
systems measurement.

For background when answering: serving stacks default conservatively (vLLM ships
`gpu_memory_utilization` at 0.9, operators push to 0.95-0.98), the unavoidable
floor is CUDA context plus allocator granularity plus cuBLAS/NCCL workspaces plus
activation scratch, and getting past ~99% means static preallocation of every
buffer — a purpose-built inference process.

**Closes when:** the external advisor gives a threshold. The engineering
question ("maximize the fraction of device memory that is both allocated and
challengeable, report the residual breakdown by cause and the marginal
tokens/sec cost of each point") only becomes worth scoping after that.

## Q5 — At what bucket period do both properties survive? (for the Interlock authors)

**Owner:** the Interlock authors. **Blocks:** whether memchal and Interlock compose, and
it's the fastest route to a working relationship with them.

Interlock destroys sub-millisecond timing on purpose — it's a covert channel, and
removing it is an intended security property. That timing is what memchal's GPU
detector measures. At 1 ms buckets the gross honest-versus-NVLink separation
survives at ~6x observed, but resolution against partial residency does not: a
90/10 split differs from honest by about 1 ms, which is one bucket. Interlock
ships a `BUCKET_MS={100,1}` knob, so this is a joint parameter, not a fixed
constraint.

There's a resolution worth proposing at the same time: have the device do its own
timestamping. An in-line hardware timestamp gives an attested elapsed-bucket count
instead of a verifier-measured round-trip time, which retires memchal's weakest
measured link — the ~40% transport floor with ~12% run-to-run variance — and
supplies the attested clock the detector currently lacks. A device doing its own
timestamping doesn't care what an outside observer can resolve.

**Closes when:** you've asked them. This one is only blocked on sending the message.

## Q6 — What model is Attestable's throughput number for?

**Owner:** nobody — it's a fact to look up. **Blocks:** citing the number
anywhere, and how seriously to take the resource.

The post says "Gemma 31B ... a dense medium-sized model." Gemma 3 ships at 1B, 4B,
12B, and 27B. There is no 31B. If it's 27B-class, 53 tokens/s on one H100 is
roughly native decode speed and proving is nearly free — an extraordinary claim
that would make mandatory proving a weak form of hardness training and shrink L4's
territory. If it's Gemma 3 1B, the overhead is ~4x: strong, not field-changing.

**Closes when:** somebody confirms the parameter count.

## Q7 — Do the two challenges have to run at the same time, and can they?

**Owner:** the project manager, as the architecture's author. **Blocks:**
whether the joint claim means anything.

Three structural problems with the two-challenge architecture, none yet answered:
proof-of-useful-work certifies compute but not memory footprint; simultaneity is
required for the joint claim to hold, since time-slicing the two challenges is a
straightforward attack; and both halves contend for the same hardware, so running
them together degrades both.

The proposal worth putting to them: have both halves emit **byte-residency bounds
over a shared time window**, so the claims compose by arithmetic instead of by
assertion. That also fits the "report a bound, not a verdict" item in the roadmap.

**Closes when:** the architecture either adopts the shared-window framing or names
a different way the two claims compose.
