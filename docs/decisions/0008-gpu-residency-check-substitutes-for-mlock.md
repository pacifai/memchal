# ADR 0008 — On GPU, cudaMalloc plus a pointer-attribute check substitutes for mlock and VmLck

**Status:** proposed (the substitution is implemented; what it is worth is argued
below and should be agreed before any GPU number is published against it).

**Amends the scope of:** [ADR 0002](0002-verify-and-refuse-on-mlock.md). That ADR
decided the honest prover must verify its lock and refuse to run unpinned. The
decision stands for the CPU. This records what the same principle becomes on a
device where its mechanism does not exist.

## Context

ADR 0002's guarantee has three parts: `mlock` the buffer, read `VmLck` from
`/proc/self/status` to confirm the lock took effect rather than trusting the
return value, and refuse to serve if it did not. An unpinned honest baseline is
worthless, because the pages it measures can be swapped out from under it.

None of the three mechanisms exists on a GPU.

- There is no `mlock`. Device memory obtained from `cudaMalloc` is not pageable
  to begin with, so there is nothing to pin.
- There is no `/proc` for the device. The driver keeps the allocation tables and
  exposes no equivalent file.
- There is, however, still something to get wrong, and it is worse than swapping.
  `cudaMallocManaged` returns a pointer that *looks* identical to a `cudaMalloc`
  pointer and behaves identically until the card runs short, at which point pages
  migrate silently over PCIe. A prover can allocate more managed memory than the
  card physically holds and answer every challenge correctly.

So the risk ADR 0002 addressed does not disappear on GPU. It changes shape: the
question is no longer "did the pages stay resident" but "is this allocation the
kind that can move at all".

## Decision

The GPU prover takes the nearest faithful analog of each of the three parts:

| CPU (ADR 0002) | GPU (here) |
|---|---|
| `mmap` + prefault + `mlock` | `cudaMalloc` |
| read `VmLck` from `/proc/self/status` | `cudaPointerGetAttributes` on the buffer |
| require `VmLck >= bytes` | require `type == cudaMemoryTypeDevice` |
| refuse unless `--allow-unlocked` | refuse unless `--allow-unlocked` |
| stamp `locked=false` into every result | stamp `locked=false` into every result |

`cudaMemoryTypeManaged` and `cudaMemoryTypeHost` both fail the check, which is
the point: the first is the UVM adversary and the second is the zero-copy one.

## What this is worth, and what it is not

**It is exactly as strong as ADR 0002 was, and no stronger.** This is the part
worth being blunt about, because it is easy to oversell in either direction.

`VmLck` was always self-reported. The CPU prover reads its own `/proc` entry and
tells the verifier what it found. A dishonest prover could always have sent
`locked=true` while holding nothing — and `cheating_prover.cpp` does precisely
that. The check never proved residency to the verifier; it stopped the *honest*
prover from publishing a baseline that was quietly unpinned. That is a real and
useful property, and it is the one that carries over.

`cudaPointerGetAttributes` is the same kind of check with the same limit. The GPU
cheating prover claims `locked=true` under every mode, because nothing prevents
it. "Did you call `cudaMalloc`?" is unanswerable from outside the box.

**Therefore the check does not weaken the case for the timing detector; it
sharpens it.** On CPU it was arguable that a cooperative prover plus a lock check
got you most of the way. On GPU that argument is gone: the single most important
adversary is one whose allocation is indistinguishable from honest by
construction. Timing is not a supplementary signal here, it is the only one that
survives a lying prover.

## Consequences

- `prover_cuda` refuses to serve on managed or host-mapped memory by default and
  reports `Ready.locked` from the driver's answer, so the existing verifier and
  harness need no change to benefit.
- `cheating_prover_cuda` claims `locked=true` regardless of backing. That is
  deliberate and documented in the source, so nobody reads it as a bug.
- Every honest GPU trial should record the driver's reported backing alongside
  its timings, the same way CPU trials record `locked`.
- **No GPU number may be published on the strength of this check alone.** It
  establishes that the honest baseline is honest. It establishes nothing about a
  remote prover, which is what the bandwidth-shaped challenge is for.

## Alternatives considered

**Trust `cudaMalloc` to have done the right thing.** Rejected for the same reason
ADR 0002 rejected trusting `mlock`'s return value: the call can succeed and still
leave you somewhere you did not intend, and the failure is silent.

**Attest the allocation cryptographically.** A device-signed statement about what
it holds would be strictly stronger than anything here, and it is the right long
term answer to the wrong-GPU delegation adversary too. It needs hardware support
this project does not assume, so it is out of scope and noted rather than dropped.

**Infer the allocation type from timing instead of asking.** This works and is
what the detector does anyway. It is not a substitute for the self-check, because
the self-check runs before any measurement and stops a bad baseline being
recorded at all.
