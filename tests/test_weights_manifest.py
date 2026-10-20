#!/usr/bin/env python3
"""Tests for scripts/weights_manifest.py: what the weights buffer is, piece by piece."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, HERE)

import make_checkpoint  # noqa: E402
import weights_manifest  # noqa: E402


def read(path: str) -> tuple[list[str], list[tuple[int, int, int, str]], int]:
    files, pieces, total = [], [], None
    with open(path) as f:
        assert f.readline().strip() == "memchal-weights 1"
        for line in f:
            kind, rest = line.rstrip("\n").split(" ", 1)
            if kind == "file":
                files.append(rest.split(" ", 1)[1])
            elif kind == "piece":
                fi, off, n, name = rest.split(" ", 3)
                pieces.append((int(fi), int(off), int(n), name))
            elif kind == "total":
                total = int(rest)
    return files, pieces, total


class ManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        subprocess.run([sys.executable, os.path.join(HERE, "make_checkpoint.py"), os.path.join(self.dir, "ckpt")],
                       check=True)
        self.shards = sorted(os.path.join(self.dir, "ckpt", f) for f in os.listdir(os.path.join(self.dir, "ckpt")))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def manifest(self, *extra: str) -> subprocess.CompletedProcess:
        out = os.path.join(self.dir, "w.weights")
        return subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "weights_manifest.py"), out,
                               *self.shards, *extra], capture_output=True, text=True)

    def test_stacks_like_vllm_in_name_order(self) -> None:
        self.assertEqual(self.manifest().returncode, 0)
        files, pieces, total = read(os.path.join(self.dir, "w.weights"))
        names = [p[3] for p in pieces]
        segments = list(dict.fromkeys(names))
        self.assertEqual(segments, sorted(segments))
        self.assertIn("model.layers.0.self_attn.qkv_proj.weight", segments)
        self.assertIn("model.layers.0.mlp.gate_up_proj.weight", segments)
        self.assertNotIn("model.layers.0.self_attn.q_proj.weight", segments)
        # Three pieces under qkv_proj, q then k then v by size, and two under gate_up.
        qkv = [p for p in pieces if p[3] == "model.layers.0.self_attn.qkv_proj.weight"]
        self.assertEqual([p[2] for p in qkv], [256 * 256, 64 * 256, 64 * 256])
        self.assertEqual(names.count("model.layers.0.mlp.gate_up_proj.weight"), 2)
        self.assertEqual(total, sum(p[2] for p in pieces))
        self.assertEqual(total, make_checkpoint.LAYERS * (256 * 256 + 2 * 64 * 256 + 256 * 256 + 2 * 512 * 256))

    def test_a_stacked_tensor_can_span_files(self) -> None:
        self.assertEqual(self.manifest().returncode, 0)
        _, pieces, _ = read(os.path.join(self.dir, "w.weights"))
        qkv1 = [p for p in pieces if p[3] == "model.layers.1.self_attn.qkv_proj.weight"]
        self.assertEqual([p[0] for p in qkv1], [0, 1, 0])

    def test_leaves_out_other_dtypes_and_vectors(self) -> None:
        self.assertEqual(self.manifest().returncode, 0)
        _, pieces, _ = read(os.path.join(self.dir, "w.weights"))
        names = {p[3] for p in pieces}
        self.assertFalse(any("down_proj" in n or "layernorm" in n or "embed" in n for n in names))

    def test_pieces_point_at_the_tensor_bytes(self) -> None:
        self.assertEqual(self.manifest().returncode, 0)
        files, pieces, _ = read(os.path.join(self.dir, "w.weights"))
        start, header = weights_manifest.read_header(self.shards[0])
        o = header["model.layers.0.self_attn.o_proj.weight"]
        piece = next(p for p in pieces if p[3] == "model.layers.0.self_attn.o_proj.weight")
        self.assertEqual(piece[1], start + o["data_offsets"][0])
        self.assertEqual(os.path.normpath(os.path.join(self.dir, files[piece[0]])), self.shards[piece[0]])

    def test_fuse_none_keeps_checkpoint_names(self) -> None:
        self.assertEqual(self.manifest("--fuse", "none").returncode, 0)
        _, pieces, _ = read(os.path.join(self.dir, "w.weights"))
        names = {p[3] for p in pieces}
        self.assertIn("model.layers.0.self_attn.q_proj.weight", names)
        self.assertFalse(any("qkv_proj" in n for n in names))

    def test_refuses_pieces_that_are_not_whole_words(self) -> None:
        bad = os.path.join(self.dir, "odd.safetensors")
        make_checkpoint.write_safetensors(bad, [("model.layers.0.mlp.up_proj.weight", "I8", [3, 3], bytes(9))])
        out = os.path.join(self.dir, "odd.weights")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "weights_manifest.py"), out, bad],
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("8-byte", r.stderr)


if __name__ == "__main__":
    unittest.main()
