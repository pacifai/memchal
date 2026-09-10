# Roadmap

Pending work, grouped by phase and ordered by what unblocks what. Each item is
actionable and carries a "done when" line so it's clear when to check it off.

Three kinds of thing live in different places:

- **Work to do** — this file.
- **Decisions to agree** — an [ADR](docs/decisions/) with `Status: proposed`,
  promoted to `accepted` once agreed.
- **Analyses** — the [investigations](docs/investigations/).

## Next — the compression frontier

A cheating prover that stores its buffer **compressed** and decompresses on demand when
probed. This is the open question named independently by two external advisors, and it
is not blocked on anything below — it can run against today's upload path.

The thing to understand about this adversary is that it is not trying to beat the honest
prover, and it knows it cannot: decompressing a block is strictly more work than reading
bytes that are already resident. It is trying to stay *close enough* that the gap
disappears into the noise floor, while holding materially fewer bytes. **Its win
condition is indistinguishability, not speed.** What it buys is free memory — floor space
it can rent out while still passing the challenge.

That makes the noise floor the real opponent, and this environment's is wide: about 40%
of an honest response is TCP transport, and absolute latency swings ~1.6x between
sessions ([ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md)). The
cheat only has to hide inside that.

Why this matters more than it first looks: the memory-hard buffer below is Argon2d
output, so it is pseudorandom and compresses to nothing — against the primitive's own
filler this attack buys exactly zero. But what memchal ultimately wants to prove
residency of is **model weights**, which are structured, often quantized, and genuinely
compressible. The attack is dead where it is easiest to measure and live exactly where it
counts. So an experiment has to use a buffer with *tunable, realistic* compressibility;
run it against the generator's output and it returns a reassuring null result that means
nothing.

- [ ] **Measure the frontier.** A `compressed` cheating-prover mode (hold the buffer
  compressed, decompress the containing block per probe, with a small decompressed-block
  cache) plus a verifier knob for buffer compressibility. *Done when:* there is a
  measured curve of **memory saved** against **timing separation from honest**, swept
  over compressibility and cache size, with the indistinguishable region stated plainly
  in ratio terms with propagated error.

The deliverable is **a measured frontier, not a pass/fail.** "Compression is defeated" is
not an available answer. "A prover can hold X% fewer bytes while staying inside the noise
floor" is.

Untested hypotheses for what would move that frontier, to be checked rather than
assumed: raising `k` (decompression cost scales with probes, transport does not),
attacking the transport floor itself (it is the largest noise term), and timing
individual probes rather than whole challenges.

## Then — proving residency of the real workload

Today the challenge proves you're holding a specific random buffer the verifier sent.
The goal is to prove you're holding something tied to the actual workload — not
generic filler — while keeping friction with model providers low.
[ADR 0004](docs/decisions/0004-adversary-taxonomy-and-defenses.md) explains why the
send-buffer design doesn't get there.

The plan is a **memory-hard, weight-bound generator** (the build-buffer design): ship
a short recipe, and both sides build a dataset from it that is expensive to regenerate
on demand (so the prover must store it) and tied to the model. The memory-hardness
falls on the one-time *generation*, not the per-challenge answer, so it does **not**
multiply the per-challenge cost — that cost is inescapable and accepted either way.

The generator itself is built: `src/argon2d.hpp` is vendored Argon2d (RFC 9106), pinned
against RFC vectors by `tests/known_answer_argon2d.cpp`, and `bench_build` reports what a
block costs. `mc::build_buffer()` hands back the Argon2d matrix as the challenge buffer.
What remains is wiring it in and attacking it.

- [ ] **Ship the recipe handshake.** Replace the buffer upload with a small `Recipe`
  message (key, salt, `m`/`t`/`p`, reserved workload tag) behind a wire-version bump, and
  add build paths to the prover (build inside the already-`mlock`ed region) and the
  verifier (build its own reference copy). Keep the send-buffer path working — every
  existing investigation, result, and the demo ride on it. *Done when:* a build-buffer
  challenge runs end to end against the honest prover with zero verify errors, and the
  existing invariant suite still passes unchanged.
- [ ] **Attack it: a `partial-recompute` mode.** Hold fraction `f` of the blocks and
  genuinely rebuild missing ancestors on demand, with a give-up cap. *Done when:* it
  loses on timing against the defended generator, an invariant pins that, and its TMTO
  curve fixes the cost parameters (`m`, `t`, `p`) that ADR 0007 needs to move from
  proposed to accepted.
- [ ] **Then re-check the cheap wins.** Our cheat keeps a random fraction; a real one
  studies the dependency graph offline and keeps the blocks costliest to rebuild. Add a
  `--keep hot` policy once the random curve exists, and report whether it moves the
  parameters.

> Not in scope, noted for the future: **challenging the weights directly** (probe a
> function of the weights the prover already holds for inference) would avoid a
> separate buffer entirely, but it needs the provider to expose or cooperate on the
> weights — the kind of friction we're trying to avoid. Revisit only if that
> constraint changes.

## Later — GPU port and decision rule

Timing is a core signal (it's what proves the data is in *fast* memory, not just
stored somewhere), but the CPU-in-Docker environment can't measure it faithfully and
CPU-DRAM latencies don't transfer to GPU-VRAM. So the authoritative timing/residency
validation rides on the deployment target — the GPU port — rather than a bare-metal
CPU run. The CPU version's job is to prove the design: the challenge math, the
correctness detector, and the *shapes* of the defenses (chaining defeats prefetch,
resident-vs-not is distinguishable in principle).

- [ ] **The GPU port**, with timing validation built in. *Done when:* the answer path
  runs as a CUDA kernel with the buffer in VRAM (`cudaMalloc`) and `map_index`'s
  128-bit multiply replaced by `__umul64hi`, **and** a cold read on real hardware
  shows the timing gap separates VRAM-resident from host-RAM or disk with the true
  per-access cost. Settle the adversaries and the design on CPU first.
- [ ] **A decision rule with error rates.** Turn the harness's separations into an
  accept/reject classifier. *Done when:* it outputs a verdict given a response and a
  claimed configuration, with measured false-positive and false-negative rates.
  Depends on the timing regime being settled on real hardware (the GPU validation
  above).

## Backlog

- [ ] **Responses ≥128 bits with a cryptographic hash.** 64 bits is guessable or
  precomputable against a real adversary. *Done when:* the response width grows, the
  known-answer test is updated, and the wire version bumps.
- [ ] **Optional: a bare-metal CPU cold read** (buffer > RAM and/or `O_DIRECT`,
  pinned with `taskset`). Not on the path — the real timing check happens on the GPU
  — but it's a cheap one-command early signal of whether the timing gap survives real
  storage. Run it only if you want that signal before the port.
- [ ] **Wire byte-order** is native and assumes same-endian peers; make it explicit
  if the protocol ever leaves loopback.
