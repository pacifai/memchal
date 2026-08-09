# Low-latency memory-challenge PoC

A verifier sends a large random buffer to a prover, then repeatedly challenges the
prover to prove the data is still resident in fast memory. On each challenge the
prover must derive thousands of unpredictable offsets from a nonce, read those
words across the whole buffer, and fold them into a chained hash. Because the
offsets aren't known until the nonce arrives and every probe affects the result,
you can't answer correctly without actually reading the data.

This is a proof-of-retention / memory-binding primitive — spot-checking à la
proof-of-retrievability, with **timing** and **correctness** as the two
discriminators. It is a CPU precursor to a GPU/VRAM version for AI-compute
governance (proving a datacenter's memory holds the workload it claims).

## What this PoC does and does NOT establish (read this first)

This repo was stabilized to make its numbers trustworthy. The honest summary:

- **Correctness detection is robust.** A prover that fabricates any withheld data
  is caught with overwhelming probability (`1 − (1−m)^k`; for `m=0.001, k=4096`
  ≈ 0.98). Verified by test: `fake` fails **every** challenge; `honest`/`disk`/
  `mmapdisk` never do.
- **Timing detection, as built, does NOT prove RAM-vs-storage.** The measured
  "disk is ~15× slower" signal is **pread syscall overhead, not storage
  distance** — the identical probe loop over a tmpfs (pure RAM, zero block I/O)
  costs ~94% as much as over the container disk. An **mmap adversary** (data on
  disk, read through a memory-mapped view, no per-probe syscall) answers at
  ~honest speed and passes correctness — **for a buffer that fits in RAM.
  Behaviour above RAM size is unmeasured** and is where timing might still have
  teeth. See [NEXT_STEPS.md](NEXT_STEPS.md).
- **There is no decision rule yet.** This measures separations; it does not yet
  classify a prover with stated false-positive/false-negative rates.

## Repository layout

```
CLAUDE.md       agent entry point (goal, architecture, rules, open issues)
src/            common.hpp (challenge math — single source of truth), prover.cpp,
                verifier.cpp, cheating_prover.cpp
scripts/        mc_stats.py (stats + error propagation, single source),
                run_trials.py (interleaved repeat-trials harness),
                plot.py (self-describing chart)
tests/          kat.cpp (known-answer test), run_tests.sh (KAT + invariants)
docs/decisions/       ADRs — why the harness/claims are what they are
docs/investigations/  analyses (question → script → results) backing the ADRs
results/        generated CSVs/plots (gitignored)
Dockerfile, run.sh, Makefile
```

Deeper docs: [`CLAUDE.md`](CLAUDE.md), the [decision records](docs/decisions/) and
the [investigations](docs/investigations/) that back them.

## Build & run

The code is built and run inside a **linux/arm64** container (native on Apple
Silicon — no emulation, which would destroy timing fidelity). `run.sh` grants the
lock privileges the honest baseline needs and mounts `results/` to the host.

```bash
./run.sh                       # build + smoke test
./run.sh bash tests/run_tests.sh                          # KAT + invariant tests
# repeat-trials (interleaved cycles) + plot:
./run.sh bash -c 'make && \
  python3 scripts/run_trials.py --repeats 10 --n 3000 --k 4096 --mb 64 \
      --modes floor,honest,disk,partial,fake,mmapdisk --out results/trials.json && \
  python3 scripts/plot.py results/trials.json'
./run.sh bash docs/investigations/disk-is-syscalls/disk_is_syscalls.sh   # disk-is-syscalls proof
```

Running natively on real Linux hardware is the same commands without the VM (see
NEXT_STEPS — that is the environment that would settle the storage-physics and
core-affinity questions this VM cannot).

## How the challenge binds to memory

On challenge `(nonce, k, lanes)` the prover, using a PRNG seeded by the nonce,
draws `k` pseudo-random offsets across the **whole** buffer, reads each word, and
folds them into a chained hash (`answer_lane_t` / `answer_challenge_t` in
`src/common.hpp` — the single source of truth called by prover, cheating prover,
and verifier alike, so a *correct* answer is byte-identical regardless of where
the bytes came from).

- **Unpredictable until issued** — offsets derive from the nonce; nothing can be
  precomputed before the challenge arrives.
- **Every probe matters** — the chained fold means altering/withholding any word
  changes the final hash, so no subset shortcut.
- **Memory-bound work** — `k` random-access reads. On this hardware roughly half
  the per-probe cost is the DRAM random-access stall and half is the hash-chain
  ALU (measured, see below); "memory-bound" is only half-true.

## Memory pinning (verified, fail-loud)

`prover.cpp` mmaps an anonymous region, prefaults every page, and `mlock()`s it.
Crucially it does **not trust `mlock`'s return value** — it verifies the lock
took effect by reading `VmLck` from `/proc/self/status`. If the buffer is not
genuinely pinned, the prover prints a FATAL banner and **refuses to run** (an
unpinned honest baseline is meaningless). `--allow-unlocked` overrides this,
prints a prominent UNLOCKED banner, and records `locked=false` all the way into
the JSON/CSV/plots so no result can silently rest on an unpinned baseline.

## Measured results

**Environment.** Docker Desktop on Apple Silicon (6 performance + 4 efficiency
cores); a linux/arm64 VM (kernel 6.10), **4 KiB pages inside the VM** (the host
is 16 KiB), container limited to **1 vCPU / 2 GiB**. Buffer 64 MiB, `k=4096`,
`lanes=32`, `n≥1500` samples/run, results aggregated over R **interleaved
cycles** (every mode measured once per cycle in randomized order, so slow drift
cancels in within-session comparisons).

**Critical caveat — absolute latencies are not portable across sessions.** The
VM's vCPUs land on performance vs efficiency cores, fixed for a whole container
session, producing a **~1.6× swing** in absolute latency between sessions (a long
single session is stable — placement does not migrate mid-run). macOS gives no
affinity control. **Only within-session ratios and paired differences are
trustworthy;** every CSV/plot is stamped with its session id and this caveat.

Representative within-session results (illustrative absolute µs from one session;
compare within-session only):

| mode | p50 (µs, illustrative) | ratio to honest | verify errors |
|------|-----------------------:|----------------:|--------------:|
| transport floor (`k=0`) |   ~47 | ~0.4× | 0 |
| honest (RAM, mlock-verified) | ~114 | 1× | 0 / n |
| disk (pread per probe) | ~1850 | **~15× ± 0.2** | 0 / n |
| fake (misses fabricated) | ~108 | **0.95× ± 0.01** | **n / n** |
| mmapdisk (on disk, mmap'd) | ~122 | **~1.05× ± 0.02** | 0 / n |

What the numbers say, checked against the six open questions:

1. **Is the RAM-vs-disk gap real?** No — it's an artifact. The same pread loop
   over tmpfs (RAM, `read_bytes=0`) costs ~94% of the container-disk latency; the
   disk file is read from the block layer exactly **once** (one full sweep) then
   served from page cache. The ~15× is **syscall/page-cache overhead**.
2. **Why is `fake` faster than `honest`?** Genuinely, not noise. `fake --frac 0.5`
   fabricates half its probes (pure ALU, no memory traffic) over a smaller
   resident buffer; `fake --frac 0` (no memory at all) is faster still but only
   ~33% — memory access is real but **not dominant**.
3. **How much is TCP?** A lot at the fast end. The `k=0` transport floor is ~40%
   of honest in-container (and ~15 µs natively). Memory-time is reported as
   `honest − floor` **with propagated error**, never as a bare number — the floor
   itself has ~12% run-to-run variance.
4. **Are the modes separable given real variance?** Yes for the correctness split
   (`fake` errors = n, always) and for disk-vs-honest (~15×, non-overlapping).
   `fake`-vs-`honest` on *timing* is a clean but tiny ~5% separation — not a
   detector.
5. **Linear in k?** Yes, ~linear with mild upward curvature at large k
   (per-probe ~9.6 → 11.1 ns as k grows) — consistent with genuine random
   probing plus growing TLB/cache pressure.
6. **VM tail noise?** The dominant environmental effect is the ~1.6× P/E-core
   between-session swing above; within a session, per-run p50 is tight (sd ~1–3%).

**The headline retraction.** The original README claimed disk/partial are "10–19×
slower because probes hit storage instead of RAM," presenting timing as a
RAM-vs-storage detector. Measurement shows that gap is **pread syscall overhead**,
and it **vanishes under a one-line mmap adversary** (`mmapdisk` ≈ honest) for a
fits-in-RAM buffer. The timing signal, as built, distinguishes *per-probe
syscalls* from *in-process loads* — not memory residency. It is **not validated as
a RAM-vs-storage detector**; whether it has any teeth requires a buffer larger
than RAM on real storage (NEXT_STEPS).

## What each detector actually catches

- **Correctness (robust).** `fake` answers fast but wrong; the chained fold makes
  withholding any fraction catchable with probability `1 − (1−m)^k`. This is the
  one claim that survives adversarial contact.
- **Timing (weak, as built).** Separates syscall-per-probe provers from
  in-process-load provers. Defeated by an mmap adversary for fits-in-RAM buffers.
  Depends heavily on transport floor and hardware; absolute numbers non-portable.

## Path to GPU

The answer path is deliberately GPU-shaped — flat word buffer, no STL, no
exceptions. Buffer would live in VRAM via `cudaMalloc` (inherently non-swappable,
the GPU analog of `mlock`), `answer_lane_t()` becomes the per-thread kernel body,
and the only non-portable line is the 128-bit multiply in `map_index()` (replace
`((__uint128)r*n)>>64` with `__umul64hi(r, n)`). `splitmix64`/`mix64` are already
identical. This is future work; see NEXT_STEPS.

## Next steps

See [NEXT_STEPS.md](NEXT_STEPS.md): the **recompute adversary** (beats both
detectors), a **genuinely-cold-read test on bare metal** (now load-bearing, not
optional — it's the only thing that would validate the timing detector), the
**mmap adversary above RAM size**, a **decision rule with FP/FN rates**, and the
**CUDA port**. Protocol hardening (≥128-bit responses, a cryptographic PRF, fresh
per-challenge nonces) is also required before this is more than a PoC.
