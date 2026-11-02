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
datacenter's memory holds the workload it claims. **The GPU port runs**: a CUDA
prover, three GPU adversaries, and a live demo, with the known-answer gate passed
on an A40. The numbers so far come from one cheap card in one session, so read
them as ratios and as evidence the thing works, not as the port's operating
curve.

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
  container's vCPUs on performance or efficiency cores for its whole life, swinging
  absolute latency by up to ~1.6×. Trust only within-session ratios and paired
  differences. See [ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md).
- **There's no decision rule yet.** The harness reports how far apart the modes
  sit, not an accept/reject classifier with error rates.
- **The GPU port is correct, and measured once.** `tests/known_answer_gpu` passed
  on an A40: the CUDA kernel reproduces all six constants pinned before any CUDA
  existed, over an uploaded buffer and over one generated on-device, on both an
  A40 and an H100. Everything GPU-side is [MEASURED] on one card in one session
  with clocks unlockable, so the ratios hold and the absolute numbers do not
  travel.
- **The GPU detector has no early warning.** The UVM cheat is indistinguishable
  from honest until free HBM drops below the buffer size, then costs ~4,600x.
  Nothing in between. See
  [uvm-needs-hbm](docs/investigations/uvm-needs-hbm/README.md).
- **The fold is fixed, and the fix is about scaling.** The sequential
  cross-lane fold was over half the response and linear in lanes. `--fold tree`
  makes it log2(lanes) device passes: 8x the lanes cost the old fold 8.1x and
  the tree fold 1.24x, for an 8.9x faster response at 131k lanes. It is now
  launch-bound rather than work-bound. See
  [tree-fold-payoff](docs/investigations/tree-fold-payoff/README.md).
- **Two Argon2d passes defeat partial recompute.** A GPU prover that discards
  blocks and rebuilds them from the recipe answers wrong on every challenge at
  `t=2`, even discarding 0.1%, because each rebuild needs a first-pass value
  nobody kept. At `t=1` it answers correctly but slowly: 3.8x honest at 0.1%
  discarded, 34x at 10%. One H100 session. See
  [partial-recompute](docs/investigations/partial-recompute/README.md).
- **The detector's resolution is ~5% of the buffer.** A prover that holds every
  byte but keeps 5% off the card separates from honest; below that it hides in
  the noise. Unlike the UVM cliff this is a smooth gradient, which is what a
  decision rule needs. The floor is the width of the honest distribution on a
  card whose clocks cannot be locked, not physics. See
  [partial-residency-resolution](docs/investigations/partial-residency-resolution/README.md).

## Architecture

- `src/challenge.hpp` — the challenge math and nothing else, with no POSIX
  headers, so it can be included from a `.cu` translation unit. `mulhi64` is the
  one operation with two spellings: the 128-bit multiply on the host,
  `__umul64hi` on the device, behind `MC_HOST_DEVICE`.
- `src/net.hpp` — the wire structs (including the build-buffer `RecipeMsg`, the
  `kFlagBuild` Hello flag, and `kWireVersion`), the socket helpers, the `Source`
  reader the CPU provers ingest through, and the `/proc` readers. It includes
  `argon2d.hpp`. Host only; none of it has a device analog.
- `src/common.hpp` — a shim including both, so every host-side caller is
  unchanged. Include `challenge.hpp` alone from CUDA. Historically this file held
  both halves (`answer_lane_t`,
  `answer_challenge_t`, `splitmix64`, `mix64`, `map_index`, `fill_buffer`). The
  single source of truth, called by the prover, the cheating prover, and the
  verifier. **Frozen** — don't change it; a known-answer test pins it
  ([ADR 0001](docs/decisions/0001-challenge-math-frozen-pinned-by-known-answer.md)).
- `src/chacha20.hpp` — a small vendored ChaCha20 (RFC 8439) for keyed-stream buffer
  generation.
- `src/argon2d.hpp` — vendored Argon2d (RFC 9106) for the build-buffer design: the
  memory-hard generator that makes "regenerate on demand" more expensive than "keep the
  bytes". `mc::build_buffer()` hands back the Argon2d **matrix** as the challenge buffer
  — memchal wants the scratch memory, not the tag. Argon2d only, and **frozen the same
  way the challenge math is**: it is pinned against RFC vectors, so a change that breaks
  them means it is no longer Argon2d and ADR 0007's analysis no longer covers it. The
  `p` lanes of each slice fill on `p` threads. On the wire behind `--gen argon2d`: the
  verifier sends a `RecipeMsg` and both sides build. The CPU and GPU provers all handle
  build mode; the GPU provers build on the host CPU and copy the result to the card
  (building on the GPU itself is still open). See [ADR 0007](docs/decisions/0007-buffer-generator-memory-hard-function.md).
- `src/argon2d_rebuild.hpp` — the partial-recompute adversary's engine: rebuilds one
  final block from a partial copy of the matrix by walking Argon2d's dependency graph,
  with a per-challenge memo and a budget. Calls the frozen primitives; changes none.
- `src/bench_build.cpp` → `bench_build` — what one 1 KiB block costs to build, which is
  the attacker's unit of work in the build-buffer design. Not a protocol participant.
- `src/prover.cpp` — the honest TCP server. Maps, prefaults, and `mlock`s the
  buffer, verifies the lock through `VmLck`, and refuses to run if it isn't pinned
  ([ADR 0002](docs/decisions/0002-verify-and-refuse-on-mlock.md)).
- `src/verifier.cpp` — the client. Generates the buffer (`--gen splitmix64|chacha`) and
  uploads it, or sends a recipe (`--gen argon2d --build-t T --build-p P`) so both
  sides build it, times challenges, checks answers, and writes JSON and CSV. The three
  defenses default **on** (`--gen chacha --chain dependent --nonce random`); the
  benchmark opt-out is to name the weak setting (`--gen splitmix64 --chain
  independent --nonce sequential`).
- `src/cheating_prover.cpp` — the CPU adversaries: `honest`, `disk`, `partial`,
  `fake`, `mmapdisk`, `recompute`, `prefetch`, `precompute`.
- `src/gpu_answer.{hpp,cu}` — the CUDA answer path. The `.cu` is the only file
  `nvcc` touches and it includes `challenge.hpp` and the CUDA runtime, nothing
  else. One thread per lane. The header is free of CUDA types, so its callers
  build with plain `g++`.
- `src/prover_cuda.cpp` — the honest GPU server. `cudaMalloc` replaces
  `mmap`+`mlock`, and `cudaPointerGetAttributes` replaces reading `VmLck`
  ([ADR 0008](docs/decisions/0008-gpu-residency-check-substitutes-for-mlock.md)).
- `src/gpu_ingest.hpp` — the GPU provers' one ingestion path for both protocols:
  receive the uploaded bytes, or receive a recipe and build the buffer on the host,
  then push it to the card through pinned staging. Host only.
- `src/cheating_prover_cuda.cpp` — the GPU adversaries: `honest`, `fake`, `uvm`,
  `zerocopy`, `partial`, `recompute`. `recompute` (build mode only) keeps `--keep` of
  the blocks on the card and rebuilds the rest on host threads when a lane reaches
  one. It always claims `locked=true`, because nothing stops a real one doing the
  same. `--hog` occupies HBM so a managed buffer cannot fit.
- `scripts/gpu_pod_setup.sh` — the first command on a rented box: records the
  machine, refuses boxes whose numbers would be meaningless, settles whether
  clocks can be locked and whether RDMA exists, then builds and runs the gate.
- `scripts/gpu_adversaries.sh` — the honest baseline and all three GPU cheats,
  interleaved across cycles so thermal drift cannot look like a treatment effect.
- `demo/` — `bridge.py` drives the CPU provers in containers; `bridge_gpu.py`
  runs **on** a GPU box and spawns processes directly, because RunPod forbids
  nesting containers. The GPU page shows HBM occupancy rather than a page map:
  there is no GPU `mincore()`.
- `scripts/mc_stats.py` — the one place stats and error propagation live.
- `scripts/run_trials.py` — the harness: interleaved cycles, a `k=0` transport
  floor, ratios and paired differences with propagated error, drift detection, and
  session-stamped output.
- `scripts/plot.py` — one self-describing chart from `trials.json`.
- `tests/` — `known_answer.cpp` (pins the challenge math), `known_answer_gpu.cpp`
  (checks the CUDA kernel against those **same pinned constants**, not against a
  fresh CPU answer — two implementations agreeing only proves they drifted
  together), `known_answer_argon2d.cpp` (pins the generator against RFC 7693 and
  RFC 9106 vectors — values this codebase did not produce), `rebuild_argon2d.cpp` (every block the
  recompute engine rebuilds matches a full build), `differential_argon2d.cpp`
  (sweeps the generator against `libargon2`, a **test-only** dependency; nothing that
  ships links it), and `run_tests.sh` (the CPU known-answer tests plus invariants).

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

- **Never change the challenge math** in `challenge.hpp` (it's frozen; the
  known-answer test will fail, on CPU and on GPU). If a change is genuinely
  intended, add new test vectors and say so loudly.
- **Keep the math free of POSIX.** `challenge.hpp` must stay includable from a
  `.cu` file, so sockets and `/proc` belong in `net.hpp`.
- **Never fold lane hashes out of order.** The cross-lane fold is a sequential
  chain, so a tree or warp-shuffle reduction silently changes every answer.
  `fold_seed`, `lane_samples`, and `lane_fold_term` exist so there is one
  definition of that order.
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

## Map of the repo

- `README.md` — the human-facing overview.
- `NEXT_STEPS.md` — pending work, with rationale.
- `docs/decisions/` — [ADRs](docs/decisions/README.md): why the design and claims
  are what they are.
- `docs/investigations/` — [analyses](docs/investigations/README.md) (question,
  script, result) that back the decisions.
- `src/`, `scripts/`, `tests/` — the core of the project.
