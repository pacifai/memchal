# Investigation: is the prefetch adversary real, and does chaining stop it?

## Question
The `prefetch` cheat keeps the data on disk but, because the *independent* challenge
derives all `k` offsets from the nonce up front, enumerates them and
`MADV_WILLNEED`-batches the pages — dodging the serialized per-probe faults that make
`mmapdisk` slow. A PR review measured it in the **cached** regime and found it
*slower* than a plain `mmapdisk` reader, concluding it's a strawman whose own
overhead (a dry-run enumeration + a per-page `madvise` storm) swamps any benefit. Is
it a strawman — or was it just measured in the wrong regime? And does the
data-dependent (`--chain dependent`) defense actually remove its edge?

## Method
`run_prefetch_needs_pattern.sh` (runs on the **host** — launches its own containers).
The prover runs in a container with a fixed memory budget; the verifier runs in a
second container sharing its network namespace (separate memory accounting). We sweep
`mmapdisk` vs `prefetch`, `independent` vs `dependent`, under a **cached** budget
(1024 MiB ≫ 64 MiB buffer) and a **starved** budget (40 MiB < buffer).

## Finding
The reviewer is right about the *artifact*, wrong about the *attack*. Prefetch only
helps when there are cold faults to hide — i.e. under memory pressure. Representative
run (`results/run.txt`, illustrative µs, within-run only):

| regime | chain | mmapdisk | prefetch | |
|---|---|--:|--:|---|
| cached  | independent | 40 µs | 427 µs | prefetch **loses ~11×** — pure overhead, nothing to prefetch |
| cached  | dependent   | 211 µs | 588 µs | (chaining slows both; prefetch still loses) |
| starved | independent | 27,439 µs | **6,591 µs** | prefetch **wins ~4.2×** — the attack is real |
| starved | dependent   | 27,518 µs | 31,934 µs | defense works — the edge is gone |

- **Cached: strawman.** With the file already in page cache there are no faults to
  batch, so the enumerate-dry-run + `madvise`-storm is pure overhead and prefetch is
  ~10× slower than a plain `mmapdisk` read. This is the regime the first cut measured
  and (mistakenly) demoed — the app copy claiming prefetch "dodges serial faults" was
  false there.
- **Starved: real attack.** With the file too big to cache, `mmapdisk` pays serial
  synchronous major faults (~27 ms) while prefetch issues all the readahead async and
  the reads land warm (~6.6 ms) — a genuine ~4× win. The overhead is real but small
  next to the fault time it hides.
- **Dependent chaining defeats it.** In the starved+dependent case prefetch can't
  enumerate the offsets (they depend on the reads), so it prefetches the wrong pages
  and falls back to serial faults (~32 ms, no better than `mmapdisk`). Latency-hardness
  does its job — but only where the attack was a threat in the first place (starved).

## Limits
Same as `mmap-needs-ram`: the budget is a cgroup limit standing in for physical RAM,
and Docker's disk is re-cached by the macOS host, so the starved numbers are a **lower
bound**. The prefetch overhead itself (a `madvise` per unique page) could be cut with
range-coalesced `madvise`/`readahead` or `io_uring`, which would *widen* the starved
win — the attack shown here is a conservative version of it.

## Reproduce
```bash
bash docs/investigations/prefetch-needs-pattern/run_prefetch_needs_pattern.sh   # host
```

## Consequence
Corrected the claim: prefetch is a real timing attack **only under memory pressure**;
the demo must be run with a starved RAM budget to show it, and the app copy was fixed
to say so. The dependent-access defense is validated in the regime that matters. See
[ADR 0003](../../decisions/0003-timing-detector-scope-and-measurement.md).
