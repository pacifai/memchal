# ADR 0002: The honest prover refuses to run unless its buffer is in fast memory

**Status:** accepted

## Context

Every cheating prover is judged against an honest one. If the honest prover
quietly ran from slow memory, the comparison would look fine and mean nothing.

Asking for fast memory doesn't guarantee getting it:

- **CPU:** `mlock` can report success without the pages staying in RAM.
- **GPU:** memory from `cudaMallocManaged` behaves like `cudaMalloc` memory
  until the card runs short. Then the driver moves pages to the server's main
  memory, with no error.

So the prover can't trust the call that allocated the buffer. It has to ask the
system where the buffer ended up.

## Decision

Right after allocating its buffer, before it receives or builds any data, the
honest prover checks where the buffer lives. If the buffer isn't in fast memory,
the prover refuses the session.

| | CPU | GPU |
|---|---|---|
| Allocate | `mmap`, touch every page, then `mlock` | `cudaMalloc` |
| Check | Read `VmLck` from `/proc/self/status` | Call `cudaPointerGetAttributes` on the buffer |
| Accept only | `VmLck` covers the whole buffer | The memory type is device memory |

`--allow-unlocked` runs anyway: the prover prints a warning and tells the
verifier `locked=false`, which the verifier records with the results. On a
system without `/proc`, the CPU prover falls back to what `mlock` returned.
`docker/run.sh` gives the container permission to lock memory, so the honest CPU
prover passes the check there.

The cheating provers never refuse. Keeping data somewhere else is what they do.

## What the check proves, and to whom

The prover checks itself and reports the result. A dishonest prover can report
`locked=true` while holding nothing, and the cheating provers do. So the check
proves nothing to the verifier. It only keeps the honest baseline honest.

That's why timing carries the result ([ADR 0003](0003-timing-detector-scope-and-measurement.md)).
On a GPU, managed memory looks the same as device memory to anyone who asks the
driver, so timing is the only signal that survives a prover that lies.

## Consequences

- No result rests on an honest baseline that ran from slow memory.
- The prover refuses before the verifier sends the buffer, so a refusal costs no
  upload.
- The check alone can't support any claim about a remote prover.

## Alternatives considered

- **Trust the allocation call.** Rejected: both platforms can succeed and still
  leave the buffer in slow memory, without an error.
- **Have the device sign a statement about what it holds.** This would be much
  stronger, and would also show which device answered. It needs hardware
  support that memchal doesn't assume.
- **Infer the memory type from timing alone.** Timing does this anyway. The
  self-check still runs first, so a bad baseline never gets recorded.
