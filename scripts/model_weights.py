#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Prepares a model's weights to start the challenge buffer.

Two commands:

  fetch     Downloads a Hugging Face model's safetensors files and writes their
            manifest. Works only for models that need no login.
  manifest  Writes the manifest for safetensors files you already have.

A manifest lists the byte ranges of a checkpoint that start the buffer: the
model's 2-D weight tensors laid end to end, in name order. Writing one reads
only the file headers, plus the tensors whose size another tensor shares, to
find duplicates.

It leaves out tensors whose bytes a prover could answer from without storing
them: 1-D tensors such as norms, which are small and often close to constant,
and any tensor whose bytes repeat another's, such as an output layer tied to
the embedding.

vLLM stacks a layer's q, k, and v projections into one qkv_proj tensor, and
gate and up into gate_up_proj. A row-major stack is the parts' bytes end to
end, so the manifest lists a stacked tensor as several pieces under one name,
in the order vLLM holds them. `--fuse none` turns this off.

`fetch` takes the files vLLM loads: the top-level ones, and only those the
index lists when there is one. It pins them to the repository's current
commit, which it records in the folder as `revision`, and keeps a file already
present with the right size.

Examples:
    python3 scripts/model_weights.py fetch Qwen/Qwen2.5-7B-Instruct models/qwen
    python3 scripts/model_weights.py manifest models/qwen.weights models/qwen/*.safetensors
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import urllib.request
from collections import Counter
from pathlib import Path

FUSE_VLLM = {
    "self_attn.qkv_proj": ["self_attn.q_proj", "self_attn.k_proj", "self_attn.v_proj"],
    "mlp.gate_up_proj": ["mlp.gate_proj", "mlp.up_proj"],
}
LAYER = re.compile(r"^(.*\.layers\.\d+)\.(.+)\.weight$")
HASH_CHUNK = 1 << 24
WORD = 8
HUB = "https://huggingface.co"
DOWNLOAD_CHUNK = 1 << 24
MANIFEST = "model.weights"
INDEX = "model.safetensors.index.json"


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


def make_manifest(out, checkpoint, dtypes=("BF16",), fuse="vllm"):
    """Writes the manifest for the checkpoint's files and prints what it left out."""
    tensors, dropped = drop_duplicates(checkpoint, collect(checkpoint, set(dtypes)))
    segments = stack(tensors, FUSE_VLLM if fuse == "vllm" else {})
    if not segments:
        raise ValueError(f"no 2-D tensors of dtype {', '.join(sorted(dtypes))}")
    total = write_manifest(out, checkpoint, segments)
    for name in dropped:
        print(f"left out {name}: its bytes repeat another tensor's")
    print(f"wrote {out}: {len(segments)} tensors, {total / 2**30:.3f} GiB")


def hub_files(repo):
    """The commit and (name, size) of each safetensors file vLLM loads at the repository's head."""
    with urllib.request.urlopen(f"{HUB}/api/models/{repo}?blobs=true") as r:
        info = json.load(r)
    names = {s["rfilename"] for s in info["siblings"]}
    found = [(s["rfilename"], s["size"]) for s in info["siblings"]
             if s["rfilename"].endswith(".safetensors") and "/" not in s["rfilename"]]
    if INDEX in names:
        with urllib.request.urlopen(f"{HUB}/{repo}/resolve/{info['sha']}/{INDEX}") as r:
            listed = set(json.load(r)["weight_map"].values())
        found = [(name, size) for name, size in found if name in listed]
    if not found:
        raise ValueError(f"{repo} has no safetensors files")
    return info["sha"], sorted(found)


def download(repo, sha, name, size, dest):
    if dest.exists() and dest.stat().st_size == size:
        print(f"have {name}")
        return
    print(f"downloading {name} ({size / 2**30:.2f} GiB)", flush=True)
    part = dest.with_suffix(".part")
    with urllib.request.urlopen(f"{HUB}/{repo}/resolve/{sha}/{name}") as r, part.open("wb") as f:
        while chunk := r.read(DOWNLOAD_CHUNK):
            f.write(chunk)
    if part.stat().st_size != size:
        raise ValueError(f"{name}: got {part.stat().st_size} bytes, expected {size}")
    part.rename(dest)


def fetch(repo, folder):
    """Downloads the model into folder, writes its manifest there, and returns the manifest's path."""
    folder = Path(folder)
    sha, found = hub_files(repo)
    folder.mkdir(parents=True, exist_ok=True)
    print(f"{repo} at commit {sha}")
    for name, size in found:
        download(repo, sha, name, size, folder / name)
    (folder / "revision").write_text(f"{repo} {sha}\n")
    out = folder / MANIFEST
    make_manifest(str(out), [str(folder / name) for name, _ in found])
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    f = sub.add_parser("fetch", help="download a Hugging Face model and write its manifest")
    f.add_argument("repo", help="the model's repository, such as Qwen/Qwen2.5-7B-Instruct")
    f.add_argument("dir", type=Path, help=f"the folder to download into; the manifest is <dir>/{MANIFEST}")
    m = sub.add_parser("manifest", help="write the manifest for safetensors files you already have")
    m.add_argument("out", help="the manifest to write")
    m.add_argument("checkpoint", nargs="+", help="the checkpoint's safetensors files")
    m.add_argument("--dtype", action="append", help="a safetensors dtype to include; repeatable (default BF16)")
    m.add_argument("--fuse", choices=["vllm", "none"], default="vllm", help="stack projections as vLLM does")
    args = p.parse_args()

    try:
        if args.command == "fetch":
            fetch(args.repo, args.dir)
        else:
            make_manifest(args.out, args.checkpoint, args.dtype or ["BF16"], args.fuse)
    except (ValueError, KeyError, OSError) as e:
        sys.exit(f"model_weights: {e}")


if __name__ == "__main__":
    main()
