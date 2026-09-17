> **Dated analysis — not current state.** Written 2026-08-16 against commit
> `4da2d55`. Most of the open issues below have since been closed in the repo;
> see the status note at the end. For current state read
> `memchal/CLAUDE.md`, `memchal/NEXT_STEPS.md`, and
> `memchal/docs/open-questions.md`.

# memchal — state of the repo as of 2026-08-16

Read of the memchal repo at commit `4da2d55`. This superseded `CLAUDE.md` and
`README.md` in the repo, both of which were one generation behind the code at
the time.

## What existed then (beyond what CLAUDE.md/README described)

Three commits landed after the "stabilization" phase the repo docs described:

- `13f50c3` — threat-model docs: recompute vs prefetch, capacity- vs
  latency-hardness, Model 1 vs Model 2.
- `79c996f` — **recompute adversary** (`--mode recompute`) + **ChaCha20
  keyed-stream generator** (`src/chacha20.hpp`, `--gen chacha`) as the
  capacity-hardness defense.
- `4da2d55` — **prefetch adversary** (`--mode prefetch`) + **data-dependent
  chained access** (`--chain dependent`, `kFlagChained`) as the
  latency-hardness defense.

Also present and undocumented in README/CLAUDE.md: the browser demo
(`demo/bridge.py` + `demo/static/`), which drives real containers via docker.

New wire/API surface: `kFlagChained = 2` in `Challenge.flags`; `Gen` enum and
`fill_buffer()` in `common.hpp`; `answer_lane_t`/`answer_challenge_t` take a
`chained` bool (default false, so old known-answer vectors stay bit-identical).

## Claim status at the time

| Claim | Status |
|---|---|
| Correctness detector catches fabrication | Solid. `fake` fails every challenge. |
| Timing tracks residency, not storage distance | Solid, with the cgroup/host-cache caveat documented in ADR 0003. |
| Recompute beats both detectors vs splitmix64 | **Verified.** Measured p50 64 µs vs honest 122 µs, 0/100 errors, ~0 bytes held. |
| ChaCha20 generation defeats recompute | True *for the adversary implemented*, but the defense is **seed secrecy, not capacity-hardness** — see issue 4. |
| Chained access defeats prefetch | **Not established** then. The implemented prefetcher was strictly worse than plain mmapdisk even in the mode it was supposed to win. |

## Open issues raised in the 2026-08-16 review

1. Chained defense strength = chain depth `k/lanes`; at `lanes >= k` it is
   completely void and nothing warns. Measured leak to a naive pre-enumerator:
   0.8% at lanes=32/k=4096, 25% at lanes=1024, 100% at lanes=k.
2. Direct tension with the GPU port: GPU wants many lanes, the defense wants
   deep chains. Same knob, opposite directions.
3. The prefetch adversary pays ~800 µs/challenge of `std::sort` + dedup +
   per-page `madvise` inside the timed window — 7.4x slower than plain mmapdisk
   in the cached regime. Any "chaining wins" result is confounded by that
   overhead.
4. ChaCha20 is counter-mode, so it gives O(1) random access. A key-knowing
   recompute adversary answers in ~263 µs/challenge holding zero bytes.
   Capacity-hardness rests entirely on seed secrecy, and the seed is 64-bit with
   a hardcoded default (`0xABCDEF0123456789`).
5. **Predictable nonces** (`nonce_base + i`, single `mt19937_64` draw) enable an
   offline precompute adversary that beats both detectors and that neither new
   defense addresses.
6. Chained honest path is ~3.5x slower than independent (429 vs 122 µs at
   k=2048) — the defense costs the honest prover its memory-level parallelism.
7. `scripts/run_trials.py` has no `--gen`/`--chain` and no `recompute`/
   `prefetch` modes, so the two new defenses cannot be measured by the harness at
   all, only by pass/fail invariant tests.
8. No `docs/investigations/` entry and no ADR for either new defense.
9. Test gaps: no honest + `--gen chacha` round-trip invariant; no known-answer
   test pinning the seed-to-ChaCha-key derivation.
10. Demo: `recompute` and `prefetch` modes never emit `--trace` events, so the
    residency panel is blank for exactly the two modes the demo was extended for.

## Status of those issues as of 2026-09-10

Checked against the repo, not against later documents.

| # | Status |
|---|---|
| 1 | Closed by ADR 0005 and the `kMinChainDepth = 16` guard in `common.hpp`. |
| 2 | Dissolved at GPU sizing — see `2026-08-16-gpu-port-plan.md` §0. |
| 3 | Closed by the `prefetch-needs-pattern` investigation, which separates the cached and starved regimes. |
| 4 | Closed as *scoped* by ADR 0006 and reopened properly by ADR 0007 (memory-hard generator). Still the live design question. |
| 5 | Closed by `--nonce random` and invariant tests 9/10. |
| 6 | Confirmed and recorded; reopens at the GPU port per ADR 0005. |
| 7 | **Closed.** `run_trials.py` now carries `--gen`, `--chain`, `--nonce` and the recompute/prefetch/precompute modes. The GPU port plan still calls this "the real blocker" — that line is stale. |
| 8 | Closed: ADRs 0004-0007 and five investigations now exist. |
| 9 | Closed by `tests/run_tests.sh` invariants and `known_answer_argon2d.cpp`. |
| 10 | **Closed.** `cheating_prover.cpp` now emits `Trace` events for `recompute`, `prefetch` and `precompute`, so the demo's residency panel is populated for all three. |

## Environment notes

The repo builds and runs natively on plain Linux with `make` + `g++ -O3
-std=c++17 -Isrc`; `./prover --allow-unlocked` works without CAP_IPC_LOCK.
Docker is only needed for the memory-budget experiments and the demo bridge.
