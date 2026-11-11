# Contributing

This guide covers where things are, how to build and test, and the rules that
keep results trustworthy. For what memchal is, start with the
[README](README.md).

## Layout

| Path | Contents |
|---|---|
| `src/shared/` | The challenge math, the buffer generators, and the messages that the verifier and provers exchange |
| `src/verifier/` | The verifier |
| `src/cpu/`, `src/gpu/` | The honest provers |
| `src/attacks/` | The cheating provers, built separately from the core |
| `src/record/` | The events that the provers and the verifier print into a session file |
| `src/bench/` | A benchmark for the cost of building the buffer |
| `tests/` | The tests, in folders that mirror `src/`, and `run_tests.sh` |
| `scripts/` | The trial harness and its plots, and the GPU session recorder |
| `runpod/` | Renting a RunPod GPU, preparing it, recording a session there, and releasing it |
| `docker/` | The toolchain image, and `run.sh`, which runs a command in it |
| `docs/decisions/` | The decision records behind the design and its claims |

Every program includes headers by their path under `src/`, for example
`"shared/challenge.hpp"`. `make` writes everything under `build/`.

## Build and test

### CPU

Everything on the CPU side builds and runs in Docker:

```bash
docker/run.sh                           # build, then one verifier against one honest prover
docker/run.sh bash tests/run_tests.sh   # all CPU tests
```

`docker/run.sh` builds a `linux/arm64` image. To use another platform, set
`PLATFORM`, for example `PLATFORM=linux/amd64`.

To measure every CPU prover over repeated trials and plot the result:

```bash
docker/run.sh bash -c 'make && \
  python3 scripts/run_trials.py --repeats 10 --challenges 3000 --samples 4096 --mb 64 \
     --modes floor,honest,disk,partial,fake,mmapdisk --out results/trials.json && \
  python3 scripts/plot.py results/trials.json'
```

### GPU

On a machine with the CUDA toolkit, `make gpu` builds the GPU provers and their
known-answer test. Run `build/tests/gpu/answer_kat` before you trust any GPU
measurement.

To rent a GPU instead, see
[Record a session on a GPU](README.md#record-a-session-on-a-gpu).

### Tests

`tests/run_tests.sh` builds everything and runs two kinds of tests:

- **Known-answer tests** pin the challenge math, the buffer generators, and
  Argon2d to fixed values, so a change to any of them fails.
- **End-to-end tests** run a verifier against the honest prover and each
  attack, and check that every answer is right or every answer is wrong, as
  expected.

CI runs these on every pull request and compiles the GPU code. The GPU
known-answer test needs a card, so it runs when a pod is prepared.

## Test environment limits

Docker on Apple Silicon runs a virtual machine, and its storage goes through
the Mac's own cache. This setup can't measure real storage speed, and absolute
times move by up to about 1.6x between sessions.

It measures these reliably: correctness, how response time grows with the
number of reads, the network overhead, run-to-run variation, and ratios between
provers within one session.

## Rules

- **Leave the challenge math alone.** `challenge.hpp` and `argon2d.hpp` are
  frozen, and the known-answer tests fail on any change. If you intend a
  change, add new test vectors and say so in the pull request.
- **Keep the challenge math portable.** `challenge.hpp` compiles for both the
  CPU and the GPU, so it includes no operating system headers.
- **Combine chain results only through the functions in `challenge.hpp`.** Any
  other way of combining them changes every answer.
- **Keep the parts apart.** Code that one part needs goes in that part's
  folder. Code that two parts need goes in `src/shared/`. Attack code goes in
  `src/attacks/`, and session events in `src/record/`.
- **Keep the core free of dependencies.** The C++ builds with `g++` and the
  standard library. The scripts and the RunPod tooling may use well-known Python
  packages: declare them at the top of the script
  ([PEP 723](https://peps.python.org/pep-0723/)) so that `uv run` fetches them.
- **Keep each language in its own file.** Put Python, JavaScript, and other
  code in its own file and call it from a shell script.
- **Keep the honest prover's memory check.** It refuses to run when the buffer
  isn't locked in fast memory.
- **Compare within a session.** Absolute times don't carry between sessions.
  Report ratios, with their error.
- **Report what you measure.** A cheat that goes uncaught is a finding. Keep
  the settings as they were before the run, and report negative or mixed
  results plainly.
- **Keep `NEXT_STEPS.md` to open work.** When an item ships, delete it there
  and record the result in the README or a decision record.
