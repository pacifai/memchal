# Next steps

Pending work, ordered by how much each changes what memchal can claim. What's
already built lives in the [README](README.md) and the
[decision records](docs/decisions/); this file holds only what's left to do.

## 1. A build-buffer, memory-hard, weight-bound generator (the frontier)

There are two ways to set up a check. In the **send-buffer** design, the verifier
sends the prover the whole buffer and keeps the seed. In the **build-buffer**
design, the verifier sends only a short recipe (a seed) and both sides build the
buffer from it.

The three ways to fake retention — recompute, prefetch, and precompute — and their
defenses are built and measured (see
[ADR 0004](docs/decisions/0004-adversary-taxonomy-and-defenses.md) and
[ADR 0005](docs/decisions/0005-per-lane-chaining-vs-gpu-parallelism.md)). Those
close the **send-buffer** design: because the buffer is crypto-random and the nonce
is unpredictable, the prover has to store every byte it was sent.

What's left is the design the parent goal needs:

- **From send-buffer to build-buffer.** Sending the buffer binds the check to the
  exact bytes you uploaded, but it doesn't scale — you upload the whole buffer every
  time. Building the buffer from a ~32-byte recipe scales, but handing the prover the
  recipe reopens the recompute attack unless the generator is **memory-hard** (Argon2-
  or scrypt-style): cheap to send, expensive to build, so a rational prover stores the
  result rather than regenerating it. That generator is the missing piece.
- **The frontier.** A build-buffer generator whose dataset is derived from the real
  model weights, so residency implies holding something tied to the actual workload
  without revealing it. That is proof-of-retrievability meeting proof-of-space for
  machine learning — a research problem, not a build task, and the destination this
  CPU version is clearing the way for.

## 2. A cold-read test on real hardware (the settling experiment)

The timing signal is validated only inside a Docker VM, whose storage is
virtualized through the host's page cache. The starved memory-mapped adversary runs
about 840× slower than honest with roughly half its reads faulting to storage, but
its budget is a container memory limit standing in for physical RAM, and the host
caches the virtual disk underneath — so that penalty is a lower bound.

Running on bare-metal Linux would settle it, with either:

- a buffer **larger than RAM**, so most probes force real page faults and the
  memory-mapped trick stops working; or
- `O_DIRECT` reads that bypass the page cache (mind the 512-byte / 4-KiB alignment
  against the 8-byte probe).

This is the one experiment that would show whether the timing detector has any
teeth against a serious adversary. The same bare-metal run would also give portable
absolute latencies: inside the VM, vCPU placement on performance vs efficiency
cores swings absolute time by up to ~1.6×, so today only within-session ratios are
trustworthy. `run.sh` already works on native Linux, so this is close to one
command once the larger-than-RAM (or `O_DIRECT`) mode exists.

## 3. A decision rule with stated error rates

The harness reports how far apart the modes sit, not a classifier. A real proof
needs a threshold that, given a response (time and correctness) and a claimed
configuration, returns accept or reject with measured false-positive and
false-negative rates. This depends on the timing regime from #2 being settled
first — otherwise you'd calibrate the detector against adversaries you already know
it can't catch.

## 4. The GPU port

The point of the parent project. The answer path is already GPU-shaped (a flat
buffer, no STL or exceptions, `answer_lane_t` as the per-thread kernel body). Port
`map_index`'s 128-bit multiply to `__umul64hi`, put the buffer in VRAM
(`cudaMalloc`, the non-swappable equivalent of `mlock`), and stage uploads through
pinned host memory. Settle the adversaries, the cold-read validation, and the
decision rule on CPU first.

## 5. Protocol hardening (before any production use)

- Make responses at least 128 bits with a cryptographic hash; 64 bits is guessable
  or precomputable against a real adversary.
- Make the unpredictable nonce (`--nonce random`) the default for any adversarial
  (non-benchmark) use. It's built, but currently opt-in.
- The wire format uses native byte order, which assumes both peers are the same
  endianness (true on loopback).
