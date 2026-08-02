# Low-latency memory-challenge PoC

A verifier sends a large random buffer to a prover, then repeatedly challenges
the prover to prove the data is still resident in fast memory. The **latency
profile** of the responses is the result: an honest prover holding the data in
RAM (or VRAM) answers with a low, tight distribution; a prover that dropped the
data, recomputes it, or let it swap to disk shows verification errors, a fat
tail, or both.

This is a proof-of-retention / memory-binding primitive — spot-checking à la
proof-of-retrievability, but with **timing** as the discriminator rather than
just correctness.

## Files
- `common.hpp` — wire protocol + the sampling/hashing core. The templated
  `answer_lane_t()` / `answer_challenge_t()` are the single source of truth;
  every prover (honest or not) and the verifier call them, so a *correct*
  answer is byte-identical to the verifier's local recomputation regardless of
  where the bytes were sourced.
- `prover.cpp` — honest TCP server. Receives the buffer into **pinned** memory
  and answers challenges.
- `cheating_prover.cpp` — dishonest server with selectable strategies (see
  "Detecting dishonest provers"), for contrast.
- `verifier.cpp` — TCP client. Generates + uploads the buffer, issues timed
  challenges, verifies each response, prints the latency profile, writes a CSV,
  and (optionally) runs a K-sweep.
- `plot_latency.py` — histogram + ECDF for one run.
- `plot_compare.py` — overlay ECDF across prover modes.
- `compare_modes.sh` — runs the verifier against every mode and tabulates.
- `Makefile`.

## Build & run
```bash
make
./prover 9099                 # terminal 1 (listens, serves connections in a loop)
./verifier --mb 256 --k 65536 --lanes 64 --n 2000   # terminal 2
python3 plot_latency.py latency.csv latency.png      # optional
```
Verifier flags: `--host --port --mb --k --lanes --n --warmup --sweep --csv --seed`.

## How the challenge binds to memory
On challenge `(nonce, k, lanes)` the prover must, using a PRNG seeded by the
nonce, draw `k` pseudo-random offsets spread across the **whole** buffer, read
the word at each, and fold them into a chained hash. Key properties:

- **Unpredictable until issued** — offsets derive from the nonce, so nothing can
  be precomputed before the challenge arrives.
- **Every probe matters** — the chained fold means dropping/altering any probed
  word changes the final hash, so the prover can't shortcut with a subset.
- **Memory-bound** — work is `k` random-access reads. If the data isn't in fast
  memory the reads become page faults / disk seeks and the tail explodes.

`lanes` partitions the `k` samples into independent streams. On CPU it's a plain
loop; it exists so the exact same math maps onto GPU threads (see below).

## Memory pinning (anti-swap)
The prover `mmap`s an anonymous region, prefaults every page, and `mlock()`s it
so the kernel may never swap it out; it also calls `mlockall(MCL_CURRENT |
MCL_FUTURE)`. If the process lacks privilege, `mlock` fails and it continues
**unlocked** after logging why. To guarantee locking:
```bash
ulimit -l unlimited            # or raise RLIMIT_MEMLOCK
# or run with CAP_IPC_LOCK, e.g.: setcap cap_ipc_lock+ep ./prover
```
For very large buffers, consider transparent/explicit huge pages
(`madvise(MADV_HUGEPAGE)` / `MAP_HUGETLB`) to cut TLB pressure.

## Path to GPU
The answer path is deliberately GPU-shaped — flat word buffer, no STL, no
exceptions:
- Buffer lives in device memory via `cudaMalloc` (VRAM is inherently
  non-swappable — the GPU analog of `mlock`). Stage uploads through pinned host
  memory (`cudaHostAlloc`) for fast H2D copies.
- `answer_lane()` becomes the per-thread kernel body; launch `lanes` threads,
  write partials to a scratch array, then reduce.
- The only non-portable line is the 128-bit multiply in `map_index()` — on CUDA
  replace `((__uint128)r*n)>>64` with `__umul64hi(r, n)`. `splitmix64`/`mix64`
  are already identical.

## Measured results (in-container: 1 vCPU, memory LOCKED, 128 MiB buffer)
End-to-end run: **0 verification errors**, prover memory reported **LOCKED**.

K-sweep (median round-trip latency, loopback TCP, single core):

| samples/challenge | median latency | throughput |
|------:|------:|------:|
| 1,024   |   34.6 us |  28 probes/us |
| 4,096   |  116.2 us |  34 probes/us |
| 16,384  |  410.0 us |  40 probes/us |
| 65,536  | 1478.6 us |  44 probes/us |
| 262,144 | 5863.2 us |  44 probes/us |

Latency is ~linear in `k` and throughput saturates near the memory-latency
ceiling — the signature of genuine random-access probing. On real hardware,
size `k` so the compute clears the transport floor (loopback TCP adds a few µs;
a NIC/RDMA path more), and pin CPU affinity for cleaner tails.

## Detecting dishonest provers
`cheating_prover --mode X` implements strategies an adversary might try. All of
them reuse the exact challenge math, so a correct answer still verifies — only
timing or correctness gives them away:

- `honest`  — full buffer pinned in RAM (baseline).
- `disk`    — buffer only on disk; every probe is a `pread()`. Correct, slow.
- `partial` — `--frac` in RAM, rest on disk; misses do `pread()`. Correct,
  **bimodal** — the tail grows as `--frac` shrinks. A timing detector.
- `fake`    — `--frac` in RAM, misses **fabricated**. Fast but wrong. A
  correctness detector.

Run the contrast:
```bash
make
./compare_modes.sh
python3 plot_compare.py            # -> latency_compare.png
```

Measured (in-container: 1 vCPU; 64 MiB buffer, k=4096, lanes=32, n=300, frac=0.5):

| mode | p50 | p99.9 | vs honest | verify errors |
|------|----:|------:|----------:|--------------:|
| honest  (RAM, pinned)        |  125 us |  224 us |  1x   | 0 / 300   |
| disk    (0% resident)        | 2344 us | 3078 us | ~19x  | 0 / 300   |
| partial (50% RAM / 50% disk) | 1317 us | 1572 us | ~10x  | 0 / 300   |
| fake    (50% RAM, faked)     |  114 us |  205 us |  1x   | **300 / 300** |

Two orthogonal detectors:
- **Timing** exposes `disk`/`partial`: they answer correctly but 10–19x slower,
  because probes hit storage instead of RAM. (These are page-cache `pread`s in a
  64 MiB container; on genuinely cold SSD/HDD the gap widens to ms-scale seeks.)
- **Correctness** exposes `fake`: it's as fast as honest but every challenge
  fails. The chained fold makes this unavoidable — if a prover withholds a
  fraction `m` of the data, a `k`-probe challenge catches it with probability
  `1 - (1-m)^k`. For `m=0.5, k=4096` that's ~1; even `m=0.001, k=4096` ≈ 0.98.
  Withholding even a sliver is caught with overwhelming probability.

See `latency_compare.png`: honest and `fake` overlap at the fast end (timing
can't separate them — hence the need for the correctness check), while `partial`
and `disk` sit one to two orders of magnitude to the right.

## Notes / next steps
- 64-bit responses are fine for a PoC correctness check; a production protocol
  wants ≥128-bit responses and a cryptographic PRF/hash so answers can't be
  guessed or precomputed, plus a fresh random nonce per challenge.
- Native byte order on the wire assumes same-endian peers (true on loopback).
- To *see* the dishonest tail, run the prover under memory pressure without
  locking (skip `ulimit -l`, allocate > RAM) and watch p99.9 diverge.
