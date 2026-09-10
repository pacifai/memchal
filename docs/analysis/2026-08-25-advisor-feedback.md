> **Dated analysis.** Written 2026-08-25 from the external advisor's messages.
> Re-scoped for one provider by `2026-09-10-runpod-setup.md`, which finds that the
> RDMA half of the campaign below (A1's RDMA rows, A2b) probably cannot run on
> RunPod. §7 is the reply plan; §8 is a list of edits to other docs that has only
> partly been carried out. For current pending work read `memchal/NEXT_STEPS.md`.

# The external advisor's feedback, 2026-08-25 — decomposition and response plan

Claim tags follow the project convention: **[MEASURED]** memchal code,
**[LIT]** third-party claim, **[ASSESS]** reasoning in this document,
**[HYPOTHESIS]** untested assumption.

---

## 1. What they said, itemized

| # | Claim or ask | What it is |
|---|---|---|
| C1 | Two distinct deceptions: the **location lie** (verifier thinks block *i* is on device A, it's on device B) and the **representation lie** (verifier thinks block *i* is raw, it's compressed) | Framing. Adopt it. |
| C2 | The location lie matters less than you've assumed. Which GPU on a rack holds the block is unimportant; CPU vs GPU is what matters | Opinion, and the load-bearing one |
| C3 | CPU vs GPU may be inferable indirectly: if the prover answers challenges on random activation blocks, and the activations evolve at a plausible rate, the rest of the state was plausibly in HBM too | [HYPOTHESIS], theirs |
| C4 | So bandwidth saturation may not be the crux | Follows from C2 + C3 |
| C5 | The representation lie is rough. DFloat11 frees maybe 40%, possibly more if the model was picked to be extra-compressible | [LIT], slightly overstated — see §4 |
| C6 | Mitigation: maximally fast round trips. Have the verifier pull memory blocks straight out over RDMA, so the prover computes nothing and the honest response is as fast as physics allows | Proposal |
| A1 | Measure latencies between devices on the rack | Ask |
| A2 | Build concrete compression and decompression attacks, measure the latency, see whether there's any hope | Ask |
| A3 | How much HBM can you honestly fill today? Set an agent on it | Ask, separable from the rest |

---

## 2. Notice this first: they are proposing a different detector

C6 is not a tweak to the bandwidth-hard design. It's the opposite shape. [ASSESS]

| | Bandwidth-hard challenge (current port plan) | RDMA pull (their proposal) |
|---|---|---|
| Targets | Location lie | Representation lie |
| Window | ~1.25 ms | ~2-5 µs |
| Bytes moved | 10 GB, prover-local | ~4 KB, across the wire |
| Prover work | A 78-million-probe kernel | None |
| Signal | Achieved aggregate bandwidth | Per-block round-trip time |
| Output | "At least X bytes were resident over window T" | "Fraction resident >= p, at confidence c" |
| Verifier needs | A GPU, or Merkle openings | A stored copy of the expected bytes |

You can run both. You can't collapse them into one challenge: one is a
milliseconds-long, compute-bound, coverage-forcing kernel, the other is a
microseconds-long hardware read with no compute in it at all.

The important part is that the second one is only sound if the timing window
comes from attested physical bounds rather than from the prover's own observed
behavior — which is the external advisor's own criticism of the current
detector. The RDMA pull is the strongest available answer to that criticism,
because it strips out every prover-side degree of freedom. The expected honest
time becomes computable in advance from NIC and HBM specs instead of calibrated
from the machine under test. Say this back to them: it's the real argument for
their idea, and it's stronger than "the honest response is faster."

---

## 3. On the location lie: assessing the activation-rate argument

### What's right about it

C3 attacks the frontier: memchal Model 2 proves residency of *filler*, not of the
operator's real weights. Using the workload's own state as the witness is a
direct route at that gap, and it costs no new protocol machinery.

### Three caveats to raise

1. **It's an aggregate argument, not a per-block one.** Fast, correct activation
   updates say *something* is in HBM. They don't say *which* 100 GB. A prover
   holding 60% raw and 40% compressed updates activations at full rate. The
   external advisor concedes this — it's why they treat the two problems
   separately — but it's worth stating that C3 provides no coverage whatsoever
   against C5. [ASSESS]
2. **It borrows soundness from other layers.** The inference "activations moved
   correctly, so the KV cache and weights were in HBM" holds only if the dataflow
   is the declared one. That's L2 (hardness training) and L3 (computational
   integrity), not L4. The argument is fine inside the full stack and circular
   outside it. [ASSESS]
3. **Activations are the small thing.** At long context the state is dominated by
   KV cache and weights. Inferring the residency of hundreds of gigabytes from
   the timely evolution of a few gigabytes is a long lever, and its fulcrum is
   assumption 2. [ASSESS]

### A cleaner way to get the same result

If the concern is rack-level capacity rather than per-device placement, challenge
**every device on the rack at the same time** and compare the aggregate bytes
demanded against the aggregate attested HBM. [ASSESS]

Any offload target inside the rack is itself under challenge and can't serve two
masters; any target outside the rack is bounded by rack egress, which an
Interlock-style perimeter device already accounts for. The location question then
dissolves by construction, with no inference about activation rates and no
dependence on L2. Worth putting in the reply as an alternative to C3 rather than
a replacement for it.

### The consequence they probably haven't noticed

The port plan's go/no-go is **eta**, the fraction of peak HBM bandwidth an honest
prover achieves on random gathers, with a break-even at eta ~= 0.11 against an
NVLink peer at 8.9x.

If you accept C2 and drop the NVLink-peer adversary, the adversary you're left
with is host DRAM over PCIe at roughly 125x. Break-even eta falls to about 0.008.
**eta stops being a project risk.** That's a large, free reduction in scope, and
it's the strongest reason to take C2 seriously rather than defend the current
design. Flag it to them as a consequence of their own point.

---

## 4. On the representation lie: posing it properly

### Correct the number first

DFloat11 compresses BFloat16 weights to 67.9-71.8% of original size — it frees
about **30%**, not 40%, and the outputs are bit-identical. [LIT] The external
advisor's hedge about an extra-compressible model is the part that matters, and
it cuts against them in an interesting way: **hardness training exists precisely
to make the weights high-entropy.** If L2 works, DFloat11's gain on the weights
goes to roughly zero, and the weight half of the representation lie is L2's
problem rather than L4's. Don't build a defense at L4 that duplicates L2.
[ASSESS]

The half that survives is the **KV cache**, which the workload generates and
which you can't make incompressible by construction — and at long context the KV
cache is most of the memory. Point the attack work there. [ASSESS]

### The structural fact that decides this

Entropy coding is not randomly accessible. To recover block *i* you must decode
from the start of its enclosing compression unit. DFloat11 decompresses at
transformer-block granularity for exactly this reason: per-matrix decompression
underutilizes the GPU. [LIT]

So the attack has a granularity, and granularity is a dial with a cost:

- Large compression unit *U*: good ratio *r(U)*, slow random access.
- Small *U*: fast random access, ratio collapses toward 1 as per-unit dictionary
  and lookup-table overhead dominates.

That gives you the experiment as a curve rather than a yes/no. Measure `r(U)` and
`t_decompress(U)`. Then ask whether any *U* exists where the ratio is still worth
having and the added latency sits under your jitter. If no such *U* exists, the
attack loses on its own terms and you can say so with a plot. This is the honest
version of A2 and it's a single-GPU, sub-$10 experiment. [ASSESS]

### Name the strongest adversary, not the naive one

The external advisor's sketch — "send the challenged block to some processor for
decompression" — is the weak form. Build these instead, in order:

1. **Persistent kernel decompressor.** A kernel spinning on a mailbox, so the
   ~3-10 µs kernel-launch floor never appears. This is the adversary that makes
   the experiment meaningful; a launch-per-challenge implementation would produce
   a flattering result you can't defend.
2. **Decompressed hot cache.** Compression frees ~30%, so spend it caching
   recently decompressed blocks. Against repeated challenges this wins outright.
   Against fresh uniform-random challenges it converts a timing test into a
   Bernoulli one — some fraction of responses fast, the rest slow.
3. **Partial compression.** Compress only a fraction *p* of memory. This sets the
   detector's resolution.

Adversary 2 is where their instinct about fast round trips pays off, and for a
reason they didn't give: **fast round trips matter less because one of them
discriminates and more because they make the statistical test cheap.** Detecting a
compressed fraction *p* needs on the order of 1/p^2 challenges. At microsecond
round trips you can afford 10^5-10^6 per second, so *p* = 1% is minutes of wall
clock and about 40 MB of network. Localizing *which* region is compressed is the
expensive version; detecting the global fraction is nearly free. Reframe C6 this
way in the reply. [ASSESS]

### The new attack surface their proposal opens

The RDMA pull only removes prover-side computation if the read really is served
by NIC hardware. Two ways the prover gets back into the path:

- **On-demand paging.** An RDMA memory region with ODP faults into software on
  access. That's `cudaMallocManaged` moved down to the NIC — the same adversary
  as #1 in the port plan, in a new place. Require pre-registered pinned regions
  and treat a fault-capable region as a failed audit.
- **Emulated RDMA.** A software target that speaks the protocol. Then everything
  is interceptable and the timing means nothing.

Both mean **their proposal moves the trust anchor from the GPU to the NIC.** That's
progress, not a flaw — a NIC is a far smaller thing to attest than a GPU — but it
has to be said out loud, because the protocol's soundness now rests on it.

### The synthesis worth sending them

Interlock is already a trusted in-line FPGA on the node boundary, built at FLI,
running on silicon and independently audited. **Make the Interlock device the
thing that does the pulling.** You get a hardware timestamp instead of a
verifier-measured round-trip time, a device the prover doesn't control, and a
natural home for the attested clock the current detector lacks. It also converts
the open question about Interlock's 1 ms bucketing from a conflict into a design
parameter, because a device doing its own timestamping doesn't care what the
external observer can resolve.

There's a second free win. Under hardness training the weights are a **declared,
public** model, so the verifier can hold a Merkle tree over the expected bytes and
check a pulled block directly. No filler, no verifier-held secret seed, no
prover-side PRF. That retires open issue #4 (ChaCha20 is counter-mode, so the
capacity defense is really seed secrecy) and open issue #5 (predictable nonces) by
making both irrelevant, and it dissolves the Model 1 / Model 2 dilemma: you ship
only the sampled blocks, not N bytes per check.

---

## 5. Their three asks, as experiments

| Ask | Experiment | Rig | Rough cost | What it settles |
|---|---|---|---|---|
| A1 | Per-tier latency and bandwidth: HBM, NVLink peer, host DRAM over PCIe, NVMe. Add RDMA read latency to GPU memory, pinned vs ODP | 1 node with >=2 NVLink GPUs and an RDMA NIC; `nvbandwidth` plus `ib_read_lat` with GPUDirect | $20-60 | The reachable-set rule, and the honest floor for the RDMA pull |
| A2 | `r(U)` vs `t_decompress(U)` frontier. Start with nvCOMP at tunable chunk size for the cheap sweep, then DFloat11 for the realistic point. Persistent-kernel decompressor, not launch-per-challenge | 1 GPU | <$10 | Whether any workable compression unit hides under the jitter |
| A2b | ODP fault latency vs pre-registered pinned RDMA read | Same node as A1 | included | Whether "the prover must fault into software" is a 10x signal or a 1.2x one |
| A3 | See §6 | 1 GPU | <$5 | The denominator for the resource-exhaustion argument |

A1 and A2b are the same afternoon on the same rented node. Do them together.

Two procurement traps carried over from the port plan: marketplace multi-GPU
instances are often PCIe rather than NVLink (check `nvidia-smi topo -m` for `NV#`,
not `PIX` or `SYS`), and virtualization layers often block peer access (check
`cudaDeviceCanAccessPeer`). Add a third for the RDMA work: many cheap instances
have no RDMA NIC at all, and GPUDirect RDMA needs the NIC and GPU under the same
PCIe root complex.

---

## 6. On the HBM-fill question

They ask how full you can honestly get: 95%, 99%, 99.99999%. Reframe it before
answering, because the number on its own doesn't mean anything. [ASSESS]

**The question that matters is the complement: how empty is dangerous?** The
resource-exhaustion argument needs occupied / total ~= 1 only to the precision at
which the leftover can host covert training. On a 180 GB card, 5% leftover is
9 GB — enough for a meaningful training run. 0.1% is 180 MB — enough for almost
nothing at that model scale. So the required fill is set by the smallest covert
footprint you care about, and that's a threat-model decision, not a systems
measurement. Ask them for that threshold; it's their half of the stack.

What's achievable today, as background: [LIT], [ASSESS]

- Serving stacks default conservatively — vLLM ships `gpu_memory_utilization` at
  0.9 and operators push it to 0.95-0.98 before allocation failures start.
- The unavoidable floor is CUDA context, allocator granularity, cuBLAS and NCCL
  workspaces, and activation scratch. Hundreds of megabytes, mostly fixed.
- Getting past ~99% means static preallocation of every buffer and no dynamic
  workspace — a purpose-built inference process, which is exactly what they're asking
  to have designed.

If you do hand this to an agent, scope it as: **maximize the fraction of device
memory that is both allocated and challengeable, under a fixed model and context
length, on one GPU, without vendor firmware cooperation. Report the residual
breakdown by cause, and the marginal cost in tokens per second of each percentage
point.** The residual breakdown is the deliverable — a bare percentage answers
nothing.

---

## 7. What to say back to them

Ordered by value.

1. **Accept C1.** The two-problem split is a better frame than the one in the
   current docs, and the second problem is genuinely the harder one.
2. **Take C2 and give them the consequence.** Dropping the NVLink-peer adversary
   takes the eta break-even from 0.11 to about 0.008 and retires the port plan's
   main risk. Offer the simultaneous rack-wide challenge as a cleaner route to the
   same place than the activation-rate inference, and note that C3 leans on L2 and
   L3 for its soundness.
3. **Split the compression attack by target.** Weights are L2's job and hardness
   training is supposed to kill DFloat11's gain outright. The KV cache is the real
   exposure, and it's the bulk of the memory at long context.
4. **Give the right argument for C6.** The reason to make round trips fast isn't
   that one of them discriminates — it's that they buy statistical power against
   small compressed fractions, and that they remove the prover's control over the
   timing window, which is their own criticism of the current detector.
5. **Name the NIC.** Their proposal moves the trust anchor from GPU to NIC, and
   Interlock is already a trusted in-line device sitting in roughly the right
   place. Proposing that Interlock does the pulling is a concrete collaboration
   opening with the Interlock authors and answers the bucket-period question at
   the same time.
6. **Commit to A1 + A2 + A2b as one campaign**, with the `r(U)` vs latency
   frontier as the deliverable, and hold the epistemic line: nothing about GPUs is
   [MEASURED] here yet.

---

## 8. What this changes in the existing docs

Status of each edit as of 2026-09-10 in brackets.

- `2026-08-16-gpu-port-plan.md` §3: the eta go/no-go is contingent on which
  adversary survives C2. Don't delete it — the bandwidth-hard challenge is still
  the only instrument that produces an aggregate byte bound — but stop describing
  eta as the project's single decision point. **[done — amendment note added]**
- `2026-08-16-gpu-port-plan.md` §5: add the ODP memory region as the RDMA-layer
  twin of adversary #1, and add the persistent-kernel decompressor as a new
  adversary. **[done — amendment note added]**
- The advisor review: the bandwidth-vs-latency conclusion needs a caveat. Latency
  is the worst-attested property when the adversary wants to look *slower* than it
  is. Against the compression attack the adversary needs to look *faster*, which
  physics forbids — so a latency-shaped detector is defensible in that direction,
  provided the window is derived from specs rather than calibrated from the prover.
  **[not done — the advisor review is not yet on disk]**
- `2026-08-23-external-landscape.md`: the Interlock timing conflict has a
  resolution now — in-device timestamping — and a stronger reason to talk to the
  Interlock authors. **[done — amendment note added]**

---

## Sources

- [DFloat11: 70% Size, 100% Accuracy,
  arXiv:2504.11651](https://arxiv.org/abs/2504.11651)
- [NVIDIA nvbandwidth](https://github.com/NVIDIA/nvbandwidth)
- [Benchmarking GPUDirect RDMA on modern server
  platforms](https://developer.nvidia.com/blog/benchmarking-gpudirect-rdma-on-modern-server-platforms/)
- [Interlock design
  overview](https://github.com/JamesPetrie/interlock/blob/main/docs/interlock-overview.md)
