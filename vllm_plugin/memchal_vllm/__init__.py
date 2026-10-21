"""memchal's vLLM plugin: answer memory challenges over the weights where vLLM holds them.

vLLM loads this in every process through the `vllm.general_plugins` entry
point, before the model loads. It does nothing unless MEMCHAL_WEIGHTS_PORT is
set. Then, in the process that owns the GPU, it wraps two Worker methods:

- `load_model`, to collect the weight tensors once vLLM has loaded and
  post-processed them;
- `compile_or_warm_up_model`, to start serving only after warm-up. CUDA graph
  capture fails if another thread uses the GPU while it runs.

Before serving it checks every tensor, refuses anything it can't serve
honestly, and can compare every tensor's bytes with the checkpoint. Then it
hands the device pointers to libmemchal_weights.so, which answers challenges
from a thread of its own, on a CUDA stream of its own (ADR 0009).

A failure here is logged and leaves vLLM serving without challenges, which a
verifier sees as a prover that never answers. It never stops the engine.

Settings, from the environment:
  MEMCHAL_WEIGHTS_PORT      port to listen on; setting it turns the plugin on
  MEMCHAL_WEIGHTS_MANIFEST  the manifest the verifier uses; names and sizes must match
  MEMCHAL_CHECK_BYTES=1     compare every tensor's bytes with the checkpoint first
  MEMCHAL_DTYPES            torch dtypes to include, comma-separated (default torch.int8)
  MEMCHAL_LIB               path to libmemchal_weights.so (default: $MEMCHAL_ROOT or ~/memchal)
  MEMCHAL_LANE_CAP          lanes to preallocate scratch for (default 131072)
  MEMCHAL_ALLOW_UNLOCKED=1  serve even if a tensor isn't device memory: a cheat, for measuring
  MEMCHAL_TRACE_EVERY, MEMCHAL_TRACE  one trace line every N challenges, to this file
"""

from __future__ import annotations

import atexit
import ctypes
import logging
import multiprocessing
import os

from .weights import Refused, collect, compare, read_manifest

log = logging.getLogger("memchal")

_lib = None
_spans = None


def _in_gpu_process() -> bool:
    # With one GPU the worker runs inside EngineCore; with tensor parallelism
    # each rank is a VllmWorker process.
    name = multiprocessing.current_process().name or ""
    return name.startswith("EngineCore") or name.startswith("VllmWorker")


def register() -> None:
    if not os.environ.get("MEMCHAL_WEIGHTS_PORT") or not _in_gpu_process():
        return
    try:
        from vllm.logger import init_logger

        # Under vLLM's own logger, so the lines land where vLLM's do.
        global log
        log = init_logger("vllm.memchal")
    except Exception:
        pass
    from vllm.v1.worker import gpu_worker

    worker = gpu_worker.Worker
    if getattr(worker, "_memchal_wrapped", False):
        return
    load_model, warm_up = worker.load_model, worker.compile_or_warm_up_model

    def wrapped_load_model(self, *args, **kwargs):
        out = load_model(self, *args, **kwargs)
        try:
            _collect(self)
        except Exception as e:  # never take the engine down
            log.error("memchal: not serving: %s", e)
        return out

    def wrapped_warm_up(self, *args, **kwargs):
        out = warm_up(self, *args, **kwargs)
        try:
            _serve()
        except Exception as e:
            log.error("memchal: not serving: %s", e)
        return out

    worker.load_model = wrapped_load_model
    worker.compile_or_warm_up_model = wrapped_warm_up
    worker._memchal_wrapped = True
    log.info("memchal: will serve challenges on port %s after warm-up", os.environ["MEMCHAL_WEIGHTS_PORT"])


def _collect(worker) -> None:
    global _spans
    _spans = None
    if worker.vllm_config.model_config.enable_sleep_mode:
        raise Refused("sleep mode unmaps the weights; a challenge during sleep would fault")
    dtypes = {d.strip() for d in os.environ.get("MEMCHAL_DTYPES", "torch.int8").split(",") if d.strip()}
    spans = collect(worker.get_model().named_parameters(), dtypes)
    total = sum(s.nbytes for s in spans)
    offloaded = [s.name for s in spans if s.offloaded]
    log.info("memchal: collected %d tensors, %.3f GiB%s", len(spans), total / 2**30,
             f"; {len(offloaded)} offloaded to host memory" if offloaded else "")

    manifest = os.environ.get("MEMCHAL_WEIGHTS_MANIFEST")
    if manifest:
        segments = read_manifest(manifest)
        diffs = compare(spans, segments)
        if diffs:
            for d in diffs[:10]:
                log.error("memchal: manifest mismatch: %s", d)
            raise Refused(f"{len(diffs)} differences from {manifest}")
        if os.environ.get("MEMCHAL_CHECK_BYTES") == "1":
            _check_bytes(spans, segments)
    _spans = spans


def _check_bytes(spans, segments) -> None:
    """Compare every tensor's bytes in GPU memory with the checkpoint's, one at a time."""
    import torch

    bad = []
    for s, g in zip(spans, segments):
        t = s.tensor if s.tensor.is_contiguous() else s.tensor.t()
        held = t.reshape(-1).view(torch.uint8)
        want = bytearray()
        for path, off, n in g.pieces:
            with open(path, "rb") as f:
                f.seek(off)
                want += f.read(n)
        ref = torch.frombuffer(want, dtype=torch.uint8).to(held.device)
        if not torch.equal(held, ref):
            diff = (held != ref).nonzero()
            bad.append(f"{s.name}: {diff.numel()} of {s.nbytes} bytes differ, first at byte {int(diff[0])}")
        del ref
    if bad:
        for b in bad[:10]:
            log.error("memchal: bytes differ: %s", b)
        raise Refused(f"{len(bad)} of {len(spans)} tensors don't hold the checkpoint's bytes")
    log.info("memchal: every tensor holds the checkpoint's bytes (%d checked)", len(spans))


def _library() -> ctypes.CDLL:
    path = os.environ.get("MEMCHAL_LIB") or os.path.join(
        os.environ.get("MEMCHAL_ROOT", os.path.expanduser("~/memchal")), "libmemchal_weights.so"
    )
    lib = ctypes.CDLL(path)
    lib.mc_weights_start.argtypes = [
        ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint32,
        ctypes.c_uint16, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32, ctypes.c_char_p,
        ctypes.c_char_p, ctypes.c_size_t,
    ]
    lib.mc_weights_start.restype = ctypes.c_int
    lib.mc_weights_backing.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.mc_weights_backing.restype = ctypes.c_int
    lib.mc_weights_stop.argtypes = []
    lib.mc_weights_stop.restype = None
    return lib


def _serve() -> None:
    global _lib
    if not _spans:
        return  # collecting failed, and said why
    import torch

    allow = os.environ.get("MEMCHAL_ALLOW_UNLOCKED") == "1"
    if any(s.offloaded for s in _spans) and not allow:
        raise Refused("some tensors are offloaded to host memory (--cpu-offload-gb)")
    _lib = _library()
    n = len(_spans)
    bases = (ctypes.c_void_p * n)(*[s.ptr for s in _spans])
    words = (ctypes.c_uint64 * n)(*[s.nbytes // 8 for s in _spans])
    err = ctypes.create_string_buffer(512)
    port = int(os.environ["MEMCHAL_WEIGHTS_PORT"])
    rc = _lib.mc_weights_start(
        torch.cuda.current_device(), bases, words, n, port,
        int(os.environ.get("MEMCHAL_LANE_CAP", "131072")), int(allow),
        int(os.environ.get("MEMCHAL_TRACE_EVERY", "0")),
        os.environ.get("MEMCHAL_TRACE", "").encode() or None, err, len(err),
    )
    if rc != 0:
        raise Refused(err.value.decode(errors="replace"))
    backing = ctypes.create_string_buffer(256)
    _lib.mc_weights_backing(backing, len(backing))
    atexit.register(_lib.mc_weights_stop)
    log.info("memchal: serving %d tensors, %.3f GiB, on port %d; backing: %s", n,
             sum(s.nbytes for s in _spans) / 2**30, port, backing.value.decode(errors="replace"))
