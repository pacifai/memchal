# ADR 0001: The challenge math is frozen and pinned by known-answer tests

**Status:** accepted

## Context

The verifier, the honest provers, and the cheating provers all compute answers
with the same functions in `src/shared/challenge.hpp`. A correct answer is
therefore the same bytes no matter where the prover kept the data. That shared
definition is what lets the verifier compare answers at all.

If that math changed, every earlier measurement would describe a different
challenge, and nothing would fail to say so.

## Decision

Treat the challenge math in `challenge.hpp`, and the Argon2d generator in
`argon2d.hpp`, as frozen. Known-answer tests pin them: fixed inputs must give
fixed outputs.

- `tests/shared/challenge_kat.cpp` pins the answers for both ways of combining
  chains, sequential and tree.
- `tests/shared/argon2d_kat.cpp` pins Argon2d to the published RFC vectors.
- `tests/gpu/answer_kat.cpp` checks that the GPU computes the same answers as
  the CPU. It runs on a real card before any GPU measurement.

The math is plain 64-bit integer arithmetic with no dependence on byte order,
so the expected values don't depend on the machine. They match on macOS and
Linux on arm64, and on the GPU.

## Consequences

- Any change to the math fails a test immediately.
- An intended change needs new test vectors, and its pull request has to say
  that results from before the change no longer compare with results after it.
