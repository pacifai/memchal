#!/usr/bin/env python3
"""Checks the manifest weights_manifest.py writes for the synthetic checkpoint.

Usage: check_manifest.py MANIFEST
"""
import sys

import make_checkpoint as ck


def main():
    lines = open(sys.argv[1]).read().splitlines()
    pieces = [line.split(maxsplit=4) for line in lines if line.startswith("piece ")]
    names = [p[4] for p in pieces]
    total = int(next(line for line in lines if line.startswith("total ")).split()[1])
    fail = []

    if any("layernorm" in n for n in names):
        fail.append("a 1-D norm is in the manifest")
    if ("lm_head.weight" in names) == ("model.embed_tokens.weight" in names):
        fail.append("lm_head repeats the embedding, so exactly one of the two belongs in the manifest")
    qkv = [p for p in pieces if p[4] == "model.layers.1.self_attn.qkv_proj.weight"]
    if [p[1] for p in qkv] != ["0", "1", "0"]:
        fail.append(f"layer 1's qkv_proj should take q, k, v from files 0, 1, 0, not {[p[1] for p in qkv]}")
    if names != sorted(names):
        fail.append("tensors are not in name order")

    two_d = [shape for _, _, shape in ck.tensors() if len(shape) == 2]
    want = sum(r * c * ck.BF16_BYTES for r, c in two_d)
    if total != want or sum(int(p[3]) for p in pieces) != want:
        fail.append(f"total is {total} bytes, expected {want}")

    for f in fail:
        print(f"FAIL: {f}")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
