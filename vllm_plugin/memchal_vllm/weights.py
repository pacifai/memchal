"""Which of a model's tensors make up the challenge buffer, and where their bytes are.

Plain Python over duck-typed tensors, so it can be tested without torch or vLLM.
A tensor here needs only what torch tensors have: `dtype`, `dim()`,
`data_ptr()`, `numel()`, `element_size()`, `is_contiguous()`, `t()`, and `is_cuda`.

The buffer is the selected tensors in name order, each one's memory span as
vLLM holds it. vLLM stores some int8 weights as a transposed view of the
loaded tensor, so a tensor's span is its storage, read in memory order, not
its logical order. A tensor that is neither contiguous nor a transposed view
of a contiguous one has no single span, and is refused.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field


class Refused(Exception):
    """The weights can't be served honestly as they are."""


@dataclass
class Span:
    name: str
    ptr: int
    nbytes: int
    # vLLM's CPU weight offload marks the parameters it moved to pinned host
    # memory. They still read correctly, over PCIe.
    offloaded: bool = False
    tensor: object = field(default=None, repr=False, compare=False)


def span(name: str, t) -> Span:
    if not getattr(t, "is_cuda", False):
        raise Refused(f"{name} is not on the GPU")
    if not (t.is_contiguous() or t.t().is_contiguous()):
        raise Refused(f"{name} is not one dense span of memory")
    ptr, nbytes = t.data_ptr(), t.numel() * t.element_size()
    if ptr % 8 or nbytes % 8:
        raise Refused(f"{name} is not whole 8-byte words at an 8-byte address")
    return Span(name, ptr, nbytes, bool(getattr(t, "_vllm_is_uva_offloaded", False)), t)


def collect(named_parameters, dtypes: set[str]) -> list[Span]:
    """Every 2-D parameter of a selected dtype, in name order."""
    spans = [span(name, t) for name, t in named_parameters if str(t.dtype) in dtypes and t.dim() == 2]
    if not spans:
        raise Refused(f"the model has no 2-D parameters of dtype {', '.join(sorted(dtypes))}")
    return sorted(spans, key=lambda s: s.name)


@dataclass
class Segment:
    name: str
    nbytes: int
    pieces: list[tuple[str, int, int]]  # (file, offset, bytes)


def read_manifest(path: str) -> list[Segment]:
    """The segments a memchal-weights 1 manifest lists, in order (see src/weights.hpp)."""
    base = os.path.dirname(os.path.abspath(path))
    files: list[str] = []
    segments: list[Segment] = []
    with open(path) as f:
        if f.readline().strip() != "memchal-weights 1":
            raise ValueError(f"{path}: not a memchal-weights 1 manifest")
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            kind, rest = line.split(" ", 1)
            if kind == "file":
                p = rest.split(" ", 1)[1]
                files.append(p if os.path.isabs(p) else os.path.join(base, p))
            elif kind == "piece":
                fi, off, n, name = rest.split(" ", 3)
                if not segments or segments[-1].name != name:
                    segments.append(Segment(name, 0, []))
                segments[-1].pieces.append((files[int(fi)], int(off), int(n)))
                segments[-1].nbytes += int(n)
    return segments


def compare(spans: list[Span], segments: list[Segment]) -> list[str]:
    """Where the tensors differ from the manifest in name or size; empty if they match."""
    diffs = []
    if len(spans) != len(segments):
        diffs.append(f"the model has {len(spans)} tensors, the manifest {len(segments)}")
    for s, g in zip(spans, segments):
        if s.name != g.name:
            diffs.append(f"{s.name} where the manifest has {g.name}")
        elif s.nbytes != g.nbytes:
            diffs.append(f"{s.name} is {s.nbytes} bytes, the manifest says {g.nbytes}")
    model, listed = {s.name for s in spans}, {g.name for g in segments}
    diffs += [f"{n} is in the model, not the manifest" for n in sorted(model - listed)]
    diffs += [f"{n} is in the manifest, not the model" for n in sorted(listed - model)]
    return diffs
