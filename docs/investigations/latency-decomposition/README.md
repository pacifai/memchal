# Investigation: where does a challenge's latency actually go?

## Question
The README called the work "memory-bound" and claimed throughput "saturates near
the memory-latency ceiling." Two things needed checking: (a) how much of a
measured challenge is TCP round trip vs actual work, and (b) is the per-probe work
really dominated by the DRAM read, or by the hash-chain arithmetic? Also: **why
does `fake` measure *faster* than `honest`?**

## Method
`decompose.sh`, all in one session:
- Sweep `k` on a **big 64 MiB** buffer (random access → DRAM-bound) and a **tiny
  64 KiB** buffer (fits L1 → reads are ~free). `k=0` gives the transport floor.
  Then: `ALU/probe = (tiny − floor)/k`, `total/probe = (big − floor)/k`,
  `DRAM stall/probe = total − ALU`.
- Compare honest (all DRAM reads) vs `fake --frac 0` (every probe fabricated in
  the ALU, zero memory traffic) at fixed `k`.

## Finding
- **Transport floor is large**, not "a few µs": ~45 µs in-container (≈15 µs
  native). At the k=4096 operating point that floor is ~40% of the honest number,
  so memory-time must be reported as `honest − floor` **with propagated error**
  (the floor itself has ~12% run-to-run variance).
- **Per-probe cost is roughly half ALU, half memory**: ~4 ns hash-chain ALU +
  ~7 ns DRAM stall (≈64% memory). "Memory-bound" is only half-true — the
  splitmix64/murmur fold is comparable to the DRAM latency.
- **Latency is ~linear in `k`** with mild upward curvature at large `k` (growing
  TLB/cache pressure) — consistent with genuine random probing.
- **`fake` is genuinely faster than `honest`, and it's not noise:** `fake --frac 0`
  does pure ALU with no memory reads and lands well below honest (illustrative:
  ~64 µs vs ~102 µs at k=4096). Fabrication is cheaper than a memory read; the
  original ~9% gap was small only because `fake --frac 0.5` skipped just half the
  reads on top of the shared transport floor. This is a benchmark-fairness point,
  not a property of memory — and it's why the timing signal alone can't separate
  `fake` from `honest` (the correctness detector does).

Full run in [`results/decomposition.txt`](results/decomposition.txt). Absolute µs
are within-session only (see `../../decisions/0003-*`).

## Reproduce
```bash
./run.sh bash docs/investigations/latency-decomposition/decompose.sh
```

## Consequence
Made the transport floor a first-class, repeated, error-propagated measurement in
the harness (the `floor` mode), and corrected the README's "memory-bound" framing.
See [ADR 0003](../../decisions/0003-timing-detector-scope-and-measurement.md).
