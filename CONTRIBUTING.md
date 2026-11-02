# Contributing

This guide covers how the repository is laid out, how to build and test it, the
limits of the test environment, and the rules that keep results trustworthy.

## Repository layout

### Challenge and protocol

- `src/challenge.hpp` — the challenge math: offset derivation, the per-lane hash,
  and both cross-lane folds. It compiles for host and device, so it includes no
  POSIX headers. `mulhi64` is the one operation with two spellings: a 128-bit
  multiply on the host and `__umul64hi` on the device. Frozen, and pinned by a
  known-answer test
  ([ADR 0001](docs/decisions/0001-challenge-math-frozen-pinned-by-known-answer.md)).
- `src/net.hpp` — the wire messages, the limits a prover enforces on them, socket
  helpers, and the `/proc` readers. Host only.
- `src/chacha20.hpp` — ChaCha20 (RFC 8439), used to generate an uploaded buffer
  from a random key.
- `src/argon2d.hpp` — Argon2d (RFC 9106), the memory-hard generator for the
  build-buffer design: the verifier sends a short recipe and both sides build the
  buffer. `mc::build_buffer()` returns the Argon2d matrix itself as the buffer.
  Frozen like the challenge math, and pinned against RFC test vectors
  ([ADR 0007](docs/decisions/0007-buffer-generator-memory-hard-function.md)).

### Programs

- `src/verifier.cpp` — the client. It generates the buffer and uploads it
  (`--gen chacha` or `--gen splitmix64`), or sends a recipe (`--gen argon2d`). It
  then times challenges, checks answers, and writes JSON and CSV. The defenses are
  on by default; `--help` lists the flags.
- `src/prover.cpp` — the honest CPU prover. It maps, prefaults, and locks the
  buffer with `mlock`, confirms the lock through `VmLck`, and refuses to run
  unlocked ([ADR 0002](docs/decisions/0002-verify-and-refuse-on-mlock.md)).
- `src/cheating_prover.cpp` — the CPU adversaries: `honest`, `disk`, `partial`,
  `fake`, `mmapdisk`, `recompute`, `prefetch`, and `precompute`.
- `src/prover_cuda.cpp` — the honest GPU prover. It allocates with `cudaMalloc`
  and confirms the allocation type with `cudaPointerGetAttributes`
  ([ADR 0008](docs/decisions/0008-gpu-residency-check-substitutes-for-mlock.md)).
- `src/cheating_prover_cuda.cpp` — the GPU adversaries: `honest`, `fake`, `uvm`,
  `zerocopy`, `partial`, and `recompute`.
- `src/gpu_answer.cu`, `src/gpu_answer.hpp` — the CUDA answer path, one thread per
  lane. The `.cu` file is the only one `nvcc` compiles. The header uses no CUDA
  types, so its callers build with `g++`.
- `src/gpu_ingest.hpp` — how the GPU provers receive the buffer: uploaded bytes,
  or a recipe they build on the host, copied to the card through pinned staging.
- `src/argon2d_rebuild.hpp` — the partial-recompute adversary's engine. It rebuilds
  one Argon2d block from a partial copy of the matrix.
- `src/bench_build.cpp` — measures what one 1 KiB Argon2d block costs to build.

### Everything else

- `tests/` — the test programs and `run_tests.sh`, described below.
- `scripts/` — the trial harness (`run_trials.py`), its statistics (`mc_stats.py`)
  and plots (`plot.py`), and the GPU adversary sweep (`gpu_adversaries.sh`).
- `runpod/` — renting a RunPod GPU, setting it up and running the known-answer
  test on it, running the demo bridge there, and releasing it.
- `demo/` — the live demo. `bridge.py` drives the CPU provers in Docker;
  `bridge_gpu.py` runs on a GPU machine and starts the provers directly.
- `docs/decisions/` — the decision records behind the design and its claims.

## Build and test

### CPU, in Docker

Everything on the CPU side builds and runs in a container:

```bash
./run.sh                           # build and run a one-challenge smoke test
./run.sh bash tests/run_tests.sh   # the full CPU test suite
```

`run.sh` builds a `linux/arm64` image and runs it with memory locking allowed, a
RAM-backed filesystem at `/ram`, fixed CPU and memory limits, and `results/`
mounted from the host. To use another platform, set `PLATFORM`, for example
`PLATFORM=linux/amd64`.

To run the trial harness and plot its result:

```bash
./run.sh bash -c 'make && \
  python3 scripts/run_trials.py --repeats 10 --n 3000 --k 4096 --mb 64 \
     --modes floor,honest,disk,partial,fake,mmapdisk --out results/trials.json && \
  python3 scripts/plot.py results/trials.json'
```

### GPU

On a machine with the CUDA toolkit, `make gpu` builds `prover_cuda`,
`cheating_prover_cuda`, and `tests/known_answer_gpu`. Run
`tests/known_answer_gpu` before trusting any GPU measurement.

To rent a RunPod GPU instead, register an SSH key on your RunPod account, copy
`.env.example` to `.env`, and fill in `RUNPOD_API_KEY` and `MEMCHAL_SSH_KEY`. Then
run `bash runpod/gpu_demo_up.sh`. It prints the hourly price,
rents the card, copies this working tree, builds, runs the known-answer test, and
opens the demo. `bash runpod/gpu_demo_down.sh` copies the results back and
releases the card.

### What the tests cover

- `tests/known_answer.cpp` — pins the challenge math and the ChaCha20 fill to
  fixed constants.
- `tests/known_answer_gpu.cpp` — checks the CUDA kernel against the same
  constants, for both folds.
- `tests/known_answer_argon2d.cpp` — pins Argon2d against the RFC 7693 and
  RFC 9106 test vectors.
- `tests/differential_argon2d.cpp` — compares Argon2d with the reference
  `libargon2` across many parameter combinations. Only this test links
  `libargon2`.
- `tests/rebuild_argon2d.cpp` — checks that every block the recompute engine
  rebuilds matches a full build.
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

It measures these reliably: correctness, how latency scales with `k`, the
network and fixed overhead (the `floor` mode, which sends zero probes),
run-to-run variance, and ratios between modes within one session.

## Rules

- **Leave the challenge math alone.** `challenge.hpp` and `argon2d.hpp` are frozen,
  and the known-answer tests fail on any change. If a change is intended, add new
  test vectors and say so in the pull request.
- **Keep the math free of POSIX.** `challenge.hpp` must stay includable from a `.cu`
  file, so sockets and `/proc` belong in `net.hpp`.
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
