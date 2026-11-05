# Decision records (ADRs)

Short records of the decisions that bear directly on the PoC's logic and claims.
Read these before changing the challenge math, the honest-baseline guarantee, or
what the timing detector is said to establish.

Names in backticks such as `disk-is-syscalls` identify the investigations behind a
claim: the maintainers' private lab notes, each with its question, script, and raw
results.

(Tooling/environment choices — containerization, JSON output, Python aggregation —
are documented in [CONTRIBUTING.md](../../CONTRIBUTING.md) and `README.md`, not here.)

| # | Decision | Why it matters |
|---|---|---|
| [0001](0001-challenge-math-frozen-pinned-by-known-answer.md) | Challenge math frozen; pinned by a known-answer test | A silent math change would invalidate every number |
| [0002](0002-refuse-unless-resident.md) | Refuse to run unless the buffer is verifiably resident (VmLck on CPU, the pointer's memory type on GPU) | No result rests on an honest baseline that ran from slow memory; timing is the only signal that survives a lying prover |
| [0003](0003-timing-detector-scope-and-measurement.md) | Timing detector: syscalls not storage; within-session only | The main claim correction — read before trusting timing |
| [0004](0004-adversary-taxonomy-and-defenses.md) | Adversary taxonomy: recompute / prefetch / precompute + defenses | The three escape hatches from proof-of-retention and their toggled defenses |
| [0005](0005-per-lane-chaining-vs-gpu-parallelism.md) | Per-lane chaining: latency-hardness vs GPU parallelism | The chain shape decision the GPU port will re-open |
| [0006](0006-chacha20-vendored-vs-dependency.md) | ChaCha20: keep the vendored implementation (send-buffer scope; revisited by 0007) | Why the project owns ~120 lines of crypto instead of taking a dependency |
| [0007](0007-buffer-generator-memory-hard-function.md) | Buffer generator: a memory-hard function for the build-buffer design (**proposed**) | Why the recompute defense needs an MHF once the prover holds the recipe, and which one |

