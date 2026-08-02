# Next steps

Out of scope for the stabilization phase (which only made existing results
trustworthy), captured here with why each matters. Ordered roughly by how much
each changes what the PoC can claim.

## 1. The recompute adversary — the most important gap

A prover that does **not** store the buffer but **regenerates** each probed word
on demand from the PRNG seed (`fill_random` is deterministic: `buf[i] =
splitmix64(seed_state_i)`). It holds *no* data, yet every answer is correct
(beats the correctness detector) and the work is pure ALU with a tiny footprint
(beats the timing detector — likely *faster* than honest, like `fake` but
correct).

Why it matters: it is the natural strongest attack on a proof-of-*retention*
primitive and it defeats **both** current detectors at once. Until it's tested,
we cannot claim the challenge forces data to be *stored* rather than *recomputed*.
The intended defense is a **memory-hard** derivation (make regeneration cost more
memory/time than storage), which is a design change, not a stabilization. This is
the first thing to build next.

## 2. A genuinely-cold-read test on bare metal — now load-bearing, not optional

We proved the current "disk" timing gap is pread **syscall overhead**, not
storage distance, and that an mmap adversary erases it for a fits-in-RAM buffer.
So the timing detector is currently **unvalidated**: we have no measurement where
timing reflects real storage physics.

What would settle it: run on **real Linux hardware** (not the Docker VM, whose
storage is virtualized through the host page cache) with **either**:
- a buffer **larger than RAM**, so most probes force real page faults / device
  reads and the mmap trick stops working; **and/or**
- `O_DIRECT` reads (bypass the page cache; note 512 B/4 KB alignment
  constraints vs the 8-byte probe).

This is the single experiment that would tell us whether the timing detector has
*any* teeth against a competent adversary. Deferred from stabilization because
building it in the VM would produce another untrustworthy number; it belongs on
bare metal. `run.sh` already works on native Linux — this is close to
one-command once the >RAM buffer / O_DIRECT mode exists.

## 3. mmap adversary above RAM size

Cheaper follow-up to #2, and a direct falsification test: measure whether
`mmapdisk` still matches honest when the buffer exceeds RAM. If it slows down
(real faults), the timing detector has a viable regime (large buffers); if not,
timing is defeated regardless of size. The `mmapdisk` mode already exists
(`src/cheating_prover.cpp`); this just needs the >RAM buffer and real storage.

## 4. A decision rule with stated FP/FN rates

Today the harness reports *separations*, not a *classifier*. A real proof needs a
threshold/decision procedure that, given a challenge response (time + correctness)
and a claimed configuration, outputs accept/reject with measured false-positive
and false-negative rates. This depends on #1–#3 (the adversary set and the real
timing regime) being settled first — otherwise we'd calibrate a detector against
adversaries we already know it can't catch.

## 5. Bare-metal / affinity rerun for trustworthy absolute numbers

The Docker-on-Apple-Silicon environment cannot measure two things the design
cares about:
- **Storage physics** (virtualized disk through host page cache) — see #2.
- **Core-affinity / tail effects.** Absolute latency swings ~1.6× between sessions
  from P-core/E-core placement, which macOS cannot pin. A native Linux run with
  `taskset` (pinned affinity, isolated core) would give portable absolute numbers
  and clean tails, and let us report percentiles like p99.9 with a sample size
  that supports them.

## 6. CUDA / GPU port

The whole point of the parent project. The answer path is already GPU-shaped
(flat buffer, no STL/exceptions, `answer_lane_t` = per-thread kernel body). Port
`map_index`'s 128-bit multiply to `__umul64hi`, put the buffer in VRAM
(`cudaMalloc`, the non-swappable analog of `mlock`), stage uploads through pinned
host memory. Everything above (adversaries, cold-read validation, decision rule)
should be settled on CPU first.

## 7. Protocol hardening (before any production use)

- Responses ≥128-bit with a cryptographic PRF/hash (64-bit is guessable/
  precomputable in a real adversarial setting).
- Fresh random nonce per challenge from the verifier.
- Wire byte-order is native (assumes same-endian peers; true on loopback).
