# Memchal — a memory-retention challenge

A verifier sends a prover a large buffer of random bytes, then repeatedly asks it
to prove the buffer is still sitting in fast memory. Each challenge carries a
nonce. From that nonce the prover derives thousands of unpredictable offsets,
reads the word at each one, and folds them all into a single chained hash. The
offsets aren't knowable until the nonce arrives, and every read changes the
result — so the prover can't return the right answer without actually reading the
data.

This is a proof-of-retention primitive: a spot check that someone still holds data
they claim to hold, using two signals — the **correctness** of the answer and the
**time** it takes to produce. It's a CPU precursor to a GPU/VRAM version aimed at
AI-compute governance: proving a datacenter's memory really holds the workload it
bills for.

The prover and the verifier share one definition of the challenge
(`src/common.hpp`). The prover answers; the verifier recomputes the same answer
locally and compares. Because both run the same code, a correct answer is
identical no matter where the prover's bytes came from — so only a wrong answer or
a slow one reveals a cheat.

## What it proves, and what it doesn't

- **Correctness detection is solid.** A prover that fabricates any withheld data is
  caught with probability `1 − (1−m)^k` (for `m=0.001, k=4096`, about 0.98). Tests
  confirm it: the `fake` prover fails every challenge; honest and disk-backed
  provers never do.
- **Timing tracks residency, not the storage device.** The buffer being "on disk"
  is not what makes a cheat slow; keeping it out of memory is. An adversary that
  leaves the data on disk but memory-maps it answers at honest speed, because the
  operating system's page cache is holding the file — the data genuinely is
  resident. Take that RAM away (budget the prover below the buffer size) and it
  collapses to hundreds of times slower, with about half its reads faulting to
  storage. So timing tells you whether the data is in fast memory right now, which
  is what the primitive cares about. See
  [ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md) for the
  measurements behind this.
- **The shortcuts around actually reading the data are each defended.** A prover can
  still try to regenerate the buffer from its seed, prefetch predictable offsets, or
  precompute answers to predictable challenges. Each has a defense you toggle on, and
  a test that shows the attack win with the defense off and lose with it on. See
  [Attacks and defenses](#attacks-and-defenses) below.
- **Two things are still open.** There's no decision rule yet — the harness reports
  how far apart the modes sit, not an accept/reject classifier with stated error
  rates. And the timing signal still needs a run on real hardware with a buffer
  larger than RAM to confirm it against a serious adversary; the current numbers
  come from a container whose memory limit stands in for physical RAM, so they're a
  lower bound. See [NEXT_STEPS.md](NEXT_STEPS.md).

## How a challenge works

A challenge is `(nonce, k, lanes)`. Seeded by the nonce, the prover draws `k`
pseudo-random offsets spread across the whole buffer, reads each word, and folds
them into a chained hash. Three properties make this bind to memory:

- **Unpredictable until issued.** The offsets come from the nonce, so nothing can
  be prepared before the challenge arrives.
- **Every read counts.** Because the hash is chained, changing or skipping any one
  word changes the final result. There's no shortcut that reads only part of the
  buffer.
- **The work is dominated by random-access reads.** `k` scattered reads across a
  large buffer. On this hardware about half the per-probe cost is the memory stall
  and half is the hashing itself.

`answer_lane_t` and `answer_challenge_t` in `src/common.hpp` are the single
definition of this math, called by the prover, the cheating prover, and the
verifier alike.

## Attacks and defenses

A cheating prover has several ways to look like it's holding the data without
actually holding it. `src/cheating_prover.cpp` implements each one as a mode, so the
defense is tested rather than asserted — run the attack, watch it win, turn the
defense on, watch it lose. The first group is caught by the two built-in signals
(a wrong answer, or a slow one). The last three are shortcuts that beat both signals
until you turn on a matching defense.

| `--mode` | What it tries | How it's caught |
|---|---|---|
| `honest` | Holds the whole buffer in locked RAM. The baseline every other mode is measured against. | — |
| `disk` | Leaves the data on disk and issues one read system call per probe. | Slow — timing. |
| `partial` | Keeps part of the buffer in RAM and the rest on disk. | Slow and bimodal — fast hits, slow misses — timing. |
| `fake` | Fabricates any word it doesn't hold. | Wrong answers — correctness. |
| `mmapdisk` | Leaves the data on disk but memory-maps it, so there's no per-probe system call. | Matches honest speed while the page cache holds the file (it really is resident), then runs ~840× slower once its RAM budget drops below the buffer — timing tracks residency, not the device. |
| `recompute` | Throws the buffer away and regenerates each probed word from the generator seed. Correct *and* fast, so it beats both signals. | `--gen chacha`: the buffer comes from a random 256-bit ChaCha20 key, so the seed can't be recovered and the regenerated words are wrong — correctness. |
| `prefetch` | Keeps the data on disk, derives all the offsets from the nonce up front, and prefetches those pages before answering — dodging the serial faults that make `mmapdisk` slow when starved. | `--chain dependent`: each offset depends on the previously read word, so the offsets can't be derived ahead of the reads, and the prover falls back to slow serial faults — timing. |
| `precompute` | Exploits predictable nonces: a background thread infers the coming nonces and computes their answers from disk in the gaps between challenges, then serves them instantly. | `--nonce random`: each nonce comes from a cryptographic random generator, so there's nothing to precompute and the prover must read during the timed window — timing. |

### Two ways to hand over the buffer

`--gen splitmix64` and `--gen chacha` **upload** the buffer: the verifier generates
every byte and sends them all. `--gen argon2d` **builds** it instead — the verifier
sends a 92-byte recipe and both sides construct the same buffer with Argon2d
(RFC 9106). Each 1 KiB block comes from the previous block and one earlier block
that the data itself selects, so a prover that discards blocks must rebuild a
cascade of ancestors to recover any one of them. That is what stops "hold the
buffer" and "regenerate on demand" from costing the same, which is the problem an
O(1) generator has once the prover holds the recipe.

The challenge math is identical either way — `answer_lane_t` reads `load(idx)` and
does not care how the buffer was filled — so every detection result above carries
over. A build-buffer run is checked by the same invariants: `fake` still fails
every challenge, the disk-backed cheats are still slow, and an honest prover's
independently built copy agrees with the verifier's on every answer.

One consequence is worth stating plainly: Argon2d cannot produce part of a matrix,
so **every prover must hold buffer-sized memory at least once, while building**. A
prover whose budget is below the buffer size cannot set up under `--gen argon2d`
at all — it fails during the build rather than answering slowly. That raises the
floor; it does not prove residency, because nothing stops a prover from freeing
the memory the moment the build finishes.

The last three defenses are verifier flags (`--gen chacha`, `--chain dependent`,
`--nonce random`), and they ship **on** by default — the verifier is secure out of
the box. To reproduce an attack winning, name the weak setting explicitly as a
benchmark opt-out: `--gen splitmix64 --chain independent --nonce sequential` (or
just the one you want to defeat). Selecting a defense isn't the same as it being
effective: `--chain dependent`, for example, only helps if each lane runs a deep
enough chain, so the verifier enforces `k / lanes ≥ 16` and refuses a shallower
config. The full reasoning is in
[ADR 0004](docs/decisions/0004-adversary-taxonomy-and-defenses.md) and
[ADR 0005](docs/decisions/0005-per-lane-chaining-vs-gpu-parallelism.md); the
measurements behind the `prefetch` and `mmapdisk` rows are in
[docs/investigations/](docs/investigations/).

## Verified memory pinning

The honest prover in `src/prover.cpp` maps an anonymous region, touches every
page, and locks it into RAM with `mlock`. It does not trust the return value of
`mlock`: it reads `VmLck` from `/proc/self/status` to confirm the lock took
effect. If the buffer isn't genuinely pinned, the prover refuses to run, because
an unpinned baseline is meaningless. Passing `--allow-unlocked` overrides this and
records `locked=false` in every output, so no result can quietly rest on an
unpinned baseline.

## Build and run

Everything builds and runs inside a linux/arm64 container (native on Apple
Silicon — emulation would ruin the timing). `run.sh` grants the memory-lock
privilege the honest prover needs and mounts `results/` back to the host.

```bash
./run.sh                              # build, then a smoke test
./run.sh bash tests/run_tests.sh      # known-answer test + invariants
# repeat trials, then plot:
./run.sh bash -c 'make && \
  python3 scripts/run_trials.py --repeats 10 --n 3000 --k 4096 --mb 64 \
      --modes floor,honest,disk,partial,fake,mmapdisk --out results/trials.json && \
  python3 scripts/plot.py results/trials.json'
```

### Live demo

Run `python3 demo/bridge.py` on the host, then open `http://127.0.0.1:8777`. Pick
a prover, give it a RAM budget, and watch it answer: a page map (blue for pages
served from memory, orange for pages faulted to storage, taken from the kernel's
own view), latency per challenge, and a log where `fake` gives itself away — fast,
and wrong every time. Every number comes from a real prover and verifier running
in containers; nothing is simulated. Tracing happens after each timed round trip,
so it doesn't distort the measurement, but the headline numbers come from the
terminal runs, not the demo.

### Supporting investigations

Two small experiments back the timing claim above. Each is a script plus a short
writeup under `docs/investigations/`: `disk-is-syscalls/` shows the disk penalty is
system-call overhead rather than storage distance, and `mmap-needs-ram/` shows the
memory-mapped cheat collapses once it can't cache the buffer. Run them to reproduce
[ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md).

## Results

Measured on Docker Desktop on Apple Silicon: a linux/arm64 VM limited to 1 vCPU
and 2 GiB, with a 64 MiB buffer, `k=4096`, and `lanes=32`. Absolute times are not
comparable across sessions — the VM pins each container to performance or
efficiency cores for its whole life, which swings absolute latency by about 1.6×.
Only within-session ratios are trustworthy, and every output file is stamped with
its session id.

Representative within-session numbers (illustrative µs; compare within a session
only):

| mode | p50 (µs) | ratio to honest | verify errors |
|------|---------:|----------------:|--------------:|
| transport floor (`k=0`) | ~47 | ~0.4× | 0 |
| honest (RAM, lock verified) | ~114 | 1× | 0 |
| disk (a read syscall per probe) | ~1850 | ~15× | 0 |
| fake (misses fabricated) | ~108 | ~0.95× | every challenge |
| mmap-on-disk, RAM to spare | ~122 | ~1.05× | 0 |
| mmap-on-disk, RAM below buffer size | ~81000 | ~840× | 0 |

What the table says:

- Correctness cleanly separates `fake` from everyone else: `fake` is wrong on every
  challenge, and the rest are always right.
- `fake` answers about as fast as honest, so timing alone is not a cheat detector —
  correctness is what catches fabrication.
- The memory-mapped adversary matches honest while it can cache the buffer, and
  blows up to about 840× once it can't. That gap is the residency signal.
- About 40% of the honest response time is TCP transport (the `k=0` floor), so
  memory time is reported as `honest − floor` with propagated error, never as a raw
  number.

The fuller analysis — why `fake` is slightly faster, how latency scales with `k`,
and the run-to-run variance — is in `docs/investigations/` and
[ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md).

## Toward a GPU version

The answer path is written to port cleanly to a GPU: a flat word buffer, no STL,
no exceptions. The buffer would live in VRAM through `cudaMalloc` (not swappable —
the GPU equivalent of `mlock`), `answer_lane_t` becomes the per-thread kernel body,
and the only non-portable line is the 128-bit multiply in `map_index`
(`((__uint128)r * n) >> 64` becomes `__umul64hi(r, n)`). `splitmix64` and `mix64`
are already identical. This is future work; see [NEXT_STEPS.md](NEXT_STEPS.md).

## Learn more

- [`CLAUDE.md`](CLAUDE.md) — orientation for anyone (human or agent) working in the
  repo: goal, layout, and the rules that keep results trustworthy.
- [Decision records](docs/decisions/) — why the challenge math is frozen, why the
  prover refuses to run unpinned, and what the timing signal does and doesn't show.
- [Investigations](docs/investigations/) — the question-script-result writeups that
  back those decisions.
- [NEXT_STEPS.md](NEXT_STEPS.md) — what's still open, and why it matters.
