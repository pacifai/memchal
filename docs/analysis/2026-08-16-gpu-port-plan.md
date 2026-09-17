> **Dated analysis.** Written 2026-08-16 against memchal @ `4da2d55`.
> Amended by `2026-08-25-advisor-feedback.md` §8 and re-scoped for one provider by
> `2026-09-10-runpod-setup.md`. Two lines are now stale: step 3 below (open issue
> #7) is **done**, and §3's "single decision point" framing for eta is softened by
> C2. For current pending work read `memchal/NEXT_STEPS.md`.

# memchal to GPU: what the port is, how to validate it without hardware, and what to do first

Reads on top of `2026-08-16-memchal-state.md` (open issues) and
`2026-08-16-advisor-review.md` (bandwidth-vs-latency framing). Assumes a B200-class node; figures from
the memory-ladder figures doc.

---

## 0. Bottom line

The port is **not** "compile `answer_lane_t` with `nvcc`." The answer path really
is GPU-shaped and that part is nearly free. What has to change is the
**challenge sizing**, because the discriminator changes: on CPU the detector is
latency-based, on a GPU node it has to be **bandwidth**-based, and that is a
different-shaped challenge (10^8 probes over ~10 GB in ~1 ms, not 10^3 probes in
~100 µs).

Two things came out of working the numbers that change the plan:

1. **A naive port has a negative margin.** Ported as-is — 8-byte fine-grained
   random probes — the honest GPU achieves ~0.8 TB/s effective, which is
   *slower* than an NVLink-peer adversary's 900 GB/s ingress. The honest prover
   loses to the cheat. The probe granularity has to go to 128 B.
2. **The GPU dissolves open issue #1/#2** (lanes-vs-chain-depth tension). That
   tension is a CPU artifact of small `k`. At bandwidth-appropriate `k` (~7.8x10^7)
   you get 2^17 lanes *and* ~600-deep dependent chains simultaneously, and the
   chaining is **free** — the kernel is bandwidth-bound, so the pointer-chase
   latency hides underneath. You do not have to choose.
3. **The verifier becomes the bottleneck** and nobody has priced it. A
   bandwidth-bound challenge is as expensive to check as to answer — ~80x the
   prover's own time on a CPU verifier. The fix (per-lane Merkle openings, 0.2%
   verifier cost) is cheap but has to go into the same wire revision as the
   >=128-bit response already owed.

And the whole thing reduces to **one measurable number** (§3), which costs about
a dollar and ten minutes on a rented GPU. Simulators cannot give you that number
(§4).

---

## 1. What carries over, what changes, what is new

### Carries over unchanged
- `splitmix64`, `mix64`, `answer_lane_t`, `answer_challenge_t` — already written
  as flat C over a raw pointer with no STL/exceptions. This was good foresight.
- `chacha20.hpp` — table-free, verifier-side only.
- Known-answer bit-identity. **Verified 2026-08-16**: `__umul64hi(r,n)` is
  bit-identical to the current `((unsigned __int128)r * n) >> 64` across 2x10^7
  random pairs plus all 64 boundary combinations of {0, 1, 2^32-1, 2^32, 2^63,
  2^64-1}. The one flagged porting risk is a non-risk.

### Changes
- **`mlock` to `cudaMalloc`.** Device memory is non-pageable by default, so the
  `--allow-unlocked` / `VmLck` machinery has no GPU analog. What replaces it as
  the thing to *verify*: that the allocation is `cudaMalloc` and not
  `cudaMallocManaged` (see §5) — and the prover can lie about that, which is
  precisely why the timing detector still has to carry the weight.
- **The timing floor moves up ~2 orders of magnitude.** Kernel launch is ~3-10 µs
  and the challenge still crosses PCIe (~2 µs) before it reaches the device. A
  100 µs challenge window is now mostly floor. Challenge windows must be
  **milliseconds**. Conveniently this is the same direction bandwidth-hardness
  pushes.
- **`common.hpp` must be split.** It currently includes `<sys/socket.h>`,
  `<netinet/in.h>`, `/proc` readers and the challenge math in one header.
  That header cannot be included from a `.cu` translation unit as-is.

### New
- Block-level reduction of lane results (the CPU fold is sequential over lanes).
  Keep the fold order identical to the CPU reference or the known-answer test
  breaks — fold in lane order, not in warp-shuffle order, unless you re-pin the
  vectors.
- Clock locking. GPU DVFS will otherwise inject variance that eats the detection
  margin the same way P-core/E-core placement did on the Mac.
  `nvidia-smi -lgc` before every run, and record the achieved SM clock per trial.

---

## 2. The design shift: bandwidth-hardness as the primary discriminator

**Why latency alone fails.** NVLink5 is ~1.5 µs vs HBM's ~700 ns — about 2x,
inside jitter. You cannot exclude a peer GPU on latency.

**Why bandwidth works.** NVLink5 is 1.8 TB/s *bidirectional* per GPU, so
**900 GB/s ingress**, and that is a hard per-GPU cap: an adversary with 71 NVL72
peers does not get more ingress than an adversary with one. Local HBM is 8 TB/s.
That is an **8.9x ceiling**, not 4.4x — `2026-08-16-advisor-review.md` used 1.8 TB/s as
unidirectional and understated the margin by half.

**Why sparse probes cannot work.** The CPU PoC's `k=4096` probes x 8 B = 33 KB.
An NVLink peer imports that in 0.04 µs. Even `k=2^24` is 134 MB, or 149 µs.
Sparse probing has no bandwidth story at all; the challenge has to force reading
a **fraction f of the whole buffer**.

### The design point

Take N = 100 GB resident, coverage f = 10%, probe granularity P = 128 B
(one sector group — random *location*, coalesced *access*):

| quantity | value |
|---|---|
| bytes touched | 10 GB |
| probes `k` | 78,125,000 |
| lanes needed to saturate HBM (Little's law: 8 TB/s x 700 ns / 128 B) | >= 43,750 |
| chosen lanes | 2^17 = 131,072 |
| chain depth `k/lanes` | 596 |
| latency-serialized floor (596 x 700 ns) | 0.417 ms |
| bandwidth floor (10 GB / 8 TB/s) | **1.250 ms** <- binding |

Because the bandwidth floor binds, the dependent chain costs the honest prover
**nothing** — the opposite of the CPU result (issue #6: chaining cost 3.5x).
Per-lane serialized throughput is 128 B / 700 ns = 183 MB/s; 2^17 lanes x
183 MB/s = 24 TB/s of demand against an 8 TB/s supply. Saturated with 3x
headroom.

### Margins at that design point

| prover | time | margin |
|---|---|---|
| honest (local HBM) | 1.25 ms | 1x |
| NVLink5 peer HBM (ingress-bound; chaining blocks prefetch) | 11.1 ms | **8.9x** |
| host DRAM over PCIe Gen5 x16 | 156 ms | 125x |
| NVMe | 7.1 s | 571x |

This is a *cleaner* detector than the CPU one, because the adversary's ceiling is
a physical link width rather than a syscall cost that turned out to be an
artifact.

### The verifier cost this creates — an unpriced consequence

At GPU sizing the verifier must recompute all 7.8x10^7 probes to check one
response. Against its own copy that is ~100 ms if it is perfectly bandwidth-bound
on 100 GB/s host DRAM, and ~8 s single-threaded at a realistic ~100 ns per random
probe. The challenge is *deliberately* memory-bound, so **verification is as
expensive as proving** — 80x the prover's own time even in the optimistic case.

Two ways out, both worth designing now rather than discovering later:

- **The verifier has a GPU too.** Legitimate in the datacenter-compliance
  setting — an inspector with hardware — and it makes the asymmetry vanish. But
  it changes the deployment story and should be stated explicitly.
- **Per-lane openings.** Have the prover return a Merkle root over the 2^17
  per-lane hashes instead of a single fold, and open a random subset. Checking
  256 lanes costs the verifier 1.5x10^5 probes — **0.2% of the work** — while a
  prover who skipped any lane is caught with probability 1-(1-p)^256. This
  changes the response from 64 bits to a root plus openings, which is required
  anyway for >=128-bit responses. Doing both in one wire revision is the
  efficient path.

This is *not* the same as the existing correctness detector — it is about making
the verifier's cost sublinear in `k`, which the current single-fold response
structurally cannot be.

---

## 3. The one number that decides whether any of this works

Everything above assumes the honest prover achieves close to peak HBM bandwidth
on **128-byte, randomly-located, data-dependent** gathers. That is not
guaranteed — random access can lose an order of magnitude. Call the achieved
fraction **eta**. Then:

| eta (achieved / peak) | honest | NVLink adversary | margin |
|---|---|---|---|
| 1.00 | 1.25 ms | 11.1 ms | 8.9x |
| 0.90 | 1.39 ms | 11.1 ms | 8.0x |
| 0.70 | 1.79 ms | 11.1 ms | 6.2x |
| 0.50 | 2.50 ms | 11.1 ms | 4.4x |
| 0.40 | 3.13 ms | 11.1 ms | 3.6x |
| 0.25 | 5.00 ms | 11.1 ms | 2.2x |
| **0.11** | 11.4 ms | 11.1 ms | **0.98x — detector is dead** |

**eta ~= 0.11 is the break-even.** Below it the honest prover is slower than the
cheat and the protocol inverts. eta is a single microbenchmark: a chained 128 B
random-gather kernel over a multi-GB buffer, achieved GB/s divided by
`nvbandwidth`'s device-to-device local figure.

It needs **one GPU, one kernel, ten minutes** — and it does *not* need NVLink,
because it is an honest-side measurement.

> Amendment (2026-08-25): if the external advisor's C2 holds and the NVLink-peer
> adversary is dropped, the surviving adversary is host DRAM over PCIe at ~125x
> and break-even eta falls to about 0.008. eta stops being a project risk. Do
> not delete the bandwidth-hard challenge — it is still the only instrument that
> produces an aggregate byte bound — but stop describing eta as the project's
> single decision point.

---

## 4. Validating without a GPU

### Tier 0 — genuinely useful with no GPU at all

- **Bit-exactness / known-answer tests.** Fully portable.
- **Compile + static analysis without a device.** `nvcc -arch=sm_100 -cubin`
  compiles fine on a CPU-only box; `ptxas -v` gives register counts and
  occupancy, `nvdisasm`/`cuobjdump` give SASS. A 128 B probe should compile to
  8x `LDG.E.128` (16-byte vector loads, `uint4`), not 16 scalar 8-byte loads. If
  it doesn't, eta is dead on arrival and you find that out for free.
- **The analytical model.** The tables in §2/§3 are a 40-line script. Extend it
  into a parameter sweep — (N, f, P, lanes, depth, eta, BW, latency) to predicted
  honest time and per-adversary margin — and it tells you which regimes are worth
  buying GPU time for.
- **CPU shadow of the GPU shape.** Run the CPU prover at GPU-like parameters
  (lanes = 2^17, k = 10^7) to shake out integer overflow and the reduction-order
  question. `Challenge.k` is `uint32_t`, which caps coverage at 4.29x10^9 probes
  = 550 GB at 128 B granularity — survivable for now, but a full 8-byte-granular
  sweep of a 192 GB buffer is 2.4x10^10 and overflows.

### Tier 0.5 — simulators: don't

- **Speed.** GPGPU-Sim is reported at ~4.8x10^5 slower than hardware. A 1.25 ms
  challenge becomes ~10 minutes of simulation *per trial*.
- **Capacity.** Functional simulation backs device memory with host RAM. A
  100 GB resident buffer needs a 100 GB host. The residency claim is *about*
  capacity, so shrinking the buffer removes the thing under test.
- **The fatal one: they don't model the adversary.** Accel-Sim/GPGPU-Sim are
  single-GPU. No NVLink peer, no NVSwitch, no host-DRAM-over-PCIe path. MGPUSim
  does model multi-GPU but is AMD GCN3-era.

### Tier 1 — rent a real GPU

The axis that matters is **not price or GPU model — it is how much of the machine
you control.**

**Flavour A — notebook / shared** (Colab, Kaggle, Lightning free tiers). Real
GPU, but you are a guest: no host privileges, no clock locking, possible
time-slicing, preemptible. **Correctness only.** Never take a publishable number
from one.

**Flavour B — container on someone else's host** (RunPod, Vast.ai, Modal,
Thunder). Real GPU, usually whole-card passthrough, root *in the container* but
not on the host. Two traps: performance counters may be locked and unfixable
(`ERR_NVGPUCTRPERM` is a host kernel-module parameter), and on Vast.ai host
quality varies by host. **The good news:** eta does not need counters — wall
clock over a known byte count is sufficient.

**Flavour C — VM-with-passthrough or bare metal** (CoreWeave, Nebius, Crusoe,
Oracle, Fluidstack, Lambda, hyperscalers). Root in a guest that owns the card:
reload the driver with profiling enabled, lock clocks, set persistence mode, read
topology honestly. **This is what the adversary experiments need.**

**Flavour D — free/academic.** The Technion runs public AI&HPC GPU clusters and
the Israel Inter-University Computation Center coordinates national allocations.
Academic HPC typically provides multi-GPU NVLink nodes with real topology for
free, at the cost of a Slurm queue.

### Mapping flavours to stages

| stage | flavour | why | rough cost |
|---|---|---|---|
| known-answer bit-identity CPU-GPU | A (free) | correctness only | $0 |
| **measure eta** | B, 1x A100/H100 | wall-clock only | <$1 |
| lanes/depth/P sweep, launch floor, clock variance | C, 1x GPU | needs clock locking | ~$5 |
| UVM + zero-copy adversaries | C, 1x GPU | single card is enough | ~$5 |
| **peer-residency adversary** | C, >=2 GPUs with real NVLink (or D) | the whole threat model | $20-60 |

### Before you trust any rented box

Two failure modes silently produce meaningless numbers: a "multi-GPU" instance
that is PCIe-only (check `nvidia-smi topo -m` for `NV#`, not `PIX`/`SYS`), and a
virtualization layer that blocks peer access (check `cudaDeviceCanAccessPeer`,
and that `nvbandwidth`'s device-to-device test reports NVLink-class figures).
A `scripts/preflight.sh` should check these plus clock-lock permission, MIG/MPS
sharing, ECC state, negotiated PCIe width, other tenants on the card, and Xid
errors — run it as the first command on every rented box and record its output
alongside every trial. A box that fails it is not a box you argue with; it is a
box you destroy and re-rent.

`nvbandwidth` is worth adopting outright as the calibration harness. Its
device-to-device local number is the denominator for eta.

---

## 5. The GPU adversary set

In rough order of how much they matter:

1. **`cudaMallocManaged` / UVM.** The direct analog of the mmap adversary, and
   worse: pages migrate silently between host and device on fault. A prover can
   allocate more managed memory than the card physically has and *look* like it
   holds it. First adversary to build. Also the one that makes "did you
   `cudaMalloc`?" unverifiable from outside.
2. **Peer-resident.** Buffer lives in a neighbour GPU's HBM, pulled over NVLink
   via `cudaDeviceEnablePeerAccess`. Needs the multi-GPU rig.
3. **Zero-copy host-mapped** (`cudaHostRegister` + `cudaHostGetDevicePointer`).
   Buffer in pinned host DRAM, read over PCIe on demand. The 125x row.
4. **Recompute on-device.** Gets *worse* on GPU: enormous ALU throughput relative
   to memory makes regenerating `splitmix64` words even more favourable than on
   CPU.
5. **Wrong-GPU / delegation.** The prover forwards the challenge to a different
   GPU that does hold the data. **Timing cannot catch this.** Out of scope for
   the timing detector; needs device attestation.
6. **Partial residency.** Hold (1-p) locally, p on a peer. A 90/10 split against
   an 8.9x ceiling still shows; a 99/1 split does not. This sets the *resolution*
   of the detector and feeds the decision rule.

> Amendment (2026-08-25): add the ODP (on-demand paging) RDMA memory region as
> the NIC-layer twin of adversary #1, and add the persistent-kernel decompressor
> as a new adversary. See `2026-08-25-advisor-feedback.md` §4.

---

## 6. First steps, in order

**Before any CUDA is written — three commits that are pure CPU work:**

1. **Split `common.hpp`.** `challenge.hpp` = the math (splitmix64, mix64,
   map_index, answer_lane_t, answer_challenge_t, chacha20) with zero POSIX
   includes; `net.hpp` = sockets and `/proc` readers. Add a `MC_HOST_DEVICE`
   macro that expands to `__host__ __device__` under `__CUDACC__` and to nothing
   otherwise. The known-answer test must pass unchanged after the split.
2. **`mulhi64()` behind the macro.** `__umul64hi` on device, the 128-bit
   expression on host. Verified bit-identical; add it to the known-answer test.
3. ~~**Close open issue #7 first.**~~ **Done as of 2026-09-10** —
   `scripts/run_trials.py` now has `--gen`, `--chain`, `--nonce` and the
   recompute/prefetch/precompute modes.

**Then, cheapest-GPU-first:**

4. **Measure eta** (§3) on one rented GPU hour, with a standalone ~100-line
   kernel — not the protocol, just the gather loop. If eta < 0.2, stop and
   redesign the probe shape before building anything else.
5. **`src/prover_cuda.cu`.** `cudaMalloc` the buffer, upload through pinned
   staging, one thread per lane, `answer_lane_t` as the thread body, block reduce
   in lane order. Success criterion: same `(nonce, k, lanes, chained)` gives the
   **same 64-bit hash** as the CPU prover, bit-for-bit. No timing claims here.
6. **One wire revision that does everything at once**: widen `k` past
   `uint32_t`, move to a >=128-bit response with a crypto PRF and fresh verifier
   nonces, and switch the response to a Merkle root over per-lane hashes with
   openings. Three breaking changes to the same struct; do them together or pay
   the re-pinning cost three times.
7. **Honest-side operating curve** on one GPU: sweep (f, P, lanes, depth), record
   achieved bandwidth and the launch-overhead floor, with clocks locked.
8. **Adversaries 1 -> 3 -> 2** on the multi-GPU node, in that order.
9. **Decision rule** with measured FP/FN, on real physics rather than a cgroup
   proxy.

Steps 1-2 are the ones to start today: no GPU needed, on the critical path
regardless of what eta turns out to be.

---

## Sources

- [NVIDIA nvbandwidth](https://github.com/NVIDIA/nvbandwidth/blob/main/README.md)
- [NVIDIA nvbandwidth technical
  blog](https://developer.nvidia.com/blog/nvidia-nvbandwidth-your-essential-tool-for-measuring-gpu-interconnect-and-memory-performance/)
- [NVLink 5.0 per-GPU
  bandwidth](https://hardwarenation.com/resources/blog/nvidia-nvlink-5-0-accelerating-multi-gpu-communication/)
- [Parallelizing a modern GPU simulator,
  arXiv:2502.14691](https://arxiv.org/html/2502.14691v1)
- [Accel-Sim (Khairy et al.)](https://mkhairy.github.io/Docs/Accel-Sim.pdf)
- [MGPUSim, ISCA 2019](https://people.bu.edu/joshi/files/mgpusim-isca2019.pdf)
- [Dissecting the NVIDIA Blackwell Architecture with Microbenchmarks,
  arXiv:2507.10789](https://arxiv.org/pdf/2507.10789)
