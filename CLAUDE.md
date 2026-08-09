# CLAUDE.md — memchal (memory-challenge PoC)

Context for any agent working in this repo. Read this first, then the linked docs
as needed. Keep this file current when you change how the project works.

## What this is
A **proof-of-retention** primitive: a verifier uploads a large random buffer to a
prover, then issues timed nonce challenges. Each challenge forces the prover to
read thousands of nonce-derived offsets across the whole buffer and fold them into
a chained hash — you can't answer correctly without actually reading the data. Two
detectors emerge: **correctness** (a prover that fabricates data answers wrong) and
**timing** (a prover whose data isn't in fast memory answers slow).

It is the **CPU precursor** to a GPU/VRAM version for AI-compute governance
(proving a datacenter's memory holds the workload it claims). The GPU port is
future work; the answer path is deliberately GPU-shaped (flat buffer, no STL).

## Status — what is and isn't trustworthy (important)
This repo has been **stabilized** (results made trustworthy, repo made legible). No
new capability was added. Current honest position:
- ✅ **Correctness detection is robust.** `fake` fails every challenge; catch
  probability `1 − (1−m)^k`. This is the claim that survives adversarial contact.
- ⚠️ **Timing tracks residency, not storage distance.** The "disk is ~15× slower"
  gap is `pread` **syscall overhead**; an **mmap adversary** looks honest only
  because the page cache holds its file (it *is* resident). Starve its RAM below
  the buffer and it is **~840× honest** (~52% of probes fault to storage). Not yet
  validated: a real-RAM/real-storage figure (needs a bare-metal >RAM run — the
  cgroup/host-cache result is a lower bound).
  See [ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md).
- ⚠️ **Absolute latencies are not portable across sessions** (Apple-Silicon P/E-core
  swing, up to ~1.6×, load-dependent). Only within-session ratios/differences are
  trustworthy. See [ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md).
- ❌ **There is no decision rule yet** (no classifier with FP/FN rates).

## Architecture / current implementation
- `src/common.hpp` — wire protocol + the challenge math (`answer_lane_t` /
  `answer_challenge_t`, `splitmix64`, `mix64`, `map_index`, `fill_random`).
  **Single source of truth**, called by prover, cheating prover, and verifier.
  **Frozen** — do not modify; pinned by a KAT ([ADR 0001](docs/decisions/0001-challenge-math-frozen-pinned-by-kat.md)).
- `src/prover.cpp` — honest TCP server; mmap+prefault+`mlock`, **verifies the lock
  (VmLck) and refuses to run if unpinned** ([ADR 0002](docs/decisions/0002-verify-and-refuse-on-mlock.md)).
- `src/verifier.cpp` — client; generates the buffer (`fill_random`, seeded),
  uploads it, times challenges, verifies answers, emits **JSON** (`--json`) + CSV.
- `src/cheating_prover.cpp` — adversaries: `honest|disk|partial|fake|mmapdisk`.
- `scripts/mc_stats.py` — single source for stats + error propagation.
- `scripts/run_trials.py` — the harness: interleaved cycles, `floor` (k=0) mode,
  ratios/paired-diffs with propagated error, drift detection, session-stamped output.
- `scripts/plot.py` — one self-describing chart from `trials.json`.
- `tests/` — `kat.cpp` (pins the math) + `run_tests.sh` (KAT + invariants).

## Build / run / test (everything goes through the container)
```bash
./run.sh                                   # build + smoke test
./run.sh bash tests/run_tests.sh           # KAT + invariants (expect 5/5)
./run.sh bash -c 'make && \
  python3 scripts/run_trials.py --repeats 10 --n 3000 --k 4096 --mb 64 \
     --modes floor,honest,disk,partial,fake,mmapdisk --out results/trials.json && \
  python3 scripts/plot.py results/trials.json'
```
`run.sh` builds a linux/arm64 image and runs with `--cap-add=IPC_LOCK
--ulimit memlock=-1`, a tmpfs at `/ram`, explicit `--cpus`/`--memory`, and mounts
`results/` to the host. Binaries build to the repo root from `src/`.

## Run environment (and its limits)
Docker Desktop on Apple Silicon → a **linux/arm64 VM** (kernel 6.10). **4 KiB
pages in the VM** (host is 16 KiB). **Storage is virtualized** (overlay/tmpfs
through the host page cache) so it **cannot measure real storage physics**. vCPUs
land on P/E cores per session → absolute-latency swing. What it *can* measure
reliably: correctness, k-scaling shape, the transport floor, run-to-run variance,
and within-session mode ratios. A bare-metal Linux rerun is one command away and is
what would settle storage-physics and affinity (see `NEXT_STEPS.md`).

## Rules for agents working here
- **Never modify the challenge math** in `common.hpp` (frozen; KAT will fail). If a
  change is truly intended, add new KAT vectors and say so loudly.
- **Compare within-session only.** Never present absolute µs as portable; use the
  harness's ratios / `honest − floor` with propagated error.
- **Don't tune parameters to make results look better.** A cheat that isn't caught
  is a finding. Report negative/nuanced results plainly (see the session-variance
  investigation, where a rerun showed 1.05× instead of the earlier 1.6×).
- **Keep the honest-baseline lock guarantee** — don't weaken the verify/refuse path.
- **Stay buildable with g++ + stdlib + python3/matplotlib.** Ask before adding deps.
- Investigations/experiments that aren't core PoC go under `docs/investigations/`
  (doc + script + results), not mixed into `src/` or `scripts/`.

## Open issues to resolve (see NEXT_STEPS.md for detail)
1. **Recompute adversary** — regenerates probed words from the PRNG seed; beats
   *both* detectors. The most important gap; needs a memory-hard derivation.
2. **Genuinely-cold-read validation** on bare metal (buffer > RAM and/or O_DIRECT)
   — the only thing that would validate any timing teeth. Load-bearing.
3. **Decision rule** with FP/FN rates — there's no classifier yet.
4. **mmap adversary above RAM size** — cheap falsification of whether timing has a
   viable regime.
5. **Bare-metal / affinity rerun** for portable absolute numbers.
6. **CUDA port**; protocol hardening (≥128-bit responses, crypto PRF, fresh nonces).

## Map of the repo
- `README.md` — human-facing overview + honest results section.
- `NEXT_STEPS.md` — out-of-scope work with rationale.
- `docs/decisions/` — [ADRs](docs/decisions/README.md): why the harness/claims are what they are.
- `docs/investigations/` — [analyses](docs/investigations/README.md) (question → script → results) that back the decisions.
- `src/`, `scripts/`, `tests/` — the core PoC.
