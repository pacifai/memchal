# Does the UVM cheat survive without HBM to hold the buffer?

**Answer: no, and the edge is sharp.** Managed memory is indistinguishable from
an honest prover right up to the point where free HBM falls below the buffer
size, then costs about 4,600x. There is no gradual degradation to detect early —
the detector either sees nothing or sees three and a half orders of magnitude.

First measurement ever taken from a GPU in this project. **[MEASURED]**

## The question

`cudaMallocManaged` returns a pointer indistinguishable from `cudaMalloc`'s.
Pages migrate between host and device on fault, so a prover can claim to hold
more than the card physically has and still answer every challenge correctly
([ADR 0008](../../decisions/0008-gpu-residency-check-substitutes-for-mlock.md)).
It is the GPU twin of the CPU `mmapdisk` adversary, and adversary #1 in the port
plan.

On CPU, `mmapdisk` looks honest while the page cache holds its file and collapses
to ~840x when starved ([ADR 0003](../../decisions/0003-timing-detector-scope-and-measurement.md)).
The question is whether the GPU behaves the same way, and at what point.

## Method

One A40 (44.4 GiB HBM) on RunPod, `nvidia/cuda:12.8.0-devel-ubuntu22.04`.
A second allocation (`--hog`) occupies device memory so the managed buffer cannot
fit — the GPU analog of the CPU demo's container memory budget. Buffer fixed at
4096 MiB; the hog is swept; everything else held constant.

`--mb 4096 --k 262144 --lanes 16384` (chain depth 16), `--gen chacha
--chain dependent --nonce random`, n=30 measured after 5 warmup.

Honest baseline for the ratio column: 629 us p50, median of two interleaved
cycles in the same session.

Script: [`run_uvm_sweep.sh`](run_uvm_sweep.sh). Machine: [`results/machine.json`](results/machine.json).

## Finding

| HBM held hostage (MiB) | nominal free (MiB) | p50 | ratio to honest | verify errors |
|---:|---:|---:|---:|---:|
| 0 | 46068 | 618 us | 0.98x | 0 |
| 36000 | 10068 | 648 us | 1.03x | 0 |
| 39000 | 7068 | 676 us | 1.07x | 0 |
| 40000 | 6068 | 535 us | 0.85x | 0 |
| 41000 | 5068 | 647 us | 1.03x | 0 |
| **42000** | **4068** | **2.92 s** | **4643x** | 0 |
| 42500 | 3568 | 3.33 s | 5298x | 0 |
| 43000 | 3068 | 3.75 s | 5958x | 0 |

The buffer is 4096 MiB. The cliff sits between 5068 MiB and 4068 MiB of nominal
free memory — that is, exactly where free HBM stops being able to hold the
buffer. Nominal free overstates the real figure by the CUDA context's couple of
hundred MiB, which puts the crossover at the buffer size rather than slightly
below it.

Three things follow.

**The detector has no early warning.** Every row above the cliff is inside the
run-to-run noise; the 0.85x at 40000 MiB is noise, not a signal, and it is larger
than any trend in the rows around it. Nothing in the timing distinguishes a cheat
holding 99% of the buffer on the card from an honest prover. The signal appears
only when residency actually fails.

**The collapse is far steeper than on CPU.** 4,600x against the CPU adversary's
840x, because a GPU page fault crosses PCIe and serializes against a kernel with
thousands of threads in flight, where the CPU equivalent faults one page into a
page cache that is already nearby.

**A null result above the cliff is a true negative, not a miss.** A managed
buffer that fits migrates to the card once and is genuinely resident. The
detector is correct to pass it. This is the same shape as the CPU finding that
`mmapdisk` looks honest while the page cache holds its file, and it is worth
stating plainly because the opposite reading — "the detector failed to catch
UVM" — is the natural one and it is wrong.

## What this does not establish

The crossover was measured at one buffer size on one card. Whether it always
lands at exactly "free HBM < buffer size", or whether a smaller buffer degrades
more gradually because a useful fraction stays resident, is unmeasured. The
partial-residency sweep in `NEXT_STEPS.md` is the experiment that would settle
it, and it is what sets the detector's real resolution.

Absolute microseconds here are not comparable to any other session. Clock locking
was denied on this box (`"clock_lock": "denied"` in the machine record), so the
card's clocks wandered freely; only the within-session ratios above are meaningful.

## Reproduce

```bash
bash scripts/gpu_pod_setup.sh          # gate the box first
bash docs/investigations/uvm-needs-hbm/run_uvm_sweep.sh
```
