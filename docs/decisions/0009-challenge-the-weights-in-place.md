# ADR 0009 — Challenge the model's weights where the inference engine holds them

**Status:** proposed. The CPU path is implemented and tested: the manifest,
the segment table, the no-upload handshake, and the CPU prover holding the
weights as separate allocations. The GPU path is written but has not run on a
GPU: the attached kernel path (`gpu::attach`), `prover_cuda --weights`,
`libmemchal_weights.so`, the vLLM plugin (`vllm_plugin/`), and
`scripts/gpu_weights_session.sh`. Off a GPU, the host code compiles and the
plugin's logic passes its tests against fakes. The CUDA file itself has not
been compiled.

**Amends:** the roadmap note that set "challenging the weights directly" aside
as out of scope ([NEXT_STEPS](../../NEXT_STEPS.md)).

## Context

Every buffer memchal has challenged so far is filler. The send-buffer design
uploads random bytes. The build-buffer design ([ADR 0007](0007-buffer-generator-memory-hard-function.md))
generates them from a recipe. Either way, proving the filler is resident shows
that the memory it occupies is not free for anything else. That's the point of
filler, and it costs the prover that much memory.

A model's weights are the opposite case. They're memory the workload already
uses. Proving the weights are resident accounts for that part of the card at no
extra memory cost, and filler only has to cover what's left over.

The roadmap set this aside because it needs cooperation from whoever runs the
model: something has to run next to the weights and answer for them. In
practice that's one plugin in the inference engine. This record decides it's
worth that.

## Decision

**1. The buffer is the engine's weight tensors laid end to end.** Order is by
tensor name, and each tensor is laid out the way the engine holds it.
`scripts/weights_manifest.py` works this out from a safetensors checkpoint
without copying it. It reads the file headers and lists byte ranges ("pieces").
It also follows vLLM's stacking of `q/k/v` into `qkv_proj` and `gate/up` into
`gate_up_proj`: a row-major stack is just the parts' bytes in a row, so a
stacked tensor is several pieces under one name.

**2. The challenge math doesn't change.** A `Segments` table
(`src/segments.hpp`) turns a word index into a piece and an offset, through a
directory with one entry per chunk. `answer_lane_t()` takes it as one more
loader. `tests/segments_test.cpp` checks that every index reads the word the
flat concatenation holds there. It also checks that every answer, in both
chain modes and both folds, equals the flat answer the known-answer test pins.

**3. Nothing is uploaded.** A Hello flag, `kHelloResident`, says the prover
already holds the buffer. The prover answers Ready only if what it holds is the
expected size. The verifier's reference copy is the checkpoint itself, mapped
read-only, so it reads only the words a challenge probes.

**4. The prover reads the tensors where they are.** A copy made for the
challenge would cost an honest prover the model's size a second time. Inside
vLLM, that means a plugin of its own:
- **A separate package.** It has its own `vllm.general_plugins` entry point and
  depends on no other plugin.
- **Collecting the tensors.** It gathers the weight tensors after loading,
  checks each one's memory type (the [ADR 0008](0008-gpu-residency-check-substitutes-for-mlock.md)
  self-check), and builds a `Segments` table on the device.
- **Serving.** It answers challenges from a thread in the process that owns the
  GPU, on its own non-blocking CUDA stream. A device-wide sync would make the
  challenge wait for inference, and inference wait for the challenge.
  `libmemchal_weights.so` holds the thread and the kernel. The plugin hands it
  the device pointers through ctypes.

**5. Start with the 2-D int8 tensors.** For these, vLLM's int8 path keeps the
checkpoint's bytes: its CUTLASS kernel stores a transposed view of the loaded
tensor, not a repacked copy. Other dtypes join only once a run has compared
each tensor's bytes in GPU memory with the checkpoint.

## What it proves, and what it doesn't

- **The same as for filler.** The probed words were in fast memory when the
  challenge ran. The resolution is what the GPU sweep measured: about 5% of a
  4 GiB buffer kept off the card
  ([partial-residency-resolution](../investigations/partial-residency-resolution/README.md)),
  which is almost exactly the size of the int8 weights of an 8B model.
- **Correctness alone proves nothing here.** The weights are public, so a
  prover can always produce the right bytes from a copy somewhere else. Only
  timing catches that, the same shift ADR 0007 describes for build-buffer.
- **Not that the weights are stored as dense checkpoint bytes.** A prover that
  keeps them compressed or sparse, and can rebuild any probed word fast enough,
  passes too, and holds less memory than the dense size. Filler is
  incompressible by construction. Weights aren't, and how much they compress
  is the next open question. A proof that the engine multiplied by these
  weights doesn't close this gap. It shows what went into each multiply, not
  how the weights sit in memory between multiplies.
- **Not that inference reads this copy.** A prover could keep the challenged
  copy and run inference from another. That costs it the memory, which is what
  this check accounts for, so it doesn't help a prover that wants memory back.

## What vLLM 0.20.2 imposes

Read from the source at tag v0.20.2. None of it has been tried on a GPU.

- **There's no post-load hook.** General plugins load in every process, before
  the model loads (`load_general_plugins`, called in `EngineCore.__init__` and
  `WorkerWrapperBase.init_worker`). There are two ways in:
  - re-register the default model loader (`register_model_loader`) with a
    subclass that captures the tensors after `load_model`;
  - wrap `vllm.v1.worker.gpu_worker.Worker.load_model`, which covers both model
    runners.

  The plan is the wrap. It should start serving only after warm-up
  (`compile_or_warm_up_model`): during CUDA graph capture, in torch's default
  mode, CUDA calls from other threads fail.
- **Where the tensors are.** With one GPU, the worker runs inside the EngineCore
  process. With tensor parallelism, each rank is its own process holding a
  shard, so each GPU needs its own prover and its own manifest. The weights are
  module `Parameter`s, reachable through `get_model()`, which unwraps the CUDA
  graph wrapper. Some are transposed views, so the prover reads each tensor's
  memory span, not its logical order.
- **CPU offload (`--cpu-offload-gb`).** Offloaded parameters report `is_cuda`
  but point at pinned host memory over PCIe. The memory-type self-check refuses
  them in an honest prover. This is also a cheat vLLM ships ready-made, and
  worth measuring.
- **Sleep mode.** It unmaps the weights. A kernel reading them during sleep
  would fault and take the engine's CUDA context down with it. Version 1
  refuses to serve when sleep mode is on.

## Alternatives considered

- **Copy the weights into a buffer memchal owns.** No plugin is needed for the
  bytes, but it costs the model's size again. That defeats the purpose, so it's
  rejected.
- **Hash every weight byte per challenge.** This covers every byte every time.
  But it's bandwidth-bound and takes milliseconds per challenge, against about
  0.2 ms for the sampled challenge. Checking it costs the verifier a full pass
  over the weights. Rejected while the sampled challenge's resolution holds up.
- **Map vLLM's tensors into memchal's own GPU prover process over CUDA IPC.**
  This keeps memchal out of vLLM, so a fault in memchal can't crash the engine.
  But kernels from two processes share the GPU by time-slicing unless MPS is
  running, which would add the engine's time slice to the honest timing. IPC
  also fails for allocations made through CUDA's virtual memory API. Kept as
  the fallback if the in-process thread proves fragile.

## Consequences

- **Wire.** There's one new Hello flag. An older prover ignores it and waits
  for an upload that never comes.
- **The verifier needs the checkpoint.** It needs read access to the same files
  the manifest names, and the manifest has to describe what the engine
  actually holds.
- **Filler still covers the rest of the card.** The weights and the filler have
  to be challenged at the same moment, so the same memory can't answer for
  both. That's a later protocol step.
- **Each probe costs a little more.** Every probe reads one directory entry. At
  16 MiB chunks, 4.4 GiB of weights needs about 300 entries, small enough to
  stay in L2. The honest baseline includes this cost, so the comparison stays
  fair.

## Related

[ADR 0001](0001-challenge-math-frozen-pinned-by-known-answer.md) (the math this
leaves alone), [ADR 0007](0007-buffer-generator-memory-hard-function.md) (filler
tied to the model, the complementary route), [ADR 0008](0008-gpu-residency-check-substitutes-for-mlock.md)
(the self-check the vLLM prover runs on each tensor).
