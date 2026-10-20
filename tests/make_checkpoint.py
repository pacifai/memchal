#!/usr/bin/env python3
"""Write a small synthetic checkpoint shaped like a Llama, for the weights tests.

Two safetensors shards, standard library only. Each of three layers has int8
q, k, v, o, gate, and up projections, which the manifest keeps, plus an fp8
down projection, a bf16 norm, and a bf16 embedding, which it leaves out. One
layer's k_proj sits in the other shard, so a stacked qkv_proj spans two files.

--zero NAME writes that tensor as zeros: a prover holding this copy holds
every byte of the right size except that tensor's, the `fake` cheat in
checkpoint form.

Example:
    python3 tests/make_checkpoint.py /tmp/wt/good
    python3 tests/make_checkpoint.py /tmp/wt/bad --zero model.layers.1.mlp.up_proj.weight
"""

from __future__ import annotations

import argparse
import json
import os
import random
import struct

HIDDEN, INTER, KV, LAYERS, VOCAB = 256, 512, 64, 3, 1000
ITEM = {"I8": 1, "F8_E4M3": 1, "BF16": 2}


def tensors() -> list[tuple[int, str, str, list[int]]]:
    """(shard, name, dtype, shape) for every tensor in the checkpoint."""
    out = []
    for i in range(LAYERS):
        layer = f"model.layers.{i}"
        shard = 0 if i < 2 else 1
        for proj, shape in [("q_proj", [HIDDEN, HIDDEN]), ("k_proj", [KV, HIDDEN]), ("v_proj", [KV, HIDDEN]),
                            ("o_proj", [HIDDEN, HIDDEN])]:
            s = 1 if (i == 1 and proj == "k_proj") else shard
            out.append((s, f"{layer}.self_attn.{proj}.weight", "I8", shape))
        out.append((shard, f"{layer}.mlp.gate_proj.weight", "I8", [INTER, HIDDEN]))
        out.append((shard, f"{layer}.mlp.up_proj.weight", "I8", [INTER, HIDDEN]))
        out.append((shard, f"{layer}.mlp.down_proj.weight", "F8_E4M3", [HIDDEN, INTER]))
        out.append((shard, f"{layer}.input_layernorm.weight", "BF16", [HIDDEN]))
    out.append((1, "model.embed_tokens.weight", "BF16", [VOCAB, HIDDEN]))
    return out


def write_safetensors(path: str, items: list[tuple[str, str, list[int], bytes]]) -> None:
    header, offset = {}, 0
    for name, dtype, shape, data in items:
        header[name] = {"dtype": dtype, "shape": shape, "data_offsets": [offset, offset + len(data)]}
        offset += len(data)
    raw = json.dumps(header).encode()
    raw += b" " * (-(8 + len(raw)) % 8)  # tensor data starts 8-byte aligned, as the format pads it
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(raw)))
        f.write(raw)
        for *_, data in items:
            f.write(data)


def main() -> None:
    p = argparse.ArgumentParser(description="Write a small Llama-shaped safetensors checkpoint.")
    p.add_argument("out_dir")
    p.add_argument("--zero", action="append", default=[], help="write this tensor as zeros")
    p.add_argument("--seed", type=int, default=7)
    args = p.parse_args()

    rng = random.Random(args.seed)
    shards: list[list] = [[], []]
    for shard, name, dtype, shape in tensors():
        n = ITEM[dtype]
        for d in shape:
            n *= d
        data = rng.randbytes(n)  # drawn even when zeroed, so other tensors don't shift
        if name in args.zero:
            data = bytes(n)
        shards[shard].append((name, dtype, shape, data))
    os.makedirs(args.out_dir, exist_ok=True)
    for i, items in enumerate(shards):
        write_safetensors(os.path.join(args.out_dir, f"model-0000{i + 1}-of-00002.safetensors"), items)


if __name__ == "__main__":
    main()
