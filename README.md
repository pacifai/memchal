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

## The GPU version

The GPU port **runs, and the correctness gate passes.** On an NVIDIA A40 the CUDA
kernel reproduced all six constants pinned before any CUDA existed — twice, once
over an uploaded buffer and once over one the GPU generated itself.

The answer path ported cleanly, as intended. `answer_lane_t` is now the
per-thread kernel body, and the only non-portable line was the 128-bit multiply
in `map_index`, which became `mulhi64()`. Three things were less obvious:

- **`common.hpp` had to be split first.** It mixed the challenge math with
  `<sys/socket.h>` and `/proc` readers, so it could not be included from a `.cu`
  file at all. The math now lives in `src/challenge.hpp`.
- **The cross-lane fold cannot be a tree reduction.** It is a sequential chain,
  so a warp-shuffle reduction would produce a different hash while looking
  perfectly reasonable.
- **`mlock` has no GPU equivalent, and the gap matters.** `cudaMallocManaged`
  returns a pointer that looks identical to `cudaMalloc`'s and migrates silently
  under pressure. The prover asks the driver what backs its buffer and refuses to
  serve unless the answer is device memory
  ([ADR 0008](docs/decisions/0008-gpu-residency-check-substitutes-for-mlock.md)).
  Like `VmLck`, that is self-reported: it stops the honest prover publishing a
  bad baseline and proves nothing about a remote one.

### Measured, on one H100 SXM, in one session

Clock locking is denied on a container provider, so the card's clocks wandered.
The ratios are meaningful; the absolute microseconds do not travel.

| mode | p50 | vs honest | verify errors | caught by |
|---|---:|---:|---:|---|
| honest | 208 us | 1.00x | 0 | — |
| `fake` | 175 us | 0.84x | **100/100** | correctness |
| `uvm`, buffer fits in free HBM | 653 us | 1.04x | 0 | **nothing — and correctly so** |
| `uvm`, no free HBM left | 3.49 s | **16786x** | 0 | timing |
| `zerocopy` (host DRAM over PCIe) | 5.95 ms | **28.6x** | 0 | timing |

4096 MiB buffer, k=262144, lanes=16384, chain depth 16, tree fold, defenses on,
n=50 per trial over two interleaved cycles. The `uvm`-fits row is the A40
figure; the rest are from the H100 session, where the reserve took free HBM to
zero rather than to just under the buffer size, which is why that row is
steeper than the A40's 4643x.

**The tree fold roughly doubled the timing detector's separation, for free.**
Running the same sweep with each fold on the same card: honest went from 401 us
to 208 us, while `uvm` (3.482 s to 3.489 s) and `zerocopy` (6.11 ms to 5.95 ms)
barely moved. The fold was a fixed host-side cost every mode paid, so removing
it shrinks the denominator and leaves the numerators alone — `uvm` 8674x to
16786x, `zerocopy` 15.2x to 28.6x. Separation was not the reason for the change
([tree-fold-payoff](docs/investigations/tree-fold-payoff/README.md) was about
scaling); it is a side effect worth knowing about, because any other fixed
overhead shared by honest and cheat is costing detector resolution the same way.

The `uvm` pair is worth reading twice. Managed memory that fits on the card
migrates there once and is genuinely resident, so passing it is a true negative
rather than a miss — the same shape as the CPU finding that a memory-mapped cheat
looks honest while the page cache holds its file. The cliff falls exactly where
free HBM stops being able to hold the buffer, with nothing in between: the
detector has no early warning. See
[uvm-needs-hbm](docs/investigations/uvm-needs-hbm/README.md).

Two honest limitations from the same session. Over half the response time is the
host-side lane fold (~312 us against the kernel's ~86 us at 16384 lanes), which
is what keeping the sequential chain correct costs. And useful-bytes bandwidth
came out near 25 GB/s against the A40's ~696 GB/s peak, because 8-byte probes
discard most of each memory transaction and a depth-16 dependent chain is
latency-bound rather than bandwidth-bound. Both are in
[NEXT_STEPS.md](NEXT_STEPS.md).

### Running it

From a laptop with no GPU, two commands do the whole thing:

```bash
bash scripts/gpu_demo_up.sh        # rent a card, ship, build, gate, open the demo
bash scripts/gpu_demo_down.sh      # pull results back, close it, release the card
```

`gpu_demo_up.sh` prints the hourly price and waits for a yes before renting. It
records the pod it created in `.gpu_demo_state`, which is the only pod the
teardown will touch, so neither script can release something you started by
hand. It fails closed if the known-answer gate does not pass: a card that
cannot reproduce the frozen constants must not produce measurements.
`gpu_demo_down.sh` refuses to release a pod whose results it could not copy.

Both need `RUNPOD_API_KEY` in your environment and an SSH key registered on
that RunPod account (`~/.ssh/neta-pacifai-runpod`, or set `MEMCHAL_SSH_KEY`).

Once a box is up, the individual steps are:

```bash
bash scripts/gpu_pod_setup.sh      # machine record, refusals, build, then the gate
bash scripts/gpu_adversaries.sh    # honest plus all three cheats, interleaved
python3 demo/bridge_gpu.py         # the live demo, on the GPU box
bash scripts/gpu_teardown.sh       # stop everything, check the card is idle, pack results
```

Run the teardown before you release the pod. It stops the bridge and any
prover still holding HBM, reports what is left on the card so "the box is idle"
is something you read rather than assume, and packs `results/` into one archive
with the command to pull it back. It does not terminate the pod: releasing
rented hardware is a decision, not a side effect of running a script.

`gpu_adversaries.sh` defaults to `--fold tree`. The verifier's own default is
`sequential`, because the tree fold changes the answers and has to be opted
into, so any command that measures the shipped design has to name it.

The GPU demo spawns processes directly rather than containers, because RunPod
runs the container for you and forbids nesting another. It shows HBM occupancy
where the CPU page shows a page map — there is no GPU `mincore()`, so per-page
residency simply is not available — and puts SM clock, temperature, and any
active throttle reason next to every measurement.

## Learn more

- [`CLAUDE.md`](CLAUDE.md) — orientation for anyone (human or agent) working in the
  repo: goal, layout, and the rules that keep results trustworthy.
- [Decision records](docs/decisions/) — why the challenge math is frozen, why the
  prover refuses to run unpinned, and what the timing signal does and doesn't show.
- [Investigations](docs/investigations/) — the question-script-result writeups that
  back those decisions.
- [NEXT_STEPS.md](NEXT_STEPS.md) — what's still open, and why it matters.
