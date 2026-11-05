# ADR 0002 — Refuse to run unless the buffer is verifiably resident

**Status:** accepted

## Context

The honest prover's results are worth something only if its buffer really sits in
fast memory: RAM that can't be swapped out on a CPU, or device memory on a GPU. An
honest prover that silently ran from slower memory would produce plausible but
meaningless numbers.

The call that requests fast memory can succeed without delivering it. On a CPU,
`mlock()` can return success without the pages being wired. On a GPU, a
`cudaMallocManaged` pointer looks and behaves like a `cudaMalloc` pointer until the
card runs short, and then its pages migrate over PCIe without any error. So the
prover can't trust the allocation call; it has to ask the system what it got.

## Decision

The honest prover checks where its buffer lives right after allocating it, before
it receives or builds any data. If the buffer isn't verifiably resident, the prover
refuses the session and closes the connection. `--allow-unlocked` overrides the
refusal, prints a warning, and sends `locked=false` to the verifier, which records
it in the JSON, CSV, and plots.

The check is the same on both platforms; only the mechanism differs:

| | CPU | GPU |
|---|---|---|
| Allocate | `mmap`, touch every page, `mlock` | `cudaMalloc` |
| Ask what you got | read `VmLck` from `/proc/self/status` | `cudaPointerGetAttributes` on the buffer |
| Accept only | `VmLck` at least the buffer size | `type == cudaMemoryTypeDevice` |

### CPU

`cpu/prover.cpp` locks the buffer and reads `VmLck` (`mc::locked_bytes()` in
`cpu/proc_stats.hpp`). It reports `locked=1` only if `VmLck` covers the buffer. On
a platform without `/proc`, it falls back to `mlock()`'s return value.
`docker/run.sh` grants `IPC_LOCK` and an unlimited memlock limit so the honest
prover locks cleanly. The CPU cheating prover runs the same check but never
refuses, because the disk, partial, and fake modes are unlocked by design.

### GPU

Device memory from `cudaMalloc` can't be paged, so there is nothing to lock and no
`/proc` entry to read. The question becomes whether the allocation is the kind that
can move at all. `gpu/prover.cpp` asks the driver for the buffer's memory type.
Managed memory (the `uvm` adversary) and host-mapped memory (the `partial`
adversary's offloaded part) both fail. The GPU cheating prover claims
`locked=true` in every mode, because nothing outside the machine can tell
otherwise.

## What the check establishes

The check is self-reported on both platforms. The prover reads its own `/proc`
entry or asks its own driver, then tells the verifier the result. A dishonest
prover can always send `locked=true` while holding nothing, and both cheating
provers do. So the check proves nothing to the verifier. It stops the honest
prover from publishing a baseline that quietly ran from slow memory.

That limit is why the timing detector carries the claim. On a GPU especially, the
most important adversary allocates memory that is indistinguishable from honest
memory when you ask about it. Timing is the only signal that survives a prover
that lies.

## Consequences

- No result silently rests on an honest baseline that wasn't resident. Each run
  records its lock status.
- The prover refuses before the verifier uploads the buffer, so an unlocked prover
  never costs a full upload.
- No GPU number can be published on the strength of this check alone. It shows
  that the honest baseline is honest; it says nothing about a remote prover.

## Alternatives considered

**Trust the allocation call.** Rejected: `mlock` and `cudaMalloc`-like calls can
succeed and still leave the buffer somewhere you didn't intend, and the failure is
silent.

**Attest the allocation cryptographically.** A statement signed by the device about
what it holds would be stronger than anything here, and would also answer the
adversary who delegates to a different GPU. It needs hardware support this project
doesn't assume.

**Infer the memory type from timing instead of asking.** The detector does this
anyway. It doesn't replace the self-check, which runs before any measurement and
keeps a bad baseline from being recorded at all.
