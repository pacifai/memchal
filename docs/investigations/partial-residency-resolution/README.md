# How little can a prover offload before the detector stops seeing it?

**Answer: about 5% of the buffer, at this geometry, on that host.** Below that it
hides inside the honest noise; at 5% it separated and kept separating smoothly
after. **[MEASURED]** on one H100, one session. A later session found that the
figure also depends on the host CPU's cache: on a host whose cache held the
offloaded bytes, 5% did not separate at all. See
[the host CPU's cache](#later-finding-the-host-cpus-cache).

This is the number a decision rule needs, and the first GPU session could not
supply it.

## The question

The UVM sweep ([uvm-needs-hbm](../uvm-needs-hbm/README.md)) moved the whole
buffer on or off the card and found a cliff with nothing in between: honest
until free memory could not hold the buffer, then 4,643x. That answers "can you
catch a prover holding none of it" and says nothing about how small a lie stays
visible — which is what sets the detector's resolution and what a classifier
would need a gradient for.

The `partial` cheat fills that gap. It holds every byte and answers correctly,
but keeps a fraction of the buffer in pinned host memory reached over PCIe.
Offload 0 is the honest prover by construction.

## Method

One H100 SXM 80GB, 4096 MiB buffer, k=262144, 16,384 lanes, chain depth 16, tree
fold, defenses on, n=40 after 10 warmup.

The honest baseline is re-measured **between every offload point**, ten times in
all, rather than once at the start. Clock locking is denied on a container
provider, so a baseline taken half an hour earlier is not a baseline.

A point counts as separated only if it clears the honest **spread**, not the
honest median: threshold is median + 3 standard deviations across those ten
baselines. That is a deliberately conservative bar, and with clocks wandering it
is the honest one.

Script: [`gpu_resolution.sh`](gpu_resolution.sh).

## Finding

Honest baseline: **188.8 us** median, **11.6 us** standard deviation over ten
interleaved measurements. Separation threshold: **223.8 us**.

| offload | p50 | ratio to honest | separated? |
|---:|---:|---:|---|
| 0.0% | 190.6 us | 1.01x | no |
| 0.1% | 193.0 us | 1.02x | no |
| 0.2% | 189.5 us | 1.00x | no |
| 0.5% | 190.1 us | 1.01x | no |
| 1.0% | 212.5 us | 1.13x | no |
| 2.0% | 205.9 us | 1.09x | no |
| **5.0%** | **249.9 us** | **1.32x** | **yes** |
| 10.0% | 319.9 us | 1.69x | yes |
| 25.0% | 796.6 us | 4.22x | yes |
| 50.0% | 2338.7 us | 12.38x | yes |

Zero verify errors at every point — this cheat is always correct, which is the
whole reason it has to be caught on timing.

**Resolution is 5%.** On a 4 GiB buffer that is about 205 MiB a prover can keep
elsewhere without this detector noticing.

**Unlike the UVM cliff, this is a gradient.** From 5% upward the ratio rises
smoothly and roughly in proportion to the offloaded fraction. That matters more
than the resolution number itself: a classifier needs something monotone to set
a threshold against, and the UVM result alone suggested there was nothing there.

**The floor is noise, not physics.** 1% already shows 1.13x, which is visible to
the eye but does not clear three standard deviations of a baseline whose spread
is 6% of its own median. The limit here is the width of the honest distribution
on a card whose clocks cannot be locked — so the resolution should improve with
more samples per point, more probes per challenge, or a box that permits clock
locking. None of those was tried.

## What this does not establish

One geometry, one card, one session. Resolution at 4096 MiB with k=262144 says
nothing directly about a 60 GB buffer, and the interesting deployment case is
the large one.

The cheat offloads a contiguous tail. A real one would choose *which* bytes to
offload — the coldest, or the ones least likely to be probed — and would do
better. This is the naive version, so 5% is an upper bound on how well the
detector does against a thoughtful adversary, not a guarantee.

## Later finding: the host CPU's cache

A demo session on 2026-10-11, on another H100 pod, did not reproduce 5%. The 5%
point's response time landed inside the honest range: 201 us against honest runs
of 189 and 207 us in the same session. The kernel time the prover reports was
what explained it. The offloaded tail's cost depended on whether the tail fit in
the host CPU's last-level cache, not only on how much of the buffer it was.

That host had two Intel Xeon Platinum 8480+ processors with 210 MiB of L3 in
total. When the GPU reads pinned host memory over PCIe, an Intel host can answer
from L3 instead of DRAM, and the prover's upload has just written the tail
through the CPU, so the tail starts out cached. Same geometry as above, apart from
the buffer size; median kernel time over 200 challenges; honest was 24 us at both
sizes. **[MEASURED]**, one session:

| buffer | offload | tail in host memory | fits in 210 MiB of L3 | extra kernel time |
|---:|---:|---:|---|---:|
| 4096 MiB | 2% | 82 MiB | yes | +7 us |
| 4096 MiB | 5% | 205 MiB | just | +11 us |
| 1024 MiB | 10% | 102 MiB | yes | +22 us |
| 1024 MiB | 20% | 205 MiB | just | +55 us |
| 4096 MiB | 10% | 410 MiB | no | +123 us |

The same 10% of the probes cost 22 us when its 102 MiB fit in cache and 123 us
when its 410 MiB did not. Holding the bytes fixed at 205 MiB, cost rose with the
share of probes that left the card (+11 us at 5%, +55 us at 20%). This sweep's
own 5% point added about 61 us, the uncached rate, so its host most likely did not
keep the tail in cache. Its machine record did not capture the CPU model, so that
cannot be confirmed.

What follows:

- **The resolution is partly a property of the host.** A prover can keep roughly
  one CPU cache's worth of bytes off the card at a discount, and the verifier
  cannot see the host's cache size from outside. Some server CPUs have more than
  1 GiB of L3.
- **The discount is bounded in bytes, not in fraction.** Against a buffer of tens
  of gigabytes per GPU, a few hundred MiB is well under 1%. A 4 GiB test buffer
  makes it look large.
- **The response hides small kernel costs.** The kernel was about 24 us of a
  ~200 us response; the rest (fold launches, readback, the network path) drifted
  by about 20 us between runs, which buried the 5% tail's 11 us.

## Reproduce

```bash
bash scripts/gpu_pod_setup.sh
bash docs/investigations/partial-residency-resolution/gpu_resolution.sh --mb 4096 --k 262144 --lanes 16384 --n 40 --fold tree
```
