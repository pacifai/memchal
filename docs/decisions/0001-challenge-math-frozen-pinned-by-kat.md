# ADR 0001 — The challenge math is frozen and pinned by a known-answer test

**Status:** accepted

## Context
`common.hpp`'s `answer_lane_t` / `answer_challenge_t` (and `splitmix64`, `mix64`,
`map_index`, `fill_random`) are the single source of truth for the challenge:
prover, cheating prover, and verifier all call the exact same code, so a *correct*
answer is byte-identical regardless of where the bytes came from. That property is
what makes the design sound; a silent change to it would invalidate every published
number without any test failing.

## Decision
Treat the challenge math in `common.hpp` as **frozen** for stabilization — no
changes to those functions. Pin them with a **known-answer test** (`tests/kat.cpp`)
that asserts fixed `(seed, nonce, k, lanes)` vectors produce fixed hashes. The
values are pure 64-bit integer arithmetic (no memory byte-order dependence), so
they are architecture-independent (verified identical on macOS/arm64 and
Linux/arm64). Invariant tests assert `honest`/`disk`/`mmapdisk` never fail
verification and `fake` always does.

## Consequences
- Any change to the challenge math is caught immediately (KAT fails → "the math
  changed, prior numbers are invalid").
- Changing the math (e.g. a memory-hard derivation to beat the recompute adversary)
  is deliberately out of stabilization scope and would require new KAT vectors —
  see NEXT_STEPS.md.
- Run `./run.sh bash tests/run_tests.sh`.
