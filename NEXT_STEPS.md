# Roadmap

Pending work, grouped by phase and ordered by what unblocks what. Each item is
actionable and carries a "done when" line so it's clear when to check it off.

Four kinds of thing live in different places:

- **Work to do** — this file.
- **Decisions to agree** — an [ADR](docs/decisions/) with `Status: proposed`,
  promoted to `accepted` once agreed.
- **Questions waiting on someone else** — [open questions](docs/open-questions.md).
- **Analyses** — the [investigations](docs/investigations/) for measurements, and
  `docs/analysis/` for the dated reasoning documents.

## Now — the two CPU commits that block the port

Neither needs a GPU. Both are on the critical path regardless of how the GPU
numbers come out, and they are small.

- [ ] **Split `common.hpp`.** `challenge.hpp` gets the math (`splitmix64`,
  `mix64`, `map_index`, `answer_lane_t`, `answer_challenge_t`, and the
  `chacha20.hpp` include) with zero POSIX includes; `net.hpp` gets the sockets
  and the `/proc` readers. Today `common.hpp` pulls in `<sys/socket.h>`,
  `<netinet/in.h>` and friends, so it cannot be included from a `.cu`
  translation unit at all. Add an `MC_HOST_DEVICE` macro that expands to
  `__host__ __device__` under `__CUDACC__` and to nothing otherwise. *Done when:*
  the known-answer test passes unchanged after the split, and `challenge.hpp`
  compiles with no POSIX headers in scope.
- [ ] **`mulhi64()` behind that macro.** `__umul64hi(r, n)` on device, the
  existing `(unsigned __int128)` expression on host. The two were checked
  bit-identical across 2x10^7 random pairs plus all 64 boundary combinations, so
  this is bookkeeping, not risk — but it has to be pinned. *Done when:* the
  helper replaces the inline expression in `map_index`, and the known-answer test
  covers it so a future edit can't silently diverge.

> Open issue #7 from the 2026-08-16 review — `run_trials.py` unable to measure
> the defenses — is **closed**. The harness now carries `--gen`, `--chain`,
> `--nonce` and the recompute/prefetch/precompute modes.

## Next — the compression frontier

A cheating prover that stores its buffer **compressed** and decompresses on demand
when probed. This is the open question named independently by two external
advisors, and it is not blocked on anything below — it can run against today's
upload path.

The thing to understand about this adversary is that it is not trying to beat the
honest prover, and it knows it cannot: decompressing a block is strictly more work
than reading bytes that are already resident. It is trying to stay *close enough*
that the gap disappears into the noise floor, while holding materially fewer
bytes. **Its win condition is indistinguishability, not speed.** What it buys is
free memory — floor space it can rent out while still passing the challenge.

That makes the noise floor the real opponent, and this environment's is wide:
about 40% of an honest response is TCP transport, and absolute latency swings
~1.6x between sessions
([ADR 0003](docs/decisions/0003-timing-detector-scope-and-measurement.md)). The
cheat only has to hide inside that.

Why this matters more than it first looks: the memory-hard buffer below is Argon2d
output, so it is pseudorandom and compresses to nothing — against the primitive's
own filler this attack buys exactly zero. But what memchal ultimately wants to
prove residency of is **model weights**, which are structured, often quantized,
and genuinely compressible. The attack is dead where it is easiest to measure and
live exactly where it counts. So an experiment has to use a buffer with *tunable,
realistic* compressibility; run it against the generator's output and it returns a
reassuring null result that means nothing.

**Aim it at the KV cache, not the weights.** Hardness training exists to make
weights high-entropy, so if that works, DFloat11's ~30% gain on weights goes to
roughly zero and the weight half of this problem belongs to L2, not to memchal.
The KV cache is runtime-generated, can't be made incompressible by construction,
and dominates memory at long context. That is the live exposure.

- [ ] **Measure the frontier on CPU.** A `compressed` cheating-prover mode (hold
  the buffer compressed, decompress the containing block per probe, with a small
  decompressed-block cache) plus a verifier knob for buffer compressibility.
  *Done when:* there is a measured curve of **memory saved** against **timing
  separation from honest**, swept over compressibility and cache size, with the
  indistinguishable region stated plainly in ratio terms with propagated error.
- [ ] **Then the GPU version (the external advisor's A2).** Build `r(U)` against
  `t_decompress(U)`: nvCOMP at tunable chunk size for the cheap sweep, DFloat11
  for the realistic point. Use a **persistent kernel decompressor** spinning on a
  mailbox — a launch-per-challenge implementation hides behind the ~3-10 µs launch
  floor and produces a flattering result that won't survive review. One GPU,
  under $10, no NVLink, no RDMA. *Done when:* the curve exists and the question
  "does any compression unit U leave a ratio worth having while sitting under the
  jitter?" has a plotted answer.

The deliverable is **a measured frontier, not a pass/fail.** "Compression is
defeated" is not an available answer. "A prover can hold X% fewer bytes while
staying inside the noise floor" is.

Build the strong adversaries, not the naive one, in this order: persistent-kernel
decompressor, then a decompressed hot cache (which converts a timing test into a
Bernoulli one), then partial compression of a fraction *p* (which sets the
detector's resolution).

Untested hypotheses for what would move the frontier, to be checked rather than
assumed: raising `k` (decompression cost scales with probes, transport does not),
attacking the transport floor itself (it is the largest noise term), and timing
individual probes rather than whole challenges.

## Then — proving residency of the real workload

Today the challenge proves you're holding a specific random buffer the verifier
sent. The goal is to prove you're holding something tied to the actual workload —
not generic filler — while keeping friction with model providers low.
[ADR 0004](docs/decisions/0004-adversary-taxonomy-and-defenses.md) explains why
the send-buffer design doesn't get there.

The plan is a **memory-hard, weight-bound generator** (the build-buffer design):
ship a short recipe, and both sides build a dataset from it that is expensive to
regenerate on demand (so the prover must store it) and tied to the model. The
memory-hardness falls on the one-time *generation*, not the per-challenge answer,
so it does **not** multiply the per-challenge cost.

The generator itself is built: `src/argon2d.hpp` is vendored Argon2d (RFC 9106),
pinned against RFC vectors by `tests/known_answer_argon2d.cpp`, and `bench_build`
reports what a block costs. `mc::build_buffer()` hands back the Argon2d matrix as
the challenge buffer. What remains is wiring it in and attacking it.

- [ ] **Ship the recipe handshake.** Replace the buffer upload with a small
  `Recipe` message (key, salt, `m`/`t`/`p`, reserved workload tag) behind a wire
  version bump, and add build paths to the prover (build inside the
  already-`mlock`ed region) and the verifier (build its own reference copy). Keep
  the send-buffer path working — every existing investigation, result, and the
  demo ride on it. *Done when:* a build-buffer challenge runs end to end against
  the honest prover with zero verify errors, and the existing invariant suite
  still passes unchanged.
- [ ] **Attack it: a `partial-recompute` mode.** Hold fraction `f` of the blocks
  and genuinely rebuild missing ancestors on demand, with a give-up cap. *Done
  when:* it loses on timing against the defended generator, an invariant pins
  that, and its TMTO curve fixes the cost parameters (`m`, `t`, `p`) that ADR 0007
  needs to move from proposed to accepted.
- [ ] **Then re-check the cheap wins.** Our cheat keeps a random fraction; a real
  one studies the dependency graph offline and keeps the blocks costliest to
  rebuild. Add a `--keep hot` policy once the random curve exists, and report
  whether it moves the parameters.

> Not in scope, noted for the future: **challenging the weights directly** (probe
> a function of the weights the prover already holds for inference) would avoid a
> separate buffer entirely, but it needs the provider to expose or cooperate on
> the weights. Revisit only if that constraint changes — and note that under
> hardness training the weights are a declared public model, which would let the
> verifier hold a Merkle tree over expected bytes and check a pulled block
> directly. That path retires the seed-secrecy and predictable-nonce problems by
> making them irrelevant.

## Then — the GPU port

The port is not "compile `answer_lane_t` with `nvcc`." The answer path is already
GPU-shaped. What changes is the **challenge sizing**, because the discriminator
changes: on CPU the detector is latency-based; on a GPU node it has to be
**bandwidth**-based, which is a different-shaped challenge (~10^8 probes over
~10 GB in ~1 ms, not ~10^3 probes in ~100 µs). Ported as-is with 8-byte probes the
honest prover is *slower* than an NVLink peer and the protocol inverts, so probe
granularity goes to 128 B.

Background and the full derivation:
[`docs/analysis/2026-08-16-gpu-port-plan.md`](docs/analysis/2026-08-16-gpu-port-plan.md).
Provider specifics:
[`docs/analysis/2026-09-10-runpod-setup.md`](docs/analysis/2026-09-10-runpod-setup.md).

- [ ] **Write `scripts/preflight.sh`.** Both GPU documents assume it exists; it
  doesn't. It must check `nvidia-smi topo -m` for `NV#` rather than `PIX`/`SYS`,
  `cudaDeviceCanAccessPeer`, clock-lock permission, MIG/MPS sharing, ECC state,
  negotiated PCIe width, other tenants on the card, Xid errors, and — for the RDMA
  rows — `ls /dev/infiniband`, `ibv_devinfo`, `lsmod | grep nvidia_peermem`.
  *Done when:* it runs as the first command on a rented box, writes a machine
  metadata record next to the trial output, and exits non-zero on a box whose
  results would be meaningless.
- [ ] **Pod A — the go/no-go box** (1x H100 or A100 SXM, Secure Cloud, on-demand,
  `nvidia/cuda:*-devel-*`, ~1 hour, ~$3). Run `preflight.sh`, then the CPU-GPU
  known-answer bit-identity check, then **eta**, then a full `nvbandwidth` run as
  the calibration baseline. eta is a standalone ~100-line chained 128 B
  random-gather kernel over a >=5 GB buffer (H100 L2 is 50 MB — go smaller and
  cache hits inflate the number), achieved GB/s over `nvbandwidth`'s
  device-to-device local figure. Wall clock only; no performance counters needed,
  which is what keeps this on a container provider. *Done when:* eta is measured
  with its variance, and the known-answer hashes match the CPU prover bit for bit.
- [ ] **Read eta against the break-even for the card you rented.** On H100 SXM the
  NVLink break-even is 0.134; on B200 it is 0.112; if the NVLink-peer adversary is
  dropped (see [open questions](docs/open-questions.md)) it falls to ~0.019 and
  eta stops being a project risk. *Done when:* the number is written down against
  the right threshold. If eta < 0.2, stop and redesign the probe shape before
  building anything else.
- [ ] **`src/prover_cuda.cu`.** `cudaMalloc` the buffer, upload through pinned
  staging, one thread per lane, `answer_lane_t` as the thread body, block-reduce
  **in lane order** — not warp-shuffle order, or the known-answer vectors break.
  *Done when:* the same `(nonce, k, lanes, chained)` gives the same 64-bit hash as
  the CPU prover, bit for bit. No timing claims at this stage.
- [ ] **One wire revision that does everything at once.** These are five breaking
  changes to the same struct; doing them separately pays the re-pinning cost five
  times. Widen `k` past `uint32_t` (it currently caps coverage at 4.29x10^9 probes,
  and an 8-byte-granular sweep of a 192 GB buffer overflows it); move to a
  >=128-bit response with a cryptographic PRF; switch the response to a Merkle root
  over per-lane hashes with a random subset opened, so verifier cost stops being
  linear in `k`; adopt commit-before-challenge nonces; and derive each lane's index
  from committed content so the prover can't reorder or substitute lanes after
  seeing the challenge. *Done when:* the wire version bumps once, the known-answer
  vectors are re-pinned once, and soundness is published as a function of the
  number of opened lanes.
- [ ] **Honest-side operating curve** on one GPU: sweep (f, P, lanes, depth),
  record achieved bandwidth and the launch-overhead floor. Clock locking will
  almost certainly be denied on a container provider, so treat DVFS as a measured
  variance term rather than noise: 30+ seconds of warm-up before the first
  recorded trial, `clocks.sm` / `clocks.mem` / `temperature.gpu` /
  `clocks_throttle_reasons.active` recorded with **every** trial, percentiles
  rather than means, response time regressed against achieved SM clock, and
  honest and adversary trials interleaved rather than run in blocks. *Done when:*
  the curve exists and the clock regression accounts for most of the variance.
- [ ] **Pod B — the compression frontier.** See the compression section above;
  this is the same rental.
- [ ] **Pod C — the adversary rig** (8x H100 SXM, whole node, ~3 hours, ~$85).
  Renting all eight is the experiment, not extravagance: it is the only way to
  guarantee NVLink between your GPUs and to own the host DRAM and PCIe root
  complex the zero-copy measurements depend on. Build the adversaries in this
  order, cheapest first: **UVM / `cudaMallocManaged`** (pages migrate silently on
  fault, so a prover can allocate more than the card holds and look honest — this
  is the mmap adversary's direct analog and the reason "did you `cudaMalloc`?" is
  unverifiable from outside), **zero-copy host-mapped** over PCIe, then
  **peer-resident** over NVLink, then the **partial-residency sweep** that sets
  the detector's resolution. *Done when:* each has a measured margin against
  honest, with the partial-residency sweep stating the smallest offload fraction
  the detector can see.

> Two adversaries are out of scope for timing and should be said so out loud:
> **wrong-GPU delegation** (the answer is genuinely computed against resident
> data, just not here) needs device attestation, and an off-node delegate is
> caught by perimeter accounting rather than by memchal. Recompute gets *worse* on
> GPU, not better — ALU throughput relative to memory is higher there.

## Then — the decision rule

- [ ] **A decision rule with error rates.** Turn the harness's separations into an
  accept/reject classifier. *Done when:* it outputs a verdict given a response and
  a claimed configuration, with measured false-positive and false-negative rates.
  Depends on the timing regime being settled on real hardware.
- [ ] **Report a bound, not a verdict.** Certify "at least X bytes demonstrably
  resident over window T" rather than pass/fail, so the output composes
  arithmetically with a capacity denominator and a boundary byte budget instead of
  being a third incompatible verdict. This is cheap and it is what makes memchal
  usable by anyone else's stack. *Done when:* the harness emits the bound and the
  confidence that goes with it.

## Backlog

- [ ] **Revisit whether a chain depth of 16 is deep enough.** The verifier
  refuses `k/lanes < kMinChainDepth` (16), so a defense can no longer be selected
  at depth 1 — but an attacker can pipeline `chain_depth` probes across lanes, so
  the floor being non-zero is not the same as the floor being sound. ADR 0005
  reopens this at the GPU port, where thousands of lanes at fixed `k` force
  shallow chains. *Done when:* the floor is set from a measured pipelining bound
  rather than chosen, and ADR 0005 records the number.
- [ ] **Optional: a bare-metal CPU cold read** (buffer > RAM and/or `O_DIRECT`,
  pinned with `taskset`). Not on the path — the real timing check happens on the
  GPU — but it's a cheap one-command early signal of whether the timing gap
  survives real storage.
- [ ] **Wire byte-order** is native and assumes same-endian peers; make it
  explicit if the protocol ever leaves loopback.
