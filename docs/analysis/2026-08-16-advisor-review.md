> **Dated analysis.** Written 2026-08-16, the earliest document in this folder
> and the origin of the layered stack the later ones build on. Two of its
> conclusions have since been amended: the NVLink ceiling is 8.9x, not 4.4x (the
> draft treated 1.8 TB/s as unidirectional — see
> `2026-08-16-gpu-port-plan.md` §2), and the bandwidth-over-latency conclusion
> needs the caveat in `2026-08-25-advisor-feedback.md` §8. Private names appear
> here as role labels. For current pending work read `../../NEXT_STEPS.md`.

# External advisor review, 2026-08-16 — mapping and draft answers

The external advisor is an ML/security researcher working on datacenter
compliance-verification protocols for international AI agreements. They shared
two unpublished notes and two questions.

> **Epistemic ground rules for this document:** every claim below is tagged as
> one of — **[MEASURED]** (backed by code and investigations in the memchal
> repo, all CPU-only, Docker-VM caveats apply), **[LIT]** (public third-party
> figures, including the memory-ladder table, which is teaching material and was
> never measured here), or **[HYPOTHESIS]** (what this project assumes about
> GPUs, untested). The code is the only source of truth. The memory-ladder
> figures are illustrative only — never present them to collaborators as this
> project's data.

## Their two notes

1. **A hardness-training note.** Developers demonstrate compliance by *resource
   exhaustion*: serve only a declared open-source model whose weight matrices
   have been put through "hardness training" — a public, unpredictable process
   that scrambles weights so multiplying by them is generically resource-hard,
   needing real memory capacity, bandwidth, and compute, with no compression,
   recompute, or algorithmic shortcut. Inference code is audited for shortcuts;
   occupying ~99% of memory bandwidth means no covert training fits beside it.
   Stated open problems: how much hardness is enough and how to verify it;
   hardness training without degrading model quality; preventing matrix-level
   and code-level shortcuts.
2. **A capacity-estimation note** on estimating the true capacities (compute,
   HBM capacity, HBM bandwidth) of an accelerator whose designer may be the
   adversary. The adversary's goal is to **hide** resources during an audit and
   release them later for covert training. Attack ladder: **A1** under-declare
   (firmware-disable cores or HBM PHYs — standard binning machinery); **A2**
   sandbagging via covert throttling (clocks, cache ways, traffic shaping);
   **A3** faked activity counters; **A4** clean-sheet deceptive architecture
   (dismissed as impractical for now). Defenses: **D1** static physical evidence
   — die photo and floor-plan area estimation via a trusted fabricator, ~10%
   accuracy claimed; **D2** dynamic activity counters (bytes moved, FLOPs) to
   expose throttling bottlenecks; **D3** counter attestation, least developed.

   Resource table highlights: **HBM bandwidth is the best-attested resource**
   (beachfront visible in the floor plan, PHY area nearly deterministic, few HBM
   producers so an independent trust anchor exists, and byte counters in memory
   controllers are hard to fake); **memory latency is the worst** (fake latency
   needs no area, is easy to add, and has few or weak counters — an explicit D2
   gap — and latency sandbagging is nearly free for a covert-training adversary
   because training is latency-insensitive); **inter-chip interconnect is
   nastiest** (retrofittable, reversible, traceless — link tiers swap in days;
   defenses are inventory bounding and network taps). Known false-positive
   sources: benign binning, benign runtime bandwidth variance.

## Where this sits relative to memchal

The advisor frames memchal as "the spot checks … the other half of it." The two
halves are complementary and meet exactly at the open problem the roadmap calls
the frontier:

- memchal Model 1 works but doesn't scale (ship N bytes per check); Model 2
  scales but proves residency of *filler*, not the operator's weights. The
  missing piece is a resident dataset **bound to the real workload**.
- Hardness training attacks the same gap from the other end: make the *actual
  weights* incompressible and non-regenerable, and spot-checking the weights
  themselves becomes meaningful — no filler, no verifier-held secret seed, and
  the capacity-hardness that the ChaCha20 defense only fakes via seed secrecy
  (open issue #4) is provided by the weights being generically hard.
- **Four-layer stack**, as updated after reading the capacity-estimation note:
  **capacity estimation** (hardware ground truth, the denominator) →
  **hardness training** (the workload can't be shortcut) → **proof of useful
  work** (arXiv:2504.09971, Komargodski-Weinstein matmul certificates, 1+o(1)
  overhead, so the compute performed is the claimed matmuls) → **memchal spot
  checks** (resources occupied right now — the numerator). The
  resource-exhaustion argument is "occupied / total ~= 1"; memchal proves the
  numerator, capacity estimation proves the denominator. Without the
  denominator, spot checks can saturate 99% of a *declared* capacity while
  covert work runs on hidden capacity.
- **Mirror-image adversaries, squeezing from both sides.** Their adversary hides
  resources and appears to have less; this project's adversary fakes occupancy
  and appears to hold more. Physical inspection upper-bounds what can exist,
  blocking under-declaration; challenge-response lower-bounds what is
  demonstrably present, blocking over-claimed residency. Neither method produces
  the other's bound — a sandbagged chip happily fails challenges slowly.

## Design consequences for this project

- **Independent support for the bandwidth-hardness hypothesis.** Their table
  rates HBM bandwidth as the best-anchored physical property and memory latency
  as the worst: easy to sandbag, weak counters, cheap for a covert-training
  adversary. Combined with the [HYPOTHESIS] that latency separations (HBM versus
  an NVLink peer) are too small to discriminate anyway, two independent
  arguments now say the same thing — **size challenges in bytes per window, not
  in probe latency**. Note that the current CPU timing detector is
  latency-shaped, aimed at the property their framework rates least defensible.
  And a timing window calibrated from the device's own observed latency is a
  window the adversary chose; windows must come from physically attested bounds.
- **The interconnect caveat cuts the other way.** "X/T above NVLink ingress
  forces local residency" is only sound against the *physical* interconnect
  bound, not the declared one — links are swappable in days without trace. Their
  fixes, inventory bounding and network taps, are load-bearing for this
  project's exclusion argument.
- **memchal as an active instrument for their D2.** Their defenses are passive:
  counters and bottleneck plausibility. A memchal-style bandwidth-saturating
  challenge is an adversary-robust *active* testing workload — unpredictable and
  correctness-bound. The cross-check: counters say N bytes moved, the challenge
  implies at least M, and a discrepancy is suspicion.
- **A shared calibration problem.** Their false-positive sources (binning,
  benign bandwidth variance) are the datacenter version of this project's
  [MEASURED] 1.6x session swing and ~12% floor variance. Both frameworks need
  the same missing artifact: a decision rule with stated false-positive and
  false-negative rates under benign variance. This is the most concrete
  collaboration surface.

## What they are indicating

- They are slotting memchal into their protocol architecture — a collaboration
  opening, and implicit validation that the spot-check half is needed.
- Spot checks alone are insufficient without workload hardness, which matches
  this project's own [MEASURED] findings: the recompute adversary beats both
  detectors, the ChaCha20 defense is seed secrecy rather than capacity-hardness,
  and the chained defense degrades with lanes.
- They are pushing from a single-node CPU proof of concept toward **system-level
  adversary modeling**: the adversary is the *set of devices reachable within
  the challenge window* — NVLink peers, host CPU and DRAM, a DPU, other racks —
  not one cheating process.
- "Trusted pingers" means they assume trusted measurement hardware on the
  fabric, which makes verifier placement and the transport floor first-class
  design parameters. The [MEASURED] CPU-scale preview: the transport floor is
  ~40% of an honest response at the fast end, with ~12% run-to-run variance.

## Draft answers to their questions

### Q1 — intra-server latencies, pinger placement, adversary reach

What this project can honestly offer sits in three registers.

- **[MEASURED, CPU]** The methodological results that transfer: timing tracks
  *residency*, not the storage device; apparent device gaps can be pure syscall
  overhead; the measurement environment can silently lend the adversary its own
  cache (page cache or host cache); the transport floor is large at the fast end
  (~40% of honest) with ~12% variance, so any discriminating gap must exceed
  floor variance; and session-to-session absolute latencies swing ~1.6x with
  core placement. These are lessons about *how to measure*, not GPU numbers.
- **[LIT]** Public order-of-magnitude figures for the GPU tier structure: HBM at
  a fraction of a microsecond and ~TB/s, NVLink at microseconds and ~1.8 TB/s,
  PCIe at ~µs and ~64 GB/s, host DRAM behind PCIe, NVMe at tens of µs. Cite as
  literature priors only.
- **[HYPOTHESIS]** The structural conjecture: latency separations between local
  HBM and an NVLink peer are small — maybe ~2x, within jitter for short
  challenges — while bandwidth separations are large (HBM >> NVLink >> PCIe), so
  on GPUs **bandwidth-hardness is likely the primary discriminator**. A
  challenge forced to move X bytes in window T, with X/T above NVLink ingress,
  can only be answered from local HBM. Reachable-set rule: a device is usable by
  the adversary if and only if RTT + bytes/bandwidth < window. Untested here.
  Sizing must use physically attested interconnect bounds, per the caveat above.
- **Proposed experiment**, which is the real answer to the question: a GPU
  microbenchmark suite in the style of `../investigations/` — per-tier
  pointer-chase latency, per-tier bandwidth, NVLink peer-read latency and
  bandwidth from inside a kernel, and host-DRAM-over-PCIe reads (pinned and
  zero-copy), all with the same falsification discipline as the CPU work.

Pinger-placement guidance defensible now: the pinger's RTT *jitter* must be
small relative to the honest-versus-cheat separation. That is [MEASURED] on CPU
as the floor-variance problem; placement specifics on a GPU server are
[HYPOTHESIS].

### Q2 — candidate challenge-response functions

- **[MEASURED, CPU]** The candidate: nonce-derived pseudo-random probes over the
  whole buffer folded into a chained hash (`src/common.hpp`), with toggles for
  generator (splitmix64 or ChaCha20) and access pattern (independent or
  dependent). Correctness catches fabrication at 1-(1-m)^k. Attack results:
  recompute beats both detectors against splitmix64; ChaCha20 helps only via
  seed secrecy, and the seed is 64-bit with a hardcoded default, which is a
  hole; the implemented prefetch adversary is confounded by its own overhead, so
  "chaining wins" is **not** established; chained-defense strength degrades as
  k/lanes and is void at lanes >= k; only about half the per-probe cost is
  memory, since the hash ALU is the rest, so "memory-hard" and "fast" trade off
  against each other; chained access costs the honest prover ~3.5x; and the
  current PRF is 64-bit non-cryptographic with predictable nonces.
- **[HYPOTHESIS, GPU]** Port shape: the buffer in VRAM via `cudaMalloc` (assumed
  to be the non-swappable analog of `mlock` — untested), `answer_lane_t` as the
  per-thread kernel body, `__umul64hi` for `map_index`. Conjectured GPU-shaped
  function: many parallel lanes, to use GPU bandwidth, of short per-lane
  dependent chains, to block prefetch, sized so aggregate bytes per window
  exceeds interconnect ingress. This *would* resolve the lanes-versus-chain-depth
  tension if the bandwidth-discriminator hypothesis holds. None of it measured.
- **[LIT]** Comparison points: proof-of-space graph labeling for capacity,
  Argon2 and scrypt-style memory-hard functions for Model-2 materialization, and
  pointer-chase microbenchmarks for latency.

## Follow-ups proposed

1. Reply in the three-register format — measured, literature, hypothesis, plus
   an experiment plan. This project's credibility is its falsification
   discipline, so lead with it.
2. The GPU microbenchmark investigation suite is the natural first GPU
   milestone: it answers Q1 and de-risks the port at once.
3. Explore the active/passive cross-check idea (memchal challenges against
   activity counters) and the shared decision-rule and calibration problem as
   the concrete collaboration surface.
