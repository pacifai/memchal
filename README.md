# memchal

[![tests](https://github.com/pacifai/memchal/actions/workflows/tests.yml/badge.svg)](https://github.com/pacifai/memchal/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

memchal checks that a computer keeps a block of data in its fast memory: RAM on
a CPU, or on-chip memory on a GPU.

A **verifier** gives a **prover** a large buffer of random data. It then sends
challenges. To answer one, the prover has to read thousands of places in the
buffer that it can't predict. The verifier checks two things about every answer:

- **Is it correct?** A wrong answer means the prover lacks some of the data.
- **Is it quick?** A slow answer means the data came from somewhere slower than
  fast memory.

## Why

Training a new AI model and serving an existing one use the same chips. A rule
that limits training holds only if someone can check what the chips declared as
serving are doing, including the memory they leave spare, which a hidden
training job could use.

If a verifier fills that spare memory with its own data and keeps checking that
the data stays there, nothing else fits. memchal is that check.

memchal is a research prototype. It measures how far apart an honest prover and
a cheating one are. A rule that turns those measurements into a pass or fail
verdict is open work, listed with the rest in [NEXT_STEPS.md](NEXT_STEPS.md).

## Quickstart

### Try it on your computer

You need [Docker](https://docs.docker.com/get-docker/). The first command builds
memchal and runs one verifier against one honest prover in RAM. The second runs
the tests, which include every attack.

```bash
docker/run.sh
docker/run.sh bash tests/run_tests.sh
```

### Record a session on a GPU

You need [uv](https://docs.astral.sh/uv/) and a [RunPod](https://www.runpod.io)
account with an
[API key](https://docs.runpod.io/get-started/api-keys) and a registered
[SSH key](https://docs.runpod.io/pods/configuration/use-ssh).

```bash
cp .env.example .env    # then set RUNPOD_API_KEY and MEMCHAL_SSH_KEY in .env
uv run runpod/record.py
```

The command shows the hourly price and waits for your yes. It then rents an
NVIDIA H100, builds memchal there, runs the honest prover and four attacks,
saves the session to `results/sessions/`, and terminates the pod, which ends
the billing. It terminates the pod when a step fails, too.

To replay the session, open the
[demo site](https://pacifai.github.io/compute-verification-demo/), select
**Custom run**, and add the session file.

## How it works

1. The verifier and the prover agree on a buffer. The verifier either uploads
   it or sends a short recipe that both sides build it from.
2. The verifier sends a challenge: a random number, called a nonce.
3. From the nonce, the prover derives the places to read, reads them, and
   combines what it read into one value.
4. The verifier computes the same value from its own copy, compares, and records
   how long the answer took.

The prover and the verifier share one definition of this math, in
[`src/shared/challenge.hpp`](src/shared/challenge.hpp). It is frozen and pinned
by known-answer tests.

## Attacks

A cheating prover tries to pass while keeping the data somewhere cheaper. Each
attack is a mode of a cheating prover in [`src/attacks/`](src/attacks/), and a
test shows how the verifier catches it.

| Attack | What the prover does | What catches it |
|---|---|---|
| `fake` | Makes up the data it doesn't hold | Correctness |
| `disk`, `partial` (CPU) | Keeps some or all of the data on disk | Timing |
| `mmapdisk` (CPU) | Memory-maps the data from disk | Timing, once RAM is too small to cache the data |
| `partial` (GPU) | Keeps part of the data in the server's main memory | Timing |
| `uvm` (GPU) | Lets the driver move data on and off the chip | Timing, once the chip is too full to hold the data |
| `recompute` | Discards the data and regenerates it from its seed | A buffer made from a random key, or built with Argon2d |
| `prefetch` (CPU) | Works out the places to read ahead of time | Dependent chains |
| `precompute` (CPU) | Guesses the next nonce and answers early | Random nonces |

The last three beat both checks until a matching defense is on. The defenses
are on by default.

## Configuration

The verifier's flags set the buffer, the challenge, and the defenses. Run
`build/verifier --help` for the full list.

| Flag | What it sets | Default |
|---|---|---|
| `--mb` | Buffer size in MiB | |
| `--samples`, `--lanes` | Reads per challenge, and how many parallel chains they split into | |
| `--challenges`, `--warmup` | Measured and unmeasured challenges | |
| `--gen` | How the buffer is made: `chacha`, `argon2d`, or `splitmix64` | `chacha` |
| `--chain` | `dependent`: each read's place depends on the previous read | `dependent` |
| `--nonce` | `random` or `sequential` nonces | `random` |
| `--fold` | How per-chain values combine: `sequential` or `tree` | `sequential` |
| `--verify-every` | Check one answer in this many | `1` |

The recording uses the settings at the top of
[`scripts/gpu_record.py`](scripts/gpu_record.py).

## Functions

| Function | Use | Source |
|---|---|---|
| Argon2d | Builds the buffer from a recipe, so that regenerating a discarded part is slow | [RFC 9106](https://www.rfc-editor.org/rfc/rfc9106) |
| ChaCha20 | Fills an uploaded buffer from a random key | [RFC 8439](https://www.rfc-editor.org/rfc/rfc8439) |
| splitmix64 | Derives the places to read. As a buffer fill, it is the weak setting that `recompute` beats | [Reference code](https://prng.di.unimi.it/splitmix64.c) |
| MurmurHash3 finalizer | Combines the words read into the answer | [smhasher](https://github.com/aappleby/smhasher) |
| `mlock`, checked through `VmLck` | Keeps the honest CPU prover's buffer in RAM | |
| `cudaMalloc`, checked through `cudaPointerGetAttributes` | Keeps the honest GPU prover's buffer on the chip | |

The honest provers refuse to run when the check fails. The
[decision records](docs/decisions/) explain each choice.

## Results

Compare times within one session only. Absolute times change between sessions
and machines.

**GPU, one NVIDIA H100 session.** 4096 MiB buffer, 262,144 reads in 16,384
chains, tree fold.

| Prover | Median answer | Against honest | Wrong answers | Caught by |
|---|---:|---:|---:|---|
| Honest | 208 µs | 1.00x | 0 | |
| `fake` | 175 µs | 0.84x | 100 of 100 | Correctness |
| `partial`, all data in main memory | 5.95 ms | 28.6x | 0 | Timing |
| `uvm`, chip full | 3.49 s | 16,786x | 0 | Timing |
| `uvm`, chip has room (NVIDIA A40) | 653 µs | 1.04x | 0 | Passes: the data is on the chip |

**CPU, Docker on Apple Silicon.** 64 MiB buffer, 4096 reads in 32 chains.

| Prover | Median answer | Against honest | Wrong answers |
|---|---:|---:|---:|
| Honest | ~114 µs | 1x | 0 |
| `fake` | ~108 µs | ~0.95x | Every challenge |
| `disk` | ~1850 µs | ~15x | 0 |
| `mmapdisk`, RAM to spare | ~122 µs | ~1.05x | 0 |
| `mmapdisk`, RAM below the buffer size | ~81,000 µs | ~840x | 0 |

## Security

A prover listens on every network interface and trusts whoever connects. Run it
only on a network you control.

## Learn more

- [CONTRIBUTING.md](CONTRIBUTING.md): the repository layout, how to build and
  test, and the rules for changes.
- [Decision records](docs/decisions/): why the design is the way it is, and the
  measurements behind each claim.
- [NEXT_STEPS.md](NEXT_STEPS.md): open work.

## License

[MIT](LICENSE)
