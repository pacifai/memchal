# CLAUDE.md — memchal (a memory-retention challenge)

Orientation for anyone working in this repo, human or agent. Read this first, then
follow the links as you need them. Keep this file current when you change how the
project works.

## What this is

A proof-of-retention primitive. A verifier uploads a large buffer of random bytes
to a prover, then issues timed challenges. Each challenge makes the prover derive
thousands of nonce-based offsets across the whole buffer, read those words, and
fold them into a chained hash. You can't answer correctly without reading the
data. Two signals expose a cheat: the answer is **wrong** (the prover fabricated
data) or the answer is **slow** (the data isn't in fast memory).

It's the CPU precursor to a GPU/VRAM version for AI-compute governance — proving a
datacenter's memory holds the workload it claims. The GPU port is future work; the
answer path is written to port cleanly (a flat buffer, no STL).

## Status — what to trust

- **Correctness detection is solid.** `fake` fails every challenge; the catch
  probability is `1 − (1−m)^k`. This is the claim that holds up under attack.
- **Timing tracks residency, not the storage device.** The old "disk is ~15×
  slower" gap was `pread` system-call overhead. A memory-mapped adversary looks
  honest only because the page cache holds its file — it really is resident. Starve
  its RAM below the buffer size and it runs about 840× slower, with roughly half
  its reads faulting to storage. Not yet confirmed on real hardware with a buffer
  larger than RAM, so the current figure is a lower bound. See
  [ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md).
- **The three shortcut attacks are defended.** A prover can recompute the buffer
  from the seed, prefetch predictable offsets, or precompute answers under
  predictable nonces. Each has a defense — ChaCha20 keyed-stream generation,
  data-dependent (chained) offsets, and an unpredictable nonce — with tests that
  show each attack win with the defense off and lose with it on. See
  [ADR 0004](docs/decisions/0004-adversary-taxonomy-and-defenses.md) and
  [ADR 0005](docs/decisions/0005-per-lane-chaining-vs-gpu-parallelism.md).
- **Absolute latencies don't carry across sessions.** Apple Silicon places each
  container's vCPUs on performance or efficiency cores for its whole life,
  swinging absolute latency by up to ~1.6×. Trust only within-session ratios and
  paired differences. See [ADR
  0003](docs/decisions/0003-timing-detector-scope-and-measurement.md).
- **There's no decision rule yet.** The harness reports how far apart the modes
  sit, not an accept/reject classifier with error rates.

## Architecture

- `src/common.hpp` — the wire protocol and the challenge math (`answer_lane_t`,
  `answer_challenge_t`, `splitmix64`, `mix64`, `map_index`, `fill_buffer`). The
  single source of truth, called by the prover, the cheating prover, and the
  verifier. **Frozen** — don't change it; a known-answer test pins it
  ([ADR 0001](docs/decisions/0001-challenge-math-frozen-pinned-by-known-answer.md)).
- `src/chacha20.hpp` — a small vendored ChaCha20 (RFC 8439) for keyed-stream buffer
  generation.
- `src/argon2d.hpp` — vendored Argon2d (RFC 9106) for the build-buffer design:
  the memory-hard generator that makes "regenerate on demand" more expensive
  than "keep the bytes". `mc::build_buffer()` hands back the Argon2d **matrix**
  as the challenge buffer — memchal wants the scratch memory, not the tag.
  Argon2d only, and **frozen the same way `common.hpp` is**: it is pinned
  against RFC vectors, so a change that breaks them means it is no longer
  Argon2d and ADR 0007's analysis no longer covers it. Not yet on the wire; see
  [ADR 0007](docs/decisions/0007-buffer-generator-memory-hard-function.md).
- `src/bench_build.cpp` → `bench_build` — what one 1 KiB block costs to build, which is
  the attacker's unit of work in the build-buffer design. Not a protocol participant.
- `src/prover.cpp` — the honest TCP server. Maps, prefaults, and `mlock`s the
  buffer, verifies the lock through `VmLck`, and refuses to run if it isn't pinned
  ([ADR 0002](docs/decisions/0002-verify-and-refuse-on-mlock.md)).
- `src/verifier.cpp` — the client. Generates the buffer (`--gen splitmix64|chacha`),
  uploads it, times challenges, checks answers, and writes JSON and CSV. The three
  defenses default **on** (`--gen chacha --chain dependent --nonce random`); the
  benchmark opt-out is to name the weak setting (`--gen splitmix64 --chain
  independent --nonce sequential`).
- `src/cheating_prover.cpp` — the adversaries: `honest`, `disk`, `partial`, `fake`,
  `mmapdisk`, `recompute`, `prefetch`, `precompute`.
- `scripts/mc_stats.py` — the one place stats and error propagation live.
- `scripts/run_trials.py` — the harness: interleaved cycles, a `k=0` transport
  floor, ratios and paired differences with propagated error, drift detection, and
  session-stamped output.
- `scripts/plot.py` — one self-describing chart from `trials.json`.
- `tests/` — `known_answer.cpp` (pins the challenge math), `known_answer_argon2d.cpp`
  (pins the generator against RFC 7693 and RFC 9106 vectors — values this codebase did
  not produce), `differential_argon2d.cpp` (sweeps the generator against `libargon2`, a
  **test-only** dependency; nothing that ships links it), and `run_tests.sh` (all three
  plus invariants).

## Build, run, test

Everything goes through the container.

```bash
./run.sh                                   # build + smoke test
./run.sh bash tests/run_tests.sh           # known-answer test + invariants
./run.sh bash -c 'make && \
  python3 scripts/run_trials.py --repeats 10 --n 3000 --k 4096 --mb 64 \
     --modes floor,honest,disk,partial,fake,mmapdisk --out results/trials.json && \
  python3 scripts/plot.py results/trials.json'
```

`run.sh` builds a linux/arm64 image and runs it with `--cap-add=IPC_LOCK
--ulimit memlock=-1`, a tmpfs at `/ram`, explicit `--cpus` and `--memory`, and
`results/` mounted to the host. Binaries build to the repo root from `src/`.

## The run environment, and its limits

Docker Desktop on Apple Silicon runs a linux/arm64 VM (kernel 6.10) with 4 KiB
pages (the host uses 16 KiB). Storage is virtualized through the host page cache,
so this environment **can't measure real storage physics**, and vCPU placement on
performance vs efficiency cores makes absolute latency swing between sessions. What
it measures reliably: correctness, how latency scales with `k`, the transport
floor, run-to-run variance, and within-session ratios between modes. A run on bare
metal Linux is one command away and is what would settle the storage-physics and
core-affinity questions (see `NEXT_STEPS.md`).

## Rules for agents working here

- **Never change the challenge math** in `common.hpp` (it's frozen; the known-answer
  test will fail). If a change is genuinely intended, add new test vectors and say
  so loudly.
- **Compare within a session only.** Never present absolute microseconds as
  portable; use the harness's ratios or `honest − floor` with propagated error.
- **Don't tune parameters to make results look better.** A cheat that isn't caught
  is a finding. Report negative or nuanced results plainly (see the session-variance
  investigation, where a rerun showed 1.05× instead of an earlier 1.6×).
- **Keep the honest-baseline lock guarantee.** Don't weaken the verify-and-refuse
  path.
- **Stay buildable with g++, the standard library, and python3/matplotlib.** Ask
  before adding dependencies.
- **Put investigations under `docs/investigations/`** (doc, script, results), not
  mixed into `src/` or `scripts/`.
- **`NEXT_STEPS.md` holds only pending work.** When something ships, move its
  description to `README.md` or an ADR and delete it from `NEXT_STEPS.md` — the file
  must never accumulate already-implemented changes.
- **Merge conversations into the living files; don't stack up new documents.**
  When a working session produces something durable, it belongs in whichever
  living file already owns that subject — new work in `NEXT_STEPS.md`, a settled
  choice as an ADR, a changed claim in the status section above and in the
  warehouse visual, a thread waiting on someone in `docs/open-questions.md`. Write
  a dated document under `docs/analysis/` only for reasoning long enough to be
  worth keeping whole, and even then write its consequences into the living files
  in the same pass. The workspace `/sync` command does this end to end.
- **Say a thing once.** If a fact is in an ADR, the living files cite it rather
  than restating it. Two copies of a number is how they drift.

## What's still open (see NEXT_STEPS.md)

1. **Proving residency of the real workload** — the hardest open problem: prove the
   prover holds something tied to the actual model (not filler), via a memory-hard,
   weight-bound generator built from a short recipe, without friction for model
   providers. See the roadmap.
2. **Timing/residency validation on real hardware** — the CPU-in-Docker environment
   can't measure it faithfully, and CPU-DRAM latencies don't transfer to GPU-VRAM, so
   the authoritative check rides on the GPU port (a bare-metal CPU cold read is an
   optional early de-risk).
3. A **decision rule** with stated false-positive and false-negative rates.
4. The **GPU port**, plus protocol hardening (≥128-bit responses).

## Where knowledge lives

Two kinds of file, and the difference is the whole filing system.

**Living files — rewritten in place, never appended to.** Read these to find out
where the project is. If one of them is stale, that is a bug.

- `CLAUDE.md` (this file) — orientation, current status, rules.
- `README.md` — the human-facing overview of what is built and what it proves.
- `NEXT_STEPS.md` — pending work only. Nothing that already shipped.
- `docs/open-questions.md` — threads blocked on another person or on missing
  information. Entries get deleted when they close, not marked resolved.

**Evidence — dated, immutable once written, superseded rather than edited.**
Follow a link into these when a living file cites one.

- `docs/decisions/` — [ADRs](docs/decisions/README.md): why the design and claims
  are what they are.
- `docs/investigations/` — [measurements](docs/investigations/README.md)
  (question, script, result) that back the decisions.
- `docs/analysis/` — dated reasoning documents from working sessions: the
  advisor review that originates the layered stack, the GPU port plan, the
  external landscape, the advisor feedback, and provider setup. Private names
  are replaced with role labels here, so these are safe to share as-is.

`src/`, `scripts/`, `tests/` are the core of the project.

The visual companion lives at `../memory-warehouse/`. It restates this repo's
findings, so it inherits this repo's caveats and has to move when a claim moves.
