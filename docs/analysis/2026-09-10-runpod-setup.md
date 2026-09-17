> **Dated analysis.** Written 2026-09-10. Supplements
> `2026-08-16-gpu-port-plan.md` §4 (Tier 1) with the specifics for one provider,
> and re-scopes the experiment agenda in `2026-08-25-advisor-feedback.md` §5
> (A1, A2, A2b, A3). The pod plan below is reflected as pending work in
> `memchal/NEXT_STEPS.md`.

# RunPod: what to set up, how faithful it is, and where it breaks

Claim tags: **[LIT]** provider docs, **[ASSESS]** reasoning here,
**[MEASURED]** run on hardware. Nothing here is [MEASURED] — no GPU has been
touched yet.

---

## 0. Bottom line

RunPod is **flavour B** in the port plan's taxonomy: a real GPU, whole-card
passthrough, root inside a Docker container on someone else's host. Pods are
containers, not VMs — RunPod's own docs say "Runpod runs Docker for you, so you
cannot spin up your own Docker instance". [LIT] There is no bare-metal product;
the marketing phrase "full control over the underlying VM" describes container
freedom, not host privileges. [LIT][ASSESS]

That places it precisely:

- **Everything on the honest side is faithful enough.** eta, the lanes/depth/probe
  sweep, known-answer bit-identity, the compression frontier (A2) — all fine.
- **The peer-residency adversary is testable**, if and only if you rent a whole
  8xSXM node so NVLink is guaranteed. This is the one that matters most and
  RunPod can do it.
- **The RDMA half of the external advisor's campaign (A1's RDMA rows, A2b)
  probably cannot run here at all.** See §4.3. Check in the first 60 seconds;
  don't discover it after booking three hours.
- **You will almost certainly not be able to lock clocks.** For a timing project
  this is the sharpest limitation, and it is manageable but must be *shown* to be
  manageable rather than assumed. See §4.1.

---

## 1. Set up three pods, in this order

### Pod A — the go/no-go box (~1 hour, ~$3)

`1x H100 SXM` or `1x A100 SXM 80GB`, **Secure Cloud**, on-demand (not spot),
container `nvidia/cuda:12.8.0-devel-ubuntu22.04`, 100 GB network volume.

Runs, in order:

1. `preflight.sh` — record the output; it is trial metadata, not a formality.
2. CPU-GPU **known-answer bit-identity**: same `(nonce, k, lanes, chained)` gives
   the same hash. No timing claims. This is the port's actual correctness gate.
3. **eta** — a standalone ~100-line chained 128 B random-gather kernel over a
   >=5 GB buffer. Achieved GB/s divided by `nvbandwidth`'s device-to-device local
   figure. Wall clock only; no counters needed, which is what keeps this in
   flavour B.
4. `nvbandwidth` full run as the calibration baseline.

Stop the pod. Per-second billing means an hour is an hour. [LIT]

### Pod B — the compression frontier (~2 hours, <$10)

`1x H100 SXM`, same image. This is the external advisor's A2 and, given their C2
argument, it is now plausibly the **highest-value experiment in the whole
campaign** — the representation lie is the half nobody has attacked.

Build the `r(U)` vs `t_decompress(U)` curve: nvCOMP at tunable chunk size for the
cheap sweep, DFloat11 for the realistic point. Use a **persistent kernel
decompressor** spinning on a mailbox, not launch-per-challenge — otherwise the
~3-10 µs launch floor flatters you and the result won't survive review.

Single GPU, no NVLink, no RDMA. RunPod is a completely faithful venue for this
one. [ASSESS]

### Pod C — the adversary rig (~3 hours, ~$85)

`8x H100 SXM`, **Secure Cloud**, whole node. At ~$3.49/GPU-hr that is ~$28/hr.
[LIT] Renting all eight is not extravagance, it is the experiment: it is the only
way to guarantee NVLink/NVSwitch between your GPUs *and* to own the host DRAM and
PCIe root complex that the zero-copy adversary measurements depend on.

Runs: peer-residency over NVLink, UVM oversubscription, zero-copy host-mapped,
partial-residency sweep (the resolution question), and the per-tier latency table
from A1 minus its RDMA rows.

A cheaper gamble: rent `2x H100 SXM` (~$7/hr) and check `nvidia-smi topo -m`. If
it shows `NV#` you got two GPUs on one node and the peer experiment works for a
quarter of the price. If it shows `SYS`/`PHB`, destroy and retry. Fine for
peer-residency; **not** fine for the host-DRAM rows, where six other tenants are
sharing your PCIe.

---

## 2. The most faithful setup, concretely

| knob | choose | why |
|---|---|---|
| Cloud | **Secure Cloud** (T3/T4 DCs) | Community Cloud is peer hosts with variable PCIe width [LIT][ASSESS] |
| GPU | **H100 SXM** (or H200 SXM) | SXM means NVLink. PCIe cards have no peer fabric and can't test the adversary at all |
| Count | **8x, whole node** for Pod C | guarantees NVLink; also guarantees exclusive host DRAM + PCIe |
| Type | **on-demand, not spot** | a preemption mid-sweep loses the run and the money |
| Image | `nvidia/cuda:*-devel-*` | you need `nvcc`, `cuobjdump`, `nvdisasm`; `runtime` images don't have them |
| Storage | **network volume**, results written there | pod-local disk dies with the pod |
| Billing | per-second, stop immediately | [LIT] |

First five commands on every pod, before anything else:

```
bash preflight.sh                       # gate the whole session on this
nvidia-smi topo -m                      # want NV#, not PIX/PHB/SYS
ls /dev/infiniband; ibv_devinfo         # decides whether A1/A2b are possible here
nvidia-smi -q -d PERFORMANCE | grep -i -A8 throttle
git clone https://github.com/NVIDIA/nvbandwidth && cd nvbandwidth && cmake . && make
```

---

## 3. Recompute your numbers — you are renting H100, the plan is written for B200

| rig | HBM BW | NVLink ingress | PCIe in | NVLink margin | PCIe margin | break-even eta (NVL) | break-even eta (PCIe) |
|---|---|---|---|---|---|---|---|
| A100 SXM 80GB | 2.04 TB/s | 300 GB/s | 32 GB/s | 6.8x | 63.7x | 0.147 | 0.016 |
| H100 SXM 80GB | 3.35 TB/s | 450 GB/s | 64 GB/s | **7.4x** | 52.3x | **0.134** | 0.019 |
| H200 SXM 141GB | 4.80 TB/s | 450 GB/s | 64 GB/s | 10.7x | 75.0x | 0.094 | 0.013 |
| B200 (plan's design point) | 8.00 TB/s | 900 GB/s | 64 GB/s | 8.9x | 125.0x | 0.112 | 0.008 |

Two things fall out. [ASSESS]

**The margin is 6.8-10.7x across four GPU generations.** The detector's
viability is not a B200 artifact — it is a property of the HBM-to-NVLink ratio,
which has stayed in a narrow band because both scale together. That is a
robustness result worth having in the reply to the external advisor, and it
means an H100 measurement transfers.

**Break-even eta on H100 is 0.134**, slightly *worse* than B200's 0.112. So the
go/no-go is marginally harder on the rig you can afford, which is the safe
direction: pass on H100 and you pass on B200. And if the external advisor's C2
holds and the NVLink-peer adversary is dropped, break-even falls to **0.019**
and eta stops being a project risk on every card in the table.

Design point on one H100 SXM 80GB, N = 60 GB, 128 B probes, lanes = 2^17:

| coverage f | bytes touched | k | chain depth | honest time | bound by |
|---|---|---|---|---|---|
| 5% | 3.0 GB | 23.4 M | 179 | 0.90 ms | bandwidth |
| 10% | 6.0 GB | 46.9 M | 358 | 1.79 ms | bandwidth |
| 25% | 15.0 GB | 117.2 M | 894 | 4.48 ms | bandwidth |

Lanes needed to saturate H100 HBM: ~17,000, so 2^17 has 7.7x headroom and the
dependent chain stays free. H100 L2 is 50 MB — keep the buffer above ~5 GB or
cache hits inflate eta and you will believe a number that isn't real.

---

## 4. Drawbacks, ranked by how much they hurt

### 4.1 No clock locking — the sharpest one

`nvidia-smi -lgc` and `-pm` need host privileges. In an unprivileged container
they will almost certainly be denied. [ASSESS — verify with `preflight.sh --fix`
on the first pod; this is the single most important unknown to resolve early.]

For a project whose whole instrument is timing, that matters. The CPU work
already hit the analogous problem: absolute latency swung ~1.6x between sessions
from P-core/E-core placement. DVFS is the GPU version.

**Why it is survivable:** the margins are 7x-52x, and clock drift is a
low-tens-of-percent effect. **What you must do about it:** stop treating it as
noise and treat it as a measured variance term.

- Long warm-up (30+ s of the kernel) before the first recorded trial, so the card
  settles into a steady thermal/clock state rather than boosting.
- Record `clocks.sm`, `clocks.mem`, `temperature.gpu` and
  `clocks_throttle_reasons.active` with **every** trial, not once per session.
- Report distributions with percentiles, not means. Regress response time against
  achieved SM clock; if the regression explains most of your variance you have
  characterised the problem rather than ignored it.
- Interleave honest and adversary trials rather than running them in blocks, so
  thermal drift can't masquerade as a treatment effect.

That last one is worth doing regardless — it is cheap and it kills the most
likely confound in the whole campaign. [ASSESS]

### 4.2 Performance counters probably unavailable

`ERR_NVGPUCTRPERM` is set by a host-side kernel module parameter; NVIDIA's own
guidance is that access "must be enabled on the host, or the container must be
started with `--cap-add=SYS_ADMIN`". [LIT] You control neither on RunPod.

**What you lose:** the ability to explain eta — DRAM throughput counters, sector
efficiency, L2 hit rate. **What you don't lose:** eta itself. Wall-clock over a
known byte count is sufficient, which is why Pod A works at all. Accept the loss
and move on; buy counters later on a flavour-C provider if a number turns out to
need explaining.

### 4.3 RDMA is unverified and possibly absent — this re-scopes the external advisor's A1/A2b

This is the finding most likely to change your plan. RunPod's Instant Clusters
configuration reference documents `NCCL_SOCKET_IFNAME=ens1` and describes
"dedicated high-bandwidth network interfaces" at 1600-3200 Gbps, but **says
nothing about RDMA, RoCE, InfiniBand, ibverbs or GPUDirect**. [LIT] A cluster
whose documented NCCL knob is `NCCL_SOCKET_IFNAME` rather than `NCCL_IB_HCA` is
the signature of socket transport, not verbs. [ASSESS] Confusingly, RunPod's own
blog discusses `NCCL_IB_HCA` and `ibv_devinfo` and says `nvidia-smi topo -m`
prints GPU-to-NIC affinity "from inside your container with no privileged
access" [LIT] — so the picture is genuinely unclear from the outside.

**Resolve it in 60 seconds, not after booking:** `ls /dev/infiniband`,
`ibv_devinfo`, `lsmod | grep nvidia_peermem`.

If they come back empty, then A1's RDMA rows, A2b (ODP fault latency vs
pre-registered pinned reads), and any prototype of the external advisor's C6
RDMA-pull detector **cannot be done on RunPod** and need a flavour-C provider
(Nebius, Crusoe, Oracle) or academic HPC. Since A2b is what decides whether "the
prover must fault into software" is a 10x signal or a 1.2x one, that is a real
dependency — but it is separable, and Pods A/B/C are worth running regardless.

### 4.4 Fractional allocations share host DRAM and PCIe

If you rent 1 or 2 GPUs of an 8-GPU node, the other tenants contend for the same
host memory controllers and PCIe root complex. Your NVLink numbers survive that;
your **host-DRAM and zero-copy adversary numbers do not** — that adversary's
whole quantity is PCIe and DRAM bandwidth you are now sharing invisibly.
`preflight.sh` should flag this by inferring the node fraction from vCPU-per-GPU.
Whole node for those rows, or don't report them.

### 4.5 No nested containers

RunPod runs the container; you cannot run Docker or docker-compose inside it.
[LIT] The CPU-side `mmap-needs-ram` investigation used a cgroup memory budget in
a nested container to starve the adversary; that exact technique won't port. The
GPU analog you actually want — `cudaMallocManaged` oversubscription — needs no
containers at all, so this costs you nothing in practice. [ASSESS]

### 4.6 Community Cloud is a trap for this project specifically

Peer-to-peer hosts with unverifiable PCIe width and topology. For a workload
whose output is a bandwidth measurement, that is the one variable you cannot let
float. Pay for Secure Cloud.

---

## 5. What RunPod does **not** re-scope

The port's critical path is untouched. The CPU-side commits (split `common.hpp`,
`mulhi64` behind the macro) still block everything and still need no GPU. Pod A's
known-answer gate and eta, and Pod B's compression frontier, are all faithful on
RunPod. Only the RDMA experiments and clock-locked absolute numbers need
somewhere else — and both are second-phase work.

---

## Sources

- [RunPod Pods overview](https://docs.runpod.io/pods/overview)
- [RunPod Instant Clusters](https://docs.runpod.io/instant-clusters)
- [RunPod Instant Clusters configuration
  reference](https://docs.runpod.io/instant-clusters/configuration)
- [RunPod H100 SXM](https://www.runpod.io/gpu-models/h100-sxm)
- [RunPod GPU pods product page](https://www.runpod.io/product/bare-metal)
- [RunPod: RoCE vs
  InfiniBand](https://www.runpod.io/blog/roce-vs-infiniband-multi-node-gpu-training)
- [NVIDIA on
  ERR_NVGPUCTRPERM](https://developer.nvidia.com/nvidia-development-tools-solutions-err_nvgpuctrperm-permission-issue-performance-counters)
- [NVIDIA nvbandwidth](https://github.com/NVIDIA/nvbandwidth)
