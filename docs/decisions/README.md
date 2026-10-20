# Decision records (ADRs)

Short records of the decisions that bear directly on the PoC's logic and claims.
Read these before changing the challenge math, the honest-baseline guarantee, or
what the timing detector is said to establish. Each is backed by the
[investigations](../investigations/).

(Tooling/environment choices — containerization, JSON output, Python aggregation —
are documented in [`CLAUDE.md`](../../CLAUDE.md) and `README.md`, not here.)

| # | Decision | Why it matters |
|---|---|---|
| [0001](0001-challenge-math-frozen-pinned-by-known-answer.md) | Challenge math frozen; pinned by a known-answer test | A silent math change would invalidate every number |
| [0002](0002-verify-and-refuse-on-mlock.md) | Verify the lock (VmLck); refuse to run unpinned | No result rests on an unpinned honest baseline |
| [0003](0003-timing-detector-scope-and-measurement.md) | Timing detector: syscalls not storage; within-session only | The main claim correction — read before trusting timing |
| [0004](0004-adversary-taxonomy-and-defenses.md) | Adversary taxonomy: recompute / prefetch / precompute + defenses | The three escape hatches from proof-of-retention and their toggled defenses |
| [0005](0005-per-lane-chaining-vs-gpu-parallelism.md) | Per-lane chaining: latency-hardness vs GPU parallelism | The chain shape decision the GPU port will re-open |
| [0006](0006-chacha20-vendored-vs-dependency.md) | ChaCha20: keep the vendored implementation (send-buffer scope; revisited by 0007) | Why we own ~120 lines of crypto instead of taking a dependency |
| [0007](0007-buffer-generator-memory-hard-function.md) | Buffer generator: a memory-hard function for the build-buffer design (**proposed**) | Why the recompute defense needs an MHF once the prover holds the recipe, and which one |
| [0008](0008-gpu-residency-check-substitutes-for-mlock.md) | GPU: cudaMalloc + a pointer-attribute check replaces mlock/VmLck (**proposed**) | What ADR 0002's guarantee becomes on a device with no mlock and no /proc — and why that makes timing the only signal that survives a lying prover |
| [0009](0009-challenge-the-weights-in-place.md) | Challenge the model's weights where the inference engine holds them (**proposed**) | Accounts for the memory the workload actually uses at no extra cost; what it can't show (compressed storage) and what vLLM imposes |
