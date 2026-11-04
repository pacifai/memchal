# Contributing

This guide covers how the repository is laid out, how to build and test it, the
limits of the test environment, and the rules that keep results trustworthy.

## Repository layout

`src/` holds the C++ and CUDA sources, one folder per part. Every program
includes headers by their path under `src/`, for example `"shared/challenge.hpp"`.

### `src/shared/`: the challenge and the protocol

- `challenge.hpp` — the challenge math: offset derivation, the per-lane hash,
  and both cross-lane folds. It compiles for host and device, so it includes no
  POSIX headers. Frozen, and pinned by a known-answer test
  ([ADR 0001](docs/decisions/0001-challenge-math-frozen-pinned-by-known-answer.md)).
- `chacha20.hpp` — ChaCha20 (RFC 8439), used to generate an uploaded buffer from
  a random key.
- `argon2d.hpp` — Argon2d (RFC 9106), the memory-hard generator for build mode:
  the verifier sends a short recipe and both sides build the buffer. Frozen like
  the challenge math, and pinned against RFC test vectors
  ([ADR 0007](docs/decisions/0007-buffer-generator-memory-hard-function.md)).
- `argon2d_rebuild.hpp` — rebuilds one Argon2d block from a partial copy of the
  matrix, for the recompute adversary.
- `protocol.hpp` — the wire messages, the limits a prover enforces on them, and
  how a prover answers a challenge message.
- `socket.hpp`, `server.hpp` — blocking socket I/O, and the parts every prover
  shares: the listener, the opening exchange, receiving or building the buffer,
  and the challenge loop.
- `cli.hpp`, `json.hpp`, `random.hpp`, `thread_pool.hpp` — small utilities.

### Programs

- `src/verifier/` — the verifier. `main.cpp` runs a session; `args.hpp` parses
  flags, `reference.hpp` generates the buffer, `session.hpp` issues and checks
  challenges, and `report.hpp` writes the latency profile, CSV, and JSON.
- `src/cpu/` — `prover.cpp` is the honest CPU prover: it locks the buffer in RAM,
  confirms the lock through `VmLck`, and refuses to run unlocked
  ([ADR 0002](docs/decisions/0002-verify-and-refuse-on-mlock.md)).
  `cheating_prover.cpp` holds the CPU adversaries: `disk`, `partial`, `fake`,
  `mmapdisk`, `recompute`, `prefetch`, and `precompute`.
- `src/gpu/` — `prover.cpp` is the honest GPU prover: it allocates with
  `cudaMalloc` and confirms the allocation type with `cudaPointerGetAttributes`
  ([ADR 0008](docs/decisions/0008-gpu-residency-check-substitutes-for-mlock.md)).
  `cheating_prover.cpp` holds the GPU adversaries: `fake`, `uvm`, `partial`, and
  `recompute`. Only the `.cu` files go through `nvcc`; the headers use no CUDA
  types, so the programs build with `g++`.
- `src/demo/` — the `@`-prefixed JSON events the browser demo reads. Nothing
  else depends on them.
- `src/bench/build_cost.cpp` — measures what one 1 KiB Argon2d block costs to
  build.

### Everything else

- `tests/` — the tests, in folders that mirror `src/`, and `run_tests.sh`.
- `scripts/` — the trial harness (`run_trials.py`), its statistics (`mc_stats.py`)
  and plots (`plot.py`), and the GPU adversary sweep (`gpu_adversaries.sh`).
- `demo/` — the live demo. `bridge.py` drives the CPU provers in Docker;
  `bridge_gpu.py` runs on a GPU machine and starts the provers directly.
- `docker/` — the toolchain image and `run.sh`, which runs a command in it.
- `runpod/` — renting a RunPod GPU, setting it up, running the demo there, and
  releasing it.
- `docs/decisions/` — the decision records behind the design and its claims.

`make` writes every binary and object under `build/`.

## Build and test

### CPU, in Docker

Everything on the CPU side builds and runs in a container:

```bash
docker/run.sh                           # build and run a one-challenge smoke test
docker/run.sh bash tests/run_tests.sh   # the full CPU test suite
```

`docker/run.sh` builds a `linux/arm64` image and runs it with memory locking
allowed, a RAM-backed filesystem at `/ram`, fixed CPU and memory limits, and the
repository mounted at `/work`. To use another platform, set `PLATFORM`, for
example `PLATFORM=linux/amd64`.

To run the trial harness and plot its result:

```bash
docker/run.sh bash -c 'make && \
  python3 scripts/run_trials.py --repeats 10 --challenges 3000 --samples 4096 --mb 64 \
     --modes floor,honest,disk,partial,fake,mmapdisk --out results/trials.json && \
  python3 scripts/plot.py results/trials.json'
```

### GPU

On a machine with the CUDA toolkit, `make gpu` builds `build/gpu/prover`,
`build/gpu/cheating_prover`, and `build/tests/gpu/answer_kat`. Run
`build/tests/gpu/answer_kat` before trusting any GPU measurement.

To rent a RunPod GPU instead, register an SSH key on your RunPod account, copy
`.env.example` to `.env`, and fill in `RUNPOD_API_KEY` and `MEMCHAL_SSH_KEY`. Then
run `bash runpod/gpu_demo_up.sh`. It prints the hourly price,
rents the card, copies this working tree, builds, runs the known-answer test, and
opens the demo. `bash runpod/gpu_demo_down.sh` copies the results back and
releases the card.

### What the tests cover

- `tests/shared/challenge_kat.cpp` — pins the challenge math and the ChaCha20
  fill to fixed constants.
- `tests/shared/argon2d_kat.cpp` — pins Argon2d against the RFC 7693 and RFC 9106
  test vectors.
- `tests/shared/argon2d_differential.cpp` — compares Argon2d with the reference
  `libargon2` across many parameter combinations. Only this test links
  `libargon2`.
- `tests/shared/argon2d_rebuild_test.cpp` — checks that every block the rebuild
  engine produces matches a full build.
- `tests/cpu/seed_recovery_test.cpp` — checks the recompute adversary's
  splitmix64 seed recovery.
- `tests/gpu/answer_kat.cpp` — checks the CUDA kernel against the same constants
  as `challenge_kat.cpp`, for both folds.
- `tests/demo/` — checks the demo's event streams and its canvas sizing.
- `tests/run_tests.sh` — builds everything, runs the tests above except the GPU
  one, and then runs a verifier against each prover and adversary to check that
  every answer is right or every answer is wrong, as expected.

CI runs the CPU suite and compiles the GPU targets on every pull request.

## Test environment limits

Docker on Apple Silicon runs a `linux/arm64` virtual machine with 4 KiB pages.
Storage is virtualized through the host's page cache, so this environment can't
measure real storage speed. The virtual CPUs land on performance or efficiency
cores for a container's whole life, which moves absolute latency by up to about
1.6x between sessions.

It measures these reliably: correctness, how latency scales with the samples per challenge, the
network and fixed overhead (the `floor` mode, which sends zero probes),
run-to-run variance, and ratios between modes within one session.

## Rules

- **Leave the challenge math alone.** `challenge.hpp` and `argon2d.hpp` are frozen,
  and the known-answer tests fail on any change. If a change is intended, add new
  test vectors and say so in the pull request.
- **Keep the math free of POSIX.** `challenge.hpp` must stay includable from a `.cu`
  file, so sockets and `/proc` stay out of it.
- **Keep the parts apart.** Code one part needs goes in that part's folder; code
  two parts need goes in `src/shared/`. Demo-only output goes in `src/demo/`.
- **Fold lane hashes only through the functions in `challenge.hpp`.** The
  sequential fold chains lanes in order, and the tree fold pairs them in a fixed
  order. Any other reduction, such as a warp shuffle, changes every answer.
- **Keep each language in its own file.** Put Python, JavaScript, and other code
  in its own file, and call it from a shell script. Never embed it through a
  heredoc or `python3 -c`.
- **Compare within a session.** Absolute microseconds don't carry between sessions.
  Report the harness's ratios or `honest − floor`, with propagated error.
- **Report what you measure.** A cheat that goes uncaught is a finding. Keep
  parameters as they were set before the run, and report negative or mixed
  results plainly.
- **Keep the honest prover's lock check.** It refuses to run when the buffer isn't
  locked in memory.
- **Build with `g++`, the standard library, and Python with matplotlib.** Ask
  before adding a dependency.
- **Keep `NEXT_STEPS.md` to open work.** When an item ships, delete it there and
  record the result in the README or a decision record.
