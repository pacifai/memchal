# Decision records

Each record explains one design decision: the problem, what was chosen, and
what it costs. Read the relevant record before you change the challenge math,
the honest prover's memory check, or a claim about what timing shows.

A record is kept as evidence. When a decision changes, a new record replaces
it, and the old one says so at the top.

| # | Decision | Why it matters |
|---|---|---|
| [0001](0001-challenge-math-frozen-pinned-by-known-answer.md) | The challenge math is frozen and pinned by known-answer tests | A silent change would make every earlier measurement meaningless |
| [0002](0002-refuse-unless-resident.md) | The honest prover refuses to run unless its buffer is in fast memory | Every result is compared against an honest baseline, so that baseline has to be real |
| [0003](0003-timing-detector-scope-and-measurement.md) | Timing shows whether data is in fast memory now, and is compared within one session only | It sets what a slow answer proves and how to measure it |
| [0004](0004-adversary-taxonomy-and-defenses.md) | Three ways to answer without holding the data, each with a defense that is on by default | It lists the cheats a correct, fast answer can still hide |
| [0005](0005-per-lane-chaining-vs-gpu-parallelism.md) | Each challenge is many short chains of dependent reads | It balances a cheat's prefetching against a GPU's parallelism |
| [0006](0006-chacha20-vendored-vs-dependency.md) | ChaCha20 is a small file in the repo, not a library | It keeps the core free of dependencies |
| [0007](0007-buffer-generator-memory-hard-function.md) | A buffer built from a recipe uses Argon2d (proposed) | Rebuilding part of the buffer has to be slower than keeping it |
