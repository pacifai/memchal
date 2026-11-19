#!/usr/bin/env python3
"""Writes a small synthetic bf16 checkpoint shaped like a Llama, for the weights tests.

Two safetensors shards, standard library only. Each layer has q, k, v, o,
gate, up, and down projections and a 1-D norm. The output layer repeats the
embedding's bytes, as a tied model's checkpoint can. One layer's k_proj sits
in the other shard, so a stacked qkv_proj spans two files.

Usage: make_checkpoint.py OUT_DIR [--seed N]
"""
import argparse
import json
import os
import random
import struct

HIDDEN, INTER, KV, LAYERS, VOCAB = 128, 256, 32, 3, 500
BF16_BYTES = 2


def tensors():
    """(shard, name, shape) for every tensor in the checkpoint."""
    out = []
    for i in range(LAYERS):
        layer = f"model.layers.{i}"
        shard = 0 if i < 2 else 1
        for proj, shape in [("q_proj", [HIDDEN, HIDDEN]), ("k_proj", [KV, HIDDEN]),
                            ("v_proj", [KV, HIDDEN]), ("o_proj", [HIDDEN, HIDDEN])]:
            out.append((1 if (i == 1 and proj == "k_proj") else shard, f"{layer}.self_attn.{proj}.weight", shape))
        out.append((shard, f"{layer}.mlp.gate_proj.weight", [INTER, HIDDEN]))
        out.append((shard, f"{layer}.mlp.up_proj.weight", [INTER, HIDDEN]))
        out.append((shard, f"{layer}.mlp.down_proj.weight", [HIDDEN, INTER]))
        out.append((shard, f"{layer}.input_layernorm.weight", [HIDDEN]))
    out.append((1, "model.embed_tokens.weight", [VOCAB, HIDDEN]))
    return out


def write_safetensors(path, items):
    header, offset = {}, 0
    for name, shape, data in items:
        header[name] = {"dtype": "BF16", "shape": shape, "data_offsets": [offset, offset + len(data)]}
        offset += len(data)
    raw = json.dumps(header).encode()
    raw += b" " * (-(8 + len(raw)) % 8)
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(raw)))
        f.write(raw)
        for *_, data in items:
            f.write(data)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("out_dir")
    p.add_argument("--seed", type=int, default=7)
    args = p.parse_args()

    rng = random.Random(args.seed)
    shards = [[], []]
    for shard, name, shape in tensors():
        n = BF16_BYTES
        for d in shape:
            n *= d
        data = rng.randbytes(n)
        shards[shard].append((name, shape, data))
        if name == "model.embed_tokens.weight":
            shards[shard].append(("lm_head.weight", shape, data))
    os.makedirs(args.out_dir, exist_ok=True)
    for i, items in enumerate(shards):
        write_safetensors(os.path.join(args.out_dir, f"model-0000{i + 1}-of-00002.safetensors"), items)


if __name__ == "__main__":
    main()
