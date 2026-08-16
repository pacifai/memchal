# Next steps

Out of scope for the stabilization phase (which only made existing results
trustworthy), captured here with why each matters. Ordered roughly by how much
each changes what the PoC can claim.

## 1. Threat models: what "retention" actually requires

The stabilized design proves "you can't answer without *reading* the data." That is
weaker than proof-of-*retention*, because there are **three ways to answer fast
without genuinely holding the data**, and each needs its own defense. All three are
built (as `cheating_prover` modes with their defenses — see
[ADR 0004](docs/decisions/0004-adversary-taxonomy-and-defenses.md)), so the claim is
tested rather than asserted.

### The three escape hatches → three defenses

- **Recompute** — don't store the buffer; regenerate each probed word on demand.
  This works *only because our generator is cheap and invertible*: `fill_random`
  is `splitmix64`, and `buf[idx] = finalizer(seed + (idx+1)·GOLDEN)` with an
  **invertible** finalizer. So an adversary recovers the seed from a single
  received word, throws the bytes away, and computes any `buf[idx]` in O(1) with
  ~zero memory — correct *and* fast, beating **both** detectors. Defeating it needs
  **capacity-hardness**: the data must be genuinely un-regenerable.
- **Prefetch** — keep the data on disk, but because *all `k` offsets are derivable
  from the nonce the instant it arrives*, compute them up front and batch/async
  prefetch the pages (parallel I/O) instead of paying serialized per-probe faults.
  This collapses the timing gap without holding anything resident. Defeating it
  needs **latency-hardness**: the access pattern must not be knowable ahead of the
  reads. (Real only under memory pressure — see the prefetch-needs-pattern
  investigation.)
- **Precompute** — the cheapest one, and it moots the other two: because nonces are
  **predictable** (`ch.nonce = nonce_base + i`), a prover keeps the data on disk and
  precomputes each answer *offline* (a background thread, in the gap between
  challenges) and serves from cache — instant, zero residency. Chaining and ChaCha
  don't help (you precompute offline). Defeating it needs an **unpredictable nonce**.

You need all three; each defense is blind to the other attacks.

### The three defenses (built)

- **Capacity-hardness → crypto keyed-stream generation.** Generate the buffer with
  a deterministic **crypto** stream (ChaCha20 keyed by a random 256-bit key) instead
  of `splitmix64`. The seed can't be recovered from the output, so the recompute
  attack regenerates *wrong* data → the correctness detector catches it. Lives
  entirely in the **verifier** (generation is verifier-side and one-time; the
  prover only stores bytes). We vendored a small ChaCha20 (RFC 8439, KAT'd) rather
  than a heavy dependency like Botan — ~120 lines, table-free, and (critically)
  **GPU-portable**, which Botan is not.
- **Unpredictability → fresh CSPRNG nonce.** `--nonce random` draws each nonce from
  a ChaCha20 CSPRNG, so a prover can't precompute answers offline. Cheap, and it
  closes the cheapest hole outright.
- **Latency-hardness → data-dependent access.** Make each next index depend on the
  *previously-read word* (`idx_{i+1} = f(acc_i)` where `acc` folds in `w_i`), so you
  cannot enumerate the offsets — or prefetch — without actually reading probe `i`
  first (pointer-chasing). This is a change to the frozen challenge math in
  `common.hpp` and gets its own KAT vectors. GPU note: keep it **per-lane**
  sequential with many lanes in parallel, so it stays latency-hard *within* a lane
  while remaining embarrassingly parallel *across* lanes (the standard memory-hard
  shape) — otherwise it fights the GPU port.

Both are wired as **toggles** (generator: splitmix64|chacha; access: independent|
dependent) defaulting to the current behavior, so existing KAT/tests are unchanged
and the demo can flip a defense on/off and show the attack succeed then fail.

### Model 1 vs Model 2 — and why the seed is (or isn't) shared

- **Model 1 — the prover only gets the bytes** (today's protocol, and what the two
  attacks/defenses above target). The verifier ships the buffer and keeps the seed.
  Crypto generation is a **cheap, complete** fix: the seed is unrecoverable, so the
  prover must store all N bytes. *Meaning:* "prove you kept the specific data I
  sent." Binds to real data, but doesn't scale — you must upload N bytes per check.
- **Model 2 — the verifier gives the prover the seed** (proof-of-space shape). The
  rationale is **scale**: you cannot upload hundreds of GB of weights per
  verification, so you ship a ~32-byte *recipe* instead, and both sides materialize
  the dataset. The apparent paradox (handing the prover the shortcut) is resolved
  only by making the generator **memory-hard** (Argon2/scrypt-style): the seed is
  cheap to send but `f(seed)` is expensive to materialize, so a rational prover
  stores rather than regenerates. *Meaning:* "prove you materialized and hold this
  recipe" — scales, but proves you hold *filler*, not the operator's weights.

**The frontier** (what actually matches the parent goal): a Model-2, memory-hard
generator whose dataset is **bound to the real weights** (derived from the model),
so residency implies holding something tied to the actual workload — without
revealing it. That is proof-of-retrievability ∩ proof-of-space for ML, and a real
research lift, not a build task. Model-1 recompute+crypto is the near-term step;
the memory-hard, weight-bound Model 2 is the destination.

## 2. A genuinely-cold-read test on bare metal — still the settling experiment

We proved the current "disk" timing gap is pread **syscall overhead**, not
storage distance, and that an mmap adversary erases it whenever the page cache
can hold its file. `docs/investigations/mmap-needs-ram/run_mmap_needs_ram.sh` closed the obvious
follow-up: starve the same adversary (cgroup budget = half the buffer) and it goes
to ~960× honest with ~52% of probes faulting to storage. What that does **not**
establish is storage physics — the budget is a cgroup limit rather than physical
RAM, and Docker's virtual disk is cached again by the macOS host, so the measured
penalty is a **lower bound**.

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

## 3. mmap adversary above RAM size — done as a cgroup proxy, open on real RAM

The falsification test ("does `mmapdisk` still match honest when it cannot cache
the buffer?") is answered in the direction that favors the detector: no. See
`docs/investigations/mmap-needs-ram/run_mmap_needs_ram.sh` — prover in its own container at half the
buffer size, ~960× honest, ~52% of probes from storage, cross-checked against the
storage traffic the run implies (~8.3 MiB/challenge at ~205 MB/s).

What is left is the same gap as #2: a **physical** >RAM buffer on **real**
storage, which would also tell us the true per-fault cost rather than a lower
bound. Also worth adding while there: `O_DIRECT` for the pread path (note the
512 B/4 KiB alignment constraint vs the 8-byte probe), and a sweep of budget/buffer
ratios to see how the penalty scales with the fraction that fits.

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
  precomputable in a real adversarial setting). **Still open.**
- Fresh random nonce per challenge from the verifier — **done** (`--nonce random`,
  ChaCha20 CSPRNG); see the precompute escape hatch in §1 and ADR 0004. Make it the
  default for any adversarial (non-benchmark) use.
- Wire byte-order is native (assumes same-endian peers; true on loopback).
