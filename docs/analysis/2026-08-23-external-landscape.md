> **Dated analysis.** Written 2026-08-23. Amended by
> `2026-08-25-advisor-feedback.md`: the Interlock timing conflict in the "timing
> defense conflicts" section now has a resolution — in-device timestamping — and
> a stronger reason to talk to the Interlock authors. Two open questions here
> (the Attestable model size, the Interlock bucket period) are tracked in
> `memchal/docs/open-questions.md`.

# External landscape review: Attestable, Interlock, and VerInf

This document places three external resources against the verification stack and
the GPU threat model.

Throughout, *memchal* means the memory-challenge proof of concept in this project:
a verifier sends a challenge that forces a prover to read a large buffer, then
uses the response time and correctness to decide whether the buffer really sits in
the prover's local memory.

## How to read the claim tags

- **[LIT]** — claimed by the external source. Not verified here.
- **[ASSESS]** — analysis in this document. Reasoning, not measurement.
- **[MEASURED]** — a memchal result already recorded in the project docs.

Nothing in this review was run or benchmarked.

## What each resource does

### Attestable: proofs of LLM inference at production speed

Attestable sells a zero-knowledge prover for transformer decoding. A
*zero-knowledge proof* lets one party convince another that a computation ran
correctly without revealing the inputs. Attestable proves `y = F(W, x, r)`: the
committed weights `W`, applied to input `x` with random seed `r`, produced output
`y`. The verifier learns nothing about the weights. [LIT]

Reported results on a single H100 GPU: [LIT]

| Measure | Value |
|---|---|
| Throughput | 53 tokens/s (batch 1, 16K context); 77 tokens/s (batch 4, 4K context) |
| Proof size | 4.35-7.92 MiB |
| Verification | 157-648 ms on CPU |
| Security | Hash functions only, no public-key cryptography, 100 bits, post-quantum |

Linear operations are dynamically quantized to 8-bit integers. Non-linear
operations stay in floating point. The blog post doesn't name the underlying proof
system.

The company's framing is worth noting: it aims to "reduce an entire AI
datacenter's trusted computing base to a single device," and it states that
verification "does not depend on trusting a hardware vendor, secure enclave, or
privileged operator."

**Caution:** Pin down the model size before you cite the throughput number. The
post names the model as "Gemma 31B ... a dense medium-sized model." Gemma 3 ships
at 1B, 4B, 12B, and 27B parameters. There is no 31B version. If the model is
27B-class, then 53 tokens/s on one H100 is roughly native decode speed, which
means proving is nearly free — an extraordinary claim. If the model is Gemma 3 1B,
the overhead is about 4x, which is strong but not field-changing. The importance
of this resource turns on that one number.

### VerInf: proofs of LLM inference at model scale

VerInf is a research prover from the Future of Life Institute (FLI). It uses
hash-based commitments in the style of Ligero, needs no trusted setup, and runs
a four-round *commit-before-challenge* protocol: the prover commits to its work
before it learns which parts the verifier will check. [LIT]

VerInf doesn't prove exact reproduction. It certifies an output stream against a
committed integer model and bounds the *unexplained information*: the output bits
that the computation doesn't account for. That relaxation is what lets it target a
real model whose inference runs in floating point.

Reported full-scale run: 400B-parameter Llama 4 Maverick, 1,000 tokens, 14.3 hours
to prove on one DGX Spark, 17.7 hours to verify on 20 CPU cores, 93.6 GB proof,
0.880 unexplained bits per token. Soundness is a documented dial: the
`LIGERO_T_QUERIES` setting controls how many columns the verifier opens, 40 in the
demonstrated run and 80 for deployment. [LIT]

### Interlock: a trusted device on the network boundary

Interlock, also from FLI, isn't cryptography applied to a workload. It's
a physical device placed in line. A field-programmable gate array (FPGA) — a chip
whose logic you configure directly, with no processor or firmware in the trusted
path — sits on the only network path into a quarantined compute node. It does
three things: [LIT]

1. **Sanitizes** Ethernet frames so no header field and no sub-millisecond timing
   pattern can carry hidden information across the boundary.
2. **Commits** to every packet through a record-to-bucket-to-epoch SHA-256
   hierarchy, folded once per second into a traffic certificate authenticated with
   a hash-based message authentication code (HMAC) under a key that only the device
   and the verifier hold.
3. **Enforces** validity rules that make each packet *self-locating*: the declared
   time bucket must match the device's own counter exactly, and packet IDs must
   increase strictly per direction. Because a packet can only sit in one place in
   the log, random sampling gives an unbiased bound over all traffic.

The device releases outbound traffic through a ping-pong buffer that swaps on a
1 ms tick, so an outside observer sees only bucket-granular timing. Certificates
chain to each other, tile the timeline with no gap or overlap, echo a verifier
nonce, and resume from a persisted monotonic counter after a power cycle. The
prover can't fork, rewind, stretch, or run ahead of the verifier's clock.

Status as of 2026-07-20: running on silicon, verified byte-for-byte against a
Python golden model, independently audited. The production build uses 1 ms buckets
at about 97 Mb/s each way. The trusted computing base is about a dozen
SystemVerilog blocks, shaped deliberately as a target for formal verification.

### Interlock and VerInf are one stack

The Interlock overview states that the recomputation step is "either a
zero-knowledge proof of the inference computation or re-execution behind a second,
simpler interlock." Its milestone list includes a multi-turn inference demo with an
in-band zero-knowledge challenge, benched against a DGX Spark — the same machine
VerInf proves on.

Interlock bounds how much information crossed the boundary. VerInf explains what
that information was. Both report in the same unit: bits of unexplained
information. The pairing is deliberate.

## Where the three fit in the verification stack

| Layer | Question it answers | Who occupies it |
|---|---|---|
| L0: capacity estimation | What hardware physically exists? (the denominator) | The external advisor's capacity-estimation notes: die photos, PHY area, HBM supply chain |
| L1: boundary accounting | What crossed this node's perimeter? | **Interlock** |
| L2: workload hardness | Can the declared work be shortcut? | Hardness training |
| L3: computational integrity | Was the declared computation the one performed? | **VerInf**, **Attestable**, proof-of-useful-work matmul certificates |
| L4: occupancy and residency | How much of the machine is demonstrably in use right now? (the numerator) | **memchal** — no one else |

Cutting across those layers are three schools of verification. [ASSESS]

- **Trusted silicon.** Trusted execution environments, confidential computing, and
  vendor attestation. This school fails against an adversary who designs the chip.
- **Cryptographic.** Attestable and VerInf. Strongest guarantees about *what* was
  computed. Structurally silent about *where* it ran and about how much of the
  machine it used.
- **Physical and behavioral.** Interlock, memchal, proof-of-space, and memory-hard
  functions. Weaker per claim, but this school survives an adversarial chip
  designer, and it's the only one that can bind a claim to a specific physical
  device.

memchal sits in the third school, next to Interlock. Attestable and VerInf sit in
the second. They're orthogonal to memchal, not competitors.

## How Interlock relates to memchal

Interlock draws the perimeter. memchal partitions the interior. [ASSESS]

Interlock bounds what crosses the node's network boundary and is agnostic to
everything inside the quarantine. memchal's entire GPU threat model — a peer GPU's
memory reached over NVLink, host memory reached over PCIe, unified-memory pages
migrating silently — lives inside that quarantine, where Interlock can't see.
Conversely, memchal can't see anything that leaves the node.

### Interlock supplies the attested clock that memchal lacks

The external advisor's sharpest criticism of memchal was that a timing window calibrated
from the device's own observed latency is a window the adversary chose, and that
windows must come from physically attested bounds. memchal has no answer to a
prover that controls the clock.

Interlock's certificate properties are that answer, and they already run on
silicon: a monotonic epoch counter, a verifier nonce echo, bucket ranges that tile
the timeline with no hidden gap, and a fixed per-bucket capacity cap.

Because the canonical 64-byte packet header carries a declared time bucket that
the device matches exactly against its own counter, a challenge issued in bucket
*b* and answered in bucket *b'* yields an attested elapsed-bucket count instead of
a round-trip time measured at the verifier. That retires memchal's weakest measured
link. The [MEASURED] transport floor of about 40% of the honest response, with
roughly 12% run-to-run variance, is a network and syscall artifact. An in-line
hardware timestamp removes the whole category.

### Interlock closes off-node delegation

GPU adversary #5 in the port plan — the prover forwards the challenge to a
different GPU that genuinely holds the data — is the attack the plan concedes
timing can't catch.

If the delegate sits outside the quarantine, Interlock catches it. The forwarded
challenge and the returned answer are traffic, they get committed, and they count
against the prover's information budget.

Note the scope. This closes off-node delegation only. Delegation to an NVLink peer
inside the same node remains memchal's problem, and nothing in these three
resources solves it.

### Interlock's sampling argument is the one memchal needs

The GPU port plan proposes replacing the single 64-bit fold with a Merkle root over
2^17 per-lane hashes, then opening about 256 lanes at 0.2% of full verifier cost.

Interlock's sampling property is that argument worked out properly. Because
packets are self-locating, the prover can't steer which item answers a challenge,
so uniform sampling over capacity becomes size-weighted sampling over real bytes.

Copy the self-locating trick directly: derive each lane's index from committed
content, so the prover can't reorder or substitute lanes after learning the
challenge.

### Interlock's timing defense conflicts with memchal's detector

Interlock destroys sub-millisecond timing on purpose. Sub-bucket timing is a covert
channel, and removing it is an intended security property. That timing is exactly
what memchal's GPU detector measures. [ASSESS]

Running a memchal challenge through an Interlock at 1 ms buckets:

| Prover | True time | Observed through 1 ms buckets |
|---|---|---|
| Honest, local HBM | 1.25 ms | ~2 ms |
| NVLink peer | 11.1 ms | ~12 ms |
| 90/10 partial residency | 2.24 ms | ~3 ms |

The gross separation between honest and NVLink survives at about 6x observed. What
doesn't survive is resolution against the partial-residency adversary, which sets
the detector's usable sensitivity and feeds the decision rule. A 90/10 split
differs from honest by about 1 ms, which is one bucket. The bucket period
therefore sets the floor on how fine a partial offload memchal can detect.

Two ways out:

- Make the challenge window many buckets long. This means a larger coverage
  fraction or a larger buffer, which raises the honest prover's cost.
- Take the timing measurement inside the device, per the attested-clock mechanism
  above, and never at the outside observer.

Interlock ships a `BUCKET_MS={100,1}` build knob, so the bucket period is a joint
parameter rather than a fixed constraint.

## How the proof systems relate to memchal

### The axes are orthogonal

A zero-knowledge proof of inference is compatible with that inference running on
1% of the machine. Worse for the resource-exhaustion argument: a prover with more
hardware than it declared produces the same proof, faster. Proof of correct
computation isn't proof of resource occupancy. [ASSESS]

State the difference as quantifiers. Zero-knowledge inference is a *sufficiency*
argument for declared work. memchal is a *necessity* argument for the absence of
undeclared work.

Covert training slips through both L1 and L3. It exfiltrates nothing during the
audit window, so an information-flow bound sees nothing, and it isn't the declared
computation, so an inference proof says nothing about it. Closing that gap is L4's
reason to exist.

One structural point is worth raising with the external advisor, because it's a
rare place where memchal is strictly stronger. A hash-based proof is portable:
nothing in it binds it to a physical device. Attestable markets this as a
feature, and for its threat model it is one. But it means the cryptographic
school can't answer "which machine did this," which is the delegation problem at
the cryptographic layer. The limit is inherent, not an engineering gap. Timing-
and bandwidth-bound challenge-response is one of the few mechanisms that can
bind a claim to a physical substrate.

### What to borrow

These are mature versions of moves the port plan already commits to.

- **Sublinear verification through opened columns.** VerInf's `LIGERO_T_QUERIES`
  is structurally the same move as "open 256 of 2^17 lanes." Adopt the discipline
  too: publish soundness as a function of the number of openings.
- **Commit-before-challenge, which closes memchal open issue #5.** Predictable
  nonces let an adversary precompute answers offline. That's the grinding attack
  VerInf's four-round design prevents. Take the fix in the same wire revision as
  the 128-bit response and the Merkle root.
- **Hash-based construction, no trusted setup, stated security bits.** This is now
  the field norm: Attestable claims 100 bits with hash functions only, VerInf uses
  hash commitments and is plausibly post-quantum. memchal's current 64-bit
  non-cryptographic pseudo-random function, with a hardcoded 64-bit seed, compares
  badly regardless of whether the capacity argument holds. The fix is cheap and it
  affects credibility.
- **Report a bound, not a verdict.** Both FLI tools report unexplained
  information, which composes additively across measurements. memchal currently
  emits pass or fail plus a false-positive and false-negative rate. If it instead
  certified "at least X bytes demonstrably resident over window T," it would
  compose with L0's capacity denominator and L1's byte budget as arithmetic rather
  than as three incompatible verdicts. This is the cheapest item on this list and
  probably the most useful for collaboration.

## What no one covers: memory residency

None of Attestable, VerInf, or Interlock claims anything about memory capacity or
residency. VerInf mentions memory only as a prover cost: 78.1 GB of GPU memory,
about 20 GB peak with lazy weight streaming. Interlock is agnostic to what happens
inside the quarantine. Attestable doesn't discuss VRAM.

Combine that with the external advisor's resource table, which rates HBM
bandwidth as the best-attested physical property and memory latency as the
worst. The highest-value unclaimed territory in the stack is a residency proof
anchored to bandwidth rather than latency — which is the design shift the GPU
port plan already made. Three independent lines of reasoning now point at
bytes-per-window as the right primitive.

## Counterarguments to prepare for

1. **"If you can prove the whole computation, why sample?"** VerInf proving a 400B
   model is a stronger result than the field expected, and it invites this
   question. The answer holds — a proof of the declared work bounds nothing about
   undeclared work, and 14.3 hours to prove, 17.7 hours to verify, and 93.6 GB per
   1,000 tokens is not an always-on instrument — but prepare it rather than
   improvise it.
2. **Attestable is the number that could change the picture.** If proving really is
   near-free at 27B scale, then mandatory proving works as a weak form of hardness
   training, because the resource envelope becomes accounted for by construction.
   That wouldn't remove L4, but it would shrink it. Confirm the model size first.
3. **Interlock is further along than memchal, in the same school.** It runs on
   silicon, matches a golden model byte for byte, has been independently audited,
   and has a sketched path to formal verification. The comparison isn't unfavorable
   — memchal is a research proof of concept and Interlock is a funded FLI prototype
   — but it sets the engineering bar for what a protocol component looks like.

## What to do next

1. **Send the external advisor the L0-L4 table.** It places their two documents,
   both of the FLI repositories, and memchal in one picture, and it shows that
   L4 is the unclaimed layer.
2. **Ask the Interlock authors the bucket-period question.** "Your timing
   quantization is my signal. At what bucket period do both properties survive?"
   is a real technical question between people building complementary halves.
3. **Confirm the Attestable model size** before citing the throughput number
   anywhere.
4. **Fold three borrowings into the planned wire revision**: commit-before-challenge
   nonces, opened-lane soundness as a documented parameter, and self-locating lane
   indices. All three are free alongside the Merkle-root change and expensive
   afterward.
5. **Reframe memchal's output as a certified byte-residency bound** instead of a
   pass-or-fail verdict, so it composes with the rest of the stack.
6. **Keep the port plan's ordering.** None of this displaces measuring eta.

## Sources

- [Attestable: Proving LLMs at Scale](https://attestable.com/blog/proving-llms-scale)
- [Attestable home page](https://attestable.com/)
- [JamesPetrie/verinf](https://github.com/JamesPetrie/verinf)
- [JamesPetrie/interlock](https://github.com/JamesPetrie/interlock)
- [Interlock design
  overview](https://github.com/JamesPetrie/interlock/blob/main/docs/interlock-overview.md)
- [Flexible Hardware-Enabled Guarantees for AI Compute,
  arXiv:2506.15093](https://arxiv.org/abs/2506.15093)
- [Technical Options for Flexible Hardware-Enabled Guarantees,
  arXiv:2506.03409](https://arxiv.org/abs/2506.03409)
