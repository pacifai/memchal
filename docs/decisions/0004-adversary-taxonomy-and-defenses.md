# ADR 0004: Three ways to answer without holding the data, and their defenses

**Status:** accepted

## Context

Correctness and timing together show that the prover read the data quickly
when the challenge arrived. That's weaker than showing the prover keeps the
data. A prover can answer correctly and fast in other ways, and each one needs
its own defense.

memchal builds each such cheat as a mode of the cheating prover, and each
defense as a verifier setting, so tests can show the cheat winning with the
defense off and losing with it on.

This record covers the design where the verifier sends the buffer to the
prover. [ADR 0007](0007-buffer-generator-memory-hard-function.md) covers the
design where both sides build the buffer from a recipe.

## The cheats and their defenses

| Cheat (`--mode`) | How it answers without keeping the data | Defense (verifier setting) | Why the defense works |
|---|---|---|---|
| `recompute` | Recovers the seed of the generator that filled the buffer from one word, since `splitmix64` can be reversed, then regenerates any word on demand. Holds nothing. | `--gen chacha` | The verifier fills the buffer with ChaCha20 under a random 256-bit key. The key can't be recovered from the data, so regenerated words are wrong, and correctness catches them. |
| `prefetch` | Keeps the data on disk. Works out every place a challenge will read before reading any, then asks the operating system to load them all at once. | `--chain dependent` | Each read's place depends on the word the previous read returned, so the places can't be known in advance. See [ADR 0005](0005-per-lane-chaining-vs-gpu-parallelism.md). |
| `precompute` | Guesses the next nonce, since sequential nonces are predictable, and works out its answer from disk between challenges. Answers instantly. | `--nonce random` | Each nonce comes from a ChaCha20-based random generator, so there's nothing to compute ahead. |

None of these affects the correctness check: the `fake` prover, which makes up
the data it lacks, fails with every defense on or off.

## Decision

All three defenses are on by default. A plain `build/verifier` uses
`--gen chacha --chain dependent --nonce random`. To measure the weak settings,
name them: `--gen splitmix64 --chain independent --nonce sequential`. The
known-answer tests and the trial harness always pass their settings
explicitly, so neither depends on the defaults.

A defense that is on isn't always effective. With `--chain dependent`, each
chain needs enough reads to matter, so the verifier refuses challenges with
fewer than 16 reads per chain.

## Consequences

- **`recompute`** is cheap and complete against `splitmix64`: it answered at
  about 0.8 times the honest prover's time while holding no data. ChaCha20 with
  a random key stops it, but ChaCha20 can produce any block directly, so it
  can't defend a buffer built from a recipe. ADR 0007 covers that case.
- **`prefetch`** only gains anything when RAM is short. With the file in the
  page cache, its extra system calls make it slower than a plain memory-mapped
  read. Under a memory limit it was about 4 times faster than a plain reader,
  and dependent chains remove that edge. Dependent chains also cost the honest
  prover about 3 to 4 times in throughput.
- **`precompute`** needs time between challenges. `--gap-us` adds random pauses
  to model a verifier that checks now and then. Random nonces close the attack
  completely at almost no cost.
- `scripts/run_trials.py` can measure every cheat with each defense on or off.
  `tests/run_tests.sh` checks that `recompute` and `precompute` win with their
  defense off and lose with it on. `prefetch` changes only timing, which the
  tests don't judge.
