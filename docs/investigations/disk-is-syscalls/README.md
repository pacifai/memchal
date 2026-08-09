# Investigation: is the "disk" timing gap storage distance, or syscall overhead?

## Question
The prototype's headline claim was that `disk`/`partial` provers are "~10–19× slower
because probes hit storage instead of RAM," i.e. the timing detector measures
**RAM-vs-storage residency**. Is that true, or is the gap something else?

## Method
Three within-session comparisons (`disk_is_syscalls.sh`):
1. Run the disk mode (4096 `pread`s/challenge) with the backing file on the
   **container overlay disk** vs on a **tmpfs** (guaranteed RAM, no block layer).
2. Read `/proc/PID/io read_bytes` before/after to see how much data is actually
   fetched from the block layer.
3. Run the `mmapdisk` adversary: same on-disk file, but read through a
   memory-mapped view (no per-probe syscall). Compare to the honest RAM prover.

## Finding
`pread` over tmpfs (which *cannot* touch storage, `read_bytes = 0`) costs
**~94–100%** of the overlay-disk latency. The overlay disk fetches the file from
the block layer **exactly once** (one full sweep = file size), then serves every
subsequent probe from the page cache. And `mmapdisk` — data genuinely on disk,
read via mmap — answers at **~honest speed** (ratio ≈ 1.0×) with 0 verify errors.

**The "disk is ~15× slower" gap is `pread` syscall overhead, not storage
distance.** But `mmapdisk` looking honest is *not* a defeat of the detector: it is
fast because the page cache is holding its whole file — it really is resident, just
not via `mlock`. The follow-up, [mmap-needs-ram](../mmap-needs-ram/), takes that
RAM away and the same adversary goes ~840× honest. So timing tracks **residency**,
and this experiment only shows that *device* is the wrong way to read it.

Representative results (illustrative, one session — absolute µs are not portable,
see `../../decisions/0003-timing-detector-scope-and-measurement.md`): tmpfs ~1425 µs
≈ overlay ~1548 µs; mmapdisk ~106 µs ≈ honest ~103 µs. Full run in
[`results/disk_is_syscalls.txt`](results/disk_is_syscalls.txt).

## Reproduce
```bash
./run.sh bash docs/investigations/disk-is-syscalls/disk_is_syscalls.sh
```

## Consequence
Drove the README's retraction of the timing claim and the promotion of `mmapdisk`
to a first-class cheat mode. See [ADR 0003](../../decisions/0003-timing-detector-scope-and-measurement.md).
