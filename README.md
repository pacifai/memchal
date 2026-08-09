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
- **Timing does NOT measure storage distance; it measures residency.** The
  measured "disk is ~15× slower" signal is **pread syscall overhead, not storage
  distance** — the identical probe loop over a tmpfs (pure RAM, zero block I/O)
  costs ~94% as much as over the container disk (experiment 1). An **mmap
  adversary** (data on disk, read through a memory-mapped view, no per-probe
  syscall) answers at ~honest speed and passes correctness — but experiment 2
  shows *why*: with RAM to spare the page cache holds the whole file, so that
  adversary **is** resident, via the page cache instead of `mlock`. Give the same
  adversary a memory budget **smaller than the buffer it claims to hold** and it
  collapses to **~840× honest** with **~52% of its probes faulting through to
  storage**. So the timing signal tracks *whether the data is in fast memory*, not
  *what device it came from* — which is what the primitive actually cares about.
- **What is still unvalidated.** Experiment 2 constrains the page cache with a
  cgroup memory budget, not physical RAM on real storage, and Docker's disk sits
  on the macOS host's own cache — so its storage costs are a **lower bound**. A
  bare-metal >RAM run is still the settling experiment. See [NEXT_STEPS.md](NEXT_STEPS.md).
- **There is no decision rule yet.** This measures separations; it does not yet
  classify a prover with stated false-positive/false-negative rates.

## Repository layout

```
CLAUDE.md       agent entry point (goal, architecture, rules, open issues)
src/            common.hpp (challenge math — single source of truth), prover.cpp,
                verifier.cpp, cheating_prover.cpp
scripts/        mc_stats.py (stats + error propagation, single source),
                run_trials.py (interleaved repeat-trials harness),
                plot.py (self-describing chart), fmt_table.py (aligned output)
tests/          kat.cpp (known-answer test), run_tests.sh (KAT + invariants)
docs/decisions/       ADRs — why the harness/claims are what they are
docs/investigations/  analyses (question → script → results) backing the ADRs,
                      incl. disk-is-syscalls/ and mmap-needs-ram/
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
# the two discriminator experiments, in demo order:
./run.sh bash docs/investigations/disk-is-syscalls/disk_is_syscalls.sh   # 1: the penalty is syscalls
bash docs/investigations/mmap-needs-ram/run_mmap_needs_ram.sh            # 2: the mmap cheat needs RAM
```

Experiment 2 runs on the **host**, not via `run.sh`: it drives docker itself,
putting the prover in a container whose memory budget is smaller than the buffer
it claims to hold, and the verifier in a second container sharing the prover's
network namespace (same loopback path as experiment 1, separate memory
accounting). Knobs: `MB K LANES N WARMUP FIT_MEM STARVED_MEM CPUS`.

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
| mmapdisk (on disk, mmap'd), RAM to spare | ~122 | **~1.05× ± 0.02** | 0 / n |
| mmapdisk, prover RAM budget < buffer | ~81000 | **~840×** | 0 / n |

What the numbers say, checked against the six open questions:

1. **Is the RAM-vs-disk gap real?** Not as originally claimed — the *device* is
   not what the gap measures. The same pread loop over tmpfs (RAM, `read_bytes=0`)
   costs ~94% of the container-disk latency; the disk file is read from the block
   layer exactly **once** (one full sweep) then served from page cache. The ~15×
   is **syscall overhead**. What the timing *does* track is residency: run the mmap
   adversary with a memory budget of half the buffer and ~52% of its probes fault
   through to storage (verified by its own `read_bytes`/`majflt` counters), costing
   **~840× honest** — and a pread prover under the same budget lands within ~2% of
   it, because real storage work swamps the syscall difference from experiment 1.
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

**The headline retraction, and what replaced it.** The original README claimed
disk/partial are "10–19× slower because probes hit storage instead of RAM,"
presenting timing as a RAM-vs-storage detector. That specific claim is wrong: the
gap is **pread syscall overhead**, and it **vanishes under a one-line mmap
adversary** for a buffer the page cache can hold. What replaced it is narrower and
survives contact: **timing tracks residency, not device.** The mmap adversary is
fast precisely because the page cache is holding its file — it is resident, just
not via `mlock` — and when its memory budget cannot hold the buffer it claims, it
is ~840× honest with ~52% of probes going to storage (a pread prover on the same
budget is within ~2%). Two limits remain: the budget is a **cgroup limit standing
in for physical RAM**, and Docker's disk sits on the macOS host's cache, so the
storage penalty measured here is a **lower bound**. A bare-metal >RAM run is still
the settling experiment (NEXT_STEPS).

## What each detector actually catches

- **Correctness (robust).** `fake` answers fast but wrong; the chained fold makes
  withholding any fraction catchable with probability `1 − (1−m)^k`. This is the
  one claim that survives adversarial contact.
- **Timing (residency, not device).** Detects whether the probed data is in fast
  memory *right now*. An mmap adversary looks honest only because the page cache is
  holding its file (it really is resident); starve its RAM below the buffer and it
  is ~840× honest. What timing does *not* establish is the storage medium, or a
  real-RAM/real-storage figure — that needs the bare-metal >RAM run. Absolute
  numbers are non-portable (transport floor + P/E-core swing).

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
