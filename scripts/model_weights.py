#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Writes a weights manifest: the byte ranges of a safetensors checkpoint that start the challenge buffer.

The buffer starts with the model's 2-D weight tensors laid end to end, in name
order. The script reads only the file headers to find where each tensor's
bytes sit, plus each tensor once to find duplicates.

It leaves out tensors whose bytes a prover could answer from without storing
them: 1-D tensors such as norms, which are small and often close to constant,
and any tensor whose bytes repeat another's, such as an output layer tied to
the embedding.

vLLM stacks a layer's q, k, and v projections into one qkv_proj tensor, and
gate and up into gate_up_proj. A row-major stack is the parts' bytes end to
end, so the manifest lists a stacked tensor as several pieces under one name,
in the order vLLM holds them. `--fuse none` turns this off.

Example:
    python3 scripts/weights_manifest.py models/qwen.weights models/qwen/*.safetensors
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from collections import Counter

FUSE_VLLM = {
    "self_attn.qkv_proj": ["self_attn.q_proj", "self_attn.k_proj", "self_attn.v_proj"],
    "mlp.gate_up_proj": ["mlp.gate_proj", "mlp.up_proj"],
}
LAYER = re.compile(r"^(.*\.layers\.\d+)\.(.+)\.weight$")
HASH_CHUNK = 1 << 24
WORD = 8


def read_header(path):
    """The offset where tensor data starts, and the header's tensor entries."""
    with open(path, "rb") as f:
        raw = f.read(8)
        if len(raw) != 8:
            raise ValueError(f"{path}: too short to be a safetensors file")
        (n,) = struct.unpack("<Q", raw)
        header = json.loads(f.read(n))
    header.pop("__metadata__", None)
    return 8 + n, header


def collect(paths, dtypes):
    """Every 2-D tensor of the given dtypes: its file index, byte offset, and length."""
    tensors = {}
    for i, path in enumerate(paths):
        data_start, header = read_header(path)
        for name, t in header.items():
            if t["dtype"] not in dtypes or len(t["shape"]) != 2:
                continue
            if name in tensors:
                raise ValueError(f"{name} appears in more than one file")
            begin, end = t["data_offsets"]
            tensors[name] = {"file": i, "offset": data_start + begin, "bytes": end - begin}
    return tensors


def digest(path, t):
    h = hashlib.blake2b(digest_size=32)
    with open(path, "rb") as f:
        f.seek(t["offset"])
        left = t["bytes"]
        while left:
            chunk = f.read(min(left, HASH_CHUNK))
            if not chunk:
                raise ValueError(f"{path} ends inside a tensor")
            h.update(chunk)
            left -= len(chunk)
    return h.digest()


def drop_duplicates(paths, tensors):
    """Keeps the first tensor, in name order, of each set with identical bytes."""
    sizes = Counter(t["bytes"] for t in tensors.values())
    seen, kept, dropped = set(), {}, []
    for name in sorted(tensors):
        t = tensors[name]
        if sizes[t["bytes"]] == 1:
            kept[name] = t
            continue
        d = digest(paths[t["file"]], t)
        if d in seen:
            dropped.append(name)
            continue
        seen.add(d)
        kept[name] = t
    return kept, dropped


def stack(tensors, fuse):
    """(name, pieces) for each tensor as the engine holds it, sorted by name."""
    left = dict(tensors)
    out = {}
    layers = sorted({m.group(1) for name in tensors if (m := LAYER.match(name))})
    for layer in layers:
        for fused, parts in fuse.items():
            names = [f"{layer}.{p}.weight" for p in parts]
            if all(n in left for n in names):
                out[f"{layer}.{fused}.weight"] = [left.pop(n) for n in names]
    for name, t in left.items():
        out[name] = [t]
    return sorted(out.items())


def write_manifest(out_path, paths, segments):
    out_dir = os.path.dirname(os.path.abspath(out_path))
    lines = ["memchal-weights 1"]
    lines += [f"file {i} {os.path.relpath(os.path.abspath(p), out_dir)}" for i, p in enumerate(paths)]
    total = 0
    for name, pieces in segments:
        for pc in pieces:
            if pc["offset"] % WORD or pc["bytes"] % WORD:
                raise ValueError(f"{name}: {pc['bytes']} bytes at offset {pc['offset']} aren't whole 8-byte words")
            lines.append(f"piece {pc['file']} {pc['offset']} {pc['bytes']} {name}")
            total += pc["bytes"]
    lines.append(f"total {total}")
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return total


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("out", help="the manifest to write")
    p.add_argument("checkpoint", nargs="+", help="the checkpoint's safetensors files")
    p.add_argument("--dtype", action="append", help="a safetensors dtype to include; repeatable (default BF16)")
    p.add_argument("--fuse", choices=["vllm", "none"], default="vllm", help="stack projections as vLLM does")
    args = p.parse_args()

    dtypes = set(args.dtype or ["BF16"])
    try:
        tensors, dropped = drop_duplicates(args.checkpoint, collect(args.checkpoint, dtypes))
        segments = stack(tensors, FUSE_VLLM if args.fuse == "vllm" else {})
        if not segments:
            raise ValueError(f"no 2-D tensors of dtype {', '.join(sorted(dtypes))}")
        total = write_manifest(args.out, args.checkpoint, segments)
    except (ValueError, KeyError, OSError) as e:
        sys.exit(f"weights_manifest: {e}")
    for name in dropped:
        print(f"left out {name}: its bytes repeat another tensor's")
    print(f"wrote {args.out}: {len(segments)} tensors, {total / 2**30:.3f} GiB")


if __name__ == "__main__":
    main()
