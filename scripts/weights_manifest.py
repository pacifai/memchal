#!/usr/bin/env python3
"""Write a weights manifest: the buffer a weights challenge reads, as byte ranges of a checkpoint.

The challenge reads one flat buffer of 64-bit words. For model weights that
buffer is defined as the engine's weight tensors laid end to end, in name
order, each one as the engine holds it in memory. This script works that out
from a safetensors checkpoint without copying it: it reads only the file
headers and lists where each tensor's bytes sit. The verifier maps the
checkpoint and reads the words a challenge probes straight from it.

The engine doesn't always hold a tensor as the checkpoint stores it. vLLM
stacks a layer's q, k, and v projections into one qkv_proj tensor, and gate
and up into gate_up_proj, rows of each part in that order. A row-major stack
is the parts' bytes end to end, so a stacked tensor is just several pieces
under one name. `--fuse none` turns that off for an engine that doesn't stack.

Only tensors whose bytes the engine is known to keep as stored belong here.
The default takes the 2-D int8 tensors. Widen it with `--dtype` once a run
has shown which other tensors the engine leaves unchanged.

Example:
    python3 scripts/weights_manifest.py data/llama.weights \\
        model-00001-of-00002.safetensors model-00002-of-00002.safetensors
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

# How vLLM stacks a layer's projections, in row order.
FUSE_VLLM = {
    "self_attn.qkv_proj": ["self_attn.q_proj", "self_attn.k_proj", "self_attn.v_proj"],
    "mlp.gate_up_proj": ["mlp.gate_proj", "mlp.up_proj"],
}
_LAYER = re.compile(r"^(.*\.layers\.\d+)\.(.+)\.weight$")


def read_header(path: str) -> tuple[int, dict]:
    """The offset where tensor data starts, and the header's tensor entries."""
    with open(path, "rb") as f:
        raw = f.read(8)
        if len(raw) != 8:
            raise ValueError(f"{path}: too short to be a safetensors file")
        (n,) = struct.unpack("<Q", raw)
        header = json.loads(f.read(n))
    header.pop("__metadata__", None)
    return 8 + n, header


def collect(paths: list[str], dtypes: set[str]) -> dict[str, dict]:
    """Every selected tensor: its file index, absolute byte offset, length, and shape."""
    tensors: dict[str, dict] = {}
    for i, path in enumerate(paths):
        data_start, header = read_header(path)
        for name, t in header.items():
            if t["dtype"] not in dtypes or len(t["shape"]) != 2:
                continue
            begin, end = t["data_offsets"]
            if name in tensors:
                raise ValueError(f"{name} appears in more than one file")
            tensors[name] = {"file": i, "offset": data_start + begin, "bytes": end - begin, "shape": t["shape"]}
    return tensors


def segments(tensors: dict[str, dict], fuse: dict[str, list[str]]) -> list[tuple[str, list[dict]]]:
    """(name, pieces) for each tensor as the engine holds it, sorted by name."""
    left = dict(tensors)
    out: dict[str, list[dict]] = {}
    layers = sorted({m.group(1) for name in tensors if (m := _LAYER.match(name))})
    for layer in layers:
        for fused, parts in fuse.items():
            names = [f"{layer}.{p}.weight" for p in parts]
            if not all(n in left for n in names):
                continue
            pieces = [left.pop(n) for n in names]
            if len({p["shape"][1] for p in pieces}) != 1:
                raise ValueError(f"{layer}.{fused}: parts differ in their inner dimension")
            out[f"{layer}.{fused}.weight"] = pieces
    for name, t in left.items():
        out[name] = [t]
    return sorted(out.items())


def write_manifest(out_path: str, paths: list[str], segs: list[tuple[str, list[dict]]]) -> int:
    out_dir = os.path.dirname(os.path.abspath(out_path))
    total = 0
    lines = ["memchal-weights 1"]
    for i, p in enumerate(paths):
        rel = os.path.relpath(os.path.abspath(p), out_dir)
        lines.append(f"file {i} {rel}")
    for name, pieces in segs:
        for pc in pieces:
            # The challenge reads aligned 64-bit words, and a word can't
            # straddle two pieces held in different places.
            if pc["offset"] % 8 or pc["bytes"] % 8:
                raise ValueError(
                    f"{name}: a piece at offset {pc['offset']} of {pc['bytes']} bytes "
                    "is not whole 8-byte words at an 8-byte offset"
                )
            lines.append(f"piece {pc['file']} {pc['offset']} {pc['bytes']} {name}")
            total += pc["bytes"]
    lines.append(f"total {total}")
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return total


def main() -> None:
    p = argparse.ArgumentParser(description="Write a weights manifest from a safetensors checkpoint.")
    p.add_argument("out", help="manifest to write")
    p.add_argument("checkpoint", nargs="+", help="safetensors files")
    p.add_argument("--dtype", action="append", help="safetensors dtype to include (repeatable; default I8)")
    p.add_argument("--fuse", choices=["vllm", "none"], default="vllm", help="stack projections as vLLM does")
    args = p.parse_args()

    dtypes = set(args.dtype or ["I8"])
    try:
        segs = segments(collect(args.checkpoint, dtypes), FUSE_VLLM if args.fuse == "vllm" else {})
        if not segs:
            raise ValueError(f"no 2-D tensors of dtype {', '.join(sorted(dtypes))}")
        total = write_manifest(args.out, args.checkpoint, segs)
    except (ValueError, KeyError, OSError) as e:
        sys.exit(f"weights_manifest: {e}")
    pieces = sum(len(pcs) for _, pcs in segs)
    print(f"wrote {args.out}: {len(segs)} segments, {pieces} pieces, {total / 2**30:.3f} GiB")


if __name__ == "__main__":
    main()
