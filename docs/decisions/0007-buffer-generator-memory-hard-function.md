# ADR 0007: A buffer built from a recipe uses Argon2d

**Status:** proposed. It becomes accepted once Argon2d's memory, pass, and
parallelism settings are fixed.

This record takes over from [ADR 0006](0006-chacha20-vendored-vs-dependency.md)
for buffers built from a recipe. ADR 0006 still holds for a buffer the verifier
sends.

## Context

By default, the verifier generates the whole buffer and sends every byte to the
prover. The prover never sees the key, so it can't regenerate the data, and the
`recompute` cheat fails on correctness ([ADR 0004](0004-adversary-taxonomy-and-defenses.md)).

Sending every byte doesn't scale to what memchal is meant to check, such as a
GPU's spare memory or a model's weights. The alternative is to send a short
recipe and have both sides build the buffer. But then the prover holds the
recipe. If any word can be computed directly from the recipe, as ChaCha20
allows, keeping the buffer and regenerating it on demand cost the same. The
regenerated words are correct, so correctness can't tell the difference.

A memory-hard function fixes this. It is slow to compute without large memory,
so keeping the result is the only cheap way to have it ready. memchal doesn't
use such a function's short output. It uses the large table the function fills
while it runs, and that table becomes the buffer.

## What the generator needs

Think of the buffer as a warehouse of numbered crates. The honest prover builds
the warehouse once and keeps it. A cheat rents less floor space and rebuilds a
crate when a challenge asks for it. The generator needs:

- **A steep tradeoff between memory and work.** Keeping half the crates should
  cost the cheat far more than twice the work.
- **Expensive partial rebuilds.** Keeping most crates and rebuilding the rest
  should still be slow.
- **A one-time cost.** The honest prover pays to build once. Answering a
  challenge stays cheap: a series of reads from memory. What has to be slow is
  the cheat's rebuild during a timed challenge.

These functions are built to resist parallel speedups, and a GPU builds fast
only in parallel. The parallelism setting has to give the honest builder enough
of it without making the cheat's rebuild cheap.

## The choice that matters: how the function picks earlier blocks

To build each block, a memory-hard function reads an earlier one. It picks
which one in one of two ways:

- **From the data:** the earlier block's position depends on the contents just
  computed. To rebuild a missing block, the cheat first needs the block it
  references, which may also be missing. Rebuilds cascade, so the tradeoff is
  steep.
- **From a fixed schedule:** the position follows a public pattern that doesn't
  depend on the data. The cheat can study the pattern ahead of time and keep
  the blocks that are most expensive to rebuild. Such functions are proven to
  resist less than the first kind.

Password hashing usually prefers a fixed schedule, because data-dependent reads
leak the password to anyone who watches which memory is touched. memchal has no
such secret: the prover already holds the recipe, and the prover is the one
memchal guards against. So the reason to give up the steeper tradeoff doesn't
apply here.

## Options

| Function | How it picks blocks | Memory-work tradeoff | Notes |
|---|---|---|---|
| PBKDF2, bcrypt | Uses little memory | None | Any block costs the same to rebuild. Rejected |
| scrypt | From the data | Steep | One cost setting. In libsodium. Fallback |
| **Argon2d** | **From the data** | **Steep** | Separate memory, pass, and parallelism settings. Chosen |
| Argon2i | Fixed schedule | Weaker, by proof | Pays for side-channel safety memchal doesn't need |
| Argon2id | Half and half | In between | The usual default for passwords |
| Balloon | Either | Depends on the variant | Less widely analyzed |

## Decision

Use Argon2d, with scrypt as the fallback. It has the steepest tradeoff of the
well-analyzed options, and separate settings for memory, passes, and
parallelism, which the GPU's lane count needs.

The challenge math doesn't change ([ADR 0001](0001-challenge-math-frozen-pinned-by-known-answer.md)).
It reads words from a buffer and doesn't care how the buffer was filled.

memchal implements Argon2d itself, Blake2b included, so its output matches
every other Argon2d implementation and can be checked against the published RFC
9106 vectors. A library was considered and rejected: no C or C++ Argon2
library hands back the filled table, which is the only part memchal needs. The
reference implementation erases it before freeing it, and libsodium doesn't
include Argon2d. The reference library is used as a test instead:
`tests/shared/argon2d_differential.cpp` compares the two across many settings.

Only Argon2d is included, not Argon2i or Argon2id, so nobody can choose a
weaker variant by mistake.

## Consequences

- **The handshake changes.** With `--gen argon2d`, the verifier sends a
  92-byte recipe instead of the buffer. For large buffers, building replaces a
  multi-gigabyte transfer.
- **Timing catches `recompute`, where correctness used to.** A cheat that
  rebuilds gives correct answers, slowly. That leans harder on timing, which
  [ADR 0003](0003-timing-detector-scope-and-measurement.md) shows is the less
  portable signal.
- **Other cheats behave as before.** Every prover builds the buffer once, then
  answers as it would have. `disk`, `mmapdisk`, and `partial` still move data
  out of fast memory after the build, and timing catches them. `fake` still
  fails on correctness.
- **The verifier builds the buffer too,** to check answers against its own
  copy. It pays the one-time cost, which is fine: the verifier isn't under
  test.
- **Building shows the memory was there once, not that it stayed.** Argon2d
  builds the whole table at once, so a prover without enough memory fails to
  build. A prover that frees blocks after the build is the `recompute` cheat on
  the GPU, which keeps a fraction of the blocks and rebuilds the rest.

## Evidence

One H100 session measured the GPU `recompute` cheat with a 4 GiB buffer,
262,144 reads in 16,384 lanes, and a parallelism of 16:

- **With two passes** (`--build-passes 2`), it answered wrong on every
  challenge, at every discarded fraction tested, down to 0.1%. Rebuilding a
  block needs values from the first pass, which nobody kept.
- **With one pass,** it answered correctly but slowly: 3.8 times the honest
  prover with 0.1% discarded, and 34 times with 10%.

Two passes are the default. Two limits apply: the cheat keeps a random set of
blocks, where a real one would keep the most expensive ones, and rebuilding
part of the buffer is a cheat this design creates, not the cheapest one
available.

## Still open

- **Memory and parallelism settings.** What parallelism above 1 costs in
  resistance hasn't been measured. The CPU builds lanes one after another,
  which gives the same bytes as building them in parallel.
- **Building on the GPU.** Today the buffer is built on the CPU and uploaded.
- **Tying the buffer to a model's weights,** listed in [NEXT_STEPS](../../NEXT_STEPS.md).
