# ADR 0008: The buffer can start with the model's weights

**Status:** proposed. The CPU path is built and tested. The GPU path compiles,
and no GPU session has run it yet.

## Context

Until now, every buffer memchal challenged was filler: random data that the
verifier sends or that both sides build from a recipe. Proving the filler is
in fast memory shows that the memory it takes up isn't free for anything else.

A server that runs a model already holds the model's weights in fast memory.
If memchal also challenges the weights, they count toward the memory the
prover accounts for, and filler has to cover only the rest. The verifier
knows which model the prover declared, so it can compute the right answers
from its own copy of the checkpoint.

## Decision

The buffer is the model's weights followed by the filler. Each challenge reads
from anywhere in the whole buffer, so the weights and the filler are checked
at the same moment. A prover can't move the filler out while the weights are
being checked, or the other way around.

- A manifest lists the weights. `scripts/weights_manifest.py` reads a
  safetensors checkpoint's headers and lists the byte range of each 2-D tensor,
  in name order. Both sides read the weights from their own copy of the
  checkpoint in that order.
- The manifest leaves out tensors a prover could answer for without storing
  them: 1-D tensors such as norms, which are small and often close to constant,
  and any tensor whose bytes repeat another's, such as an output layer tied to
  the embedding.
- The verifier's hello carries a flag, followed by a Blake2b hash of the
  manifest and the number of weight bytes. A prover started with a different
  manifest, or with none, refuses the session.
- In build mode, the hash also goes into the filler's Argon2d recipe as
  associated data, so the filler is tied to the declared model.
- Each prover reads the weights into the start of its buffer, then receives or
  builds the filler after them. Because the buffer stays one flat array, the
  challenge math, the GPU kernels, and every existing attack work on it
  unchanged.

## What it proves

A correct answer proves nothing about the weights. They're public, so a
cheating prover can always produce the right bytes from a copy kept somewhere
else. Only timing catches that, as it does for the `partial` and `uvm`
attacks.

Unlike filler, weights compress. A prover that stores them compressed holds
less memory and still answers correctly, as long as it can decompress any word
quickly. The `compress` attack measures this. It stores each bf16 value's sign
and mantissa as one byte, and its exponent as a small offset from the lowest
exponent in its block of 256 values, so any word decodes with a few extra
reads and no sequential decoding. On synthetic bf16 weights it uses 76% of the
original size (`tests/attacks/weights_codec_test.cpp`). Lossless bf16
compressors reach about 70% on real models, but they decode in long runs, which
costs a cheat more per read.

Until a session shows whether timing catches `compress`, count the weights
only at the size the best attack has reached, and the remainder as memory that
isn't accounted for.

## What comes next

A real inference engine keeps each weight tensor in its own allocation, and the
honest prover should read them there instead of in a copy. An earlier
prototype did this inside vLLM: a table mapped each word of the buffer to a
tensor and an offset, so the challenge math stayed unchanged, and a plugin
answered challenges from a thread in the engine's process on its own CUDA
stream. That work, and checking that vLLM keeps each tensor's checkpoint bytes,
are open in [NEXT_STEPS](../../NEXT_STEPS.md).

## Alternatives considered

- Challenge the weights and the filler in separate sessions. It's simpler, but
  a prover could move one out of the way while the other is checked.
- Encode the weights with a slow, keyed function before challenging them, so
  they look random and can't be compressed. This closes the compression gap,
  but the engine would then have to decode every weight it reads during
  inference. It stays an option if `compress` passes.
- Hash every weight byte in each challenge. It covers every byte, but takes
  milliseconds per challenge instead of the sampled challenge's fraction of a
  millisecond.

## Related

[ADR 0003](0003-timing-detector-scope-and-measurement.md) (what timing shows),
[ADR 0004](0004-adversary-taxonomy-and-defenses.md) (the attacks), and
[ADR 0007](0007-buffer-generator-memory-hard-function.md) (the filler built
from a recipe).
