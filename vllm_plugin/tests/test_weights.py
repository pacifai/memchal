#!/usr/bin/env python3
"""Tests for the plugin's choice of tensors, on fakes: no torch, no vLLM, no GPU."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests"))

from memchal_vllm.weights import Refused, collect, compare, read_manifest, span  # noqa: E402


class Fake:
    """Just enough of a torch tensor: a shape, a dtype, an address, a layout."""

    def __init__(self, rows, cols, dtype="torch.int8", ptr=0x1000, contiguous=True, item=1, cuda=True, **attrs):
        self.shape, self.dtype, self.ptr, self.contig, self.item, self.is_cuda = (rows, cols), dtype, ptr, contiguous, item, cuda
        self.__dict__.update(attrs)

    def dim(self):
        return len([d for d in self.shape if d is not None])

    def data_ptr(self):
        return self.ptr

    def numel(self):
        return self.shape[0] * (self.shape[1] or 1)

    def element_size(self):
        return self.item

    def is_contiguous(self):
        return self.contig

    def t(self):
        # The transpose of a transposed view is the contiguous original.
        return Fake(self.shape[1], self.shape[0], self.dtype, self.ptr, not self.contig, self.item, self.is_cuda)


class Vector(Fake):
    def dim(self):
        return 1


class CollectTest(unittest.TestCase):
    def test_takes_2d_int8_in_name_order(self):
        params = [
            ("model.layers.1.mlp.down_proj.weight", Fake(64, 128, ptr=0x3000)),
            ("model.layers.0.mlp.down_proj.weight", Fake(64, 128, ptr=0x2000)),
            ("model.layers.0.input_layernorm.weight", Vector(64, None, dtype="torch.int8")),
            ("model.embed_tokens.weight", Fake(1000, 64, dtype="torch.bfloat16", item=2)),
        ]
        spans = collect(params, {"torch.int8"})
        self.assertEqual([s.name for s in spans],
                         ["model.layers.0.mlp.down_proj.weight", "model.layers.1.mlp.down_proj.weight"])
        self.assertEqual([s.nbytes for s in spans], [64 * 128, 64 * 128])

    def test_a_transposed_view_is_its_storage(self):
        # vLLM's CUTLASS int8 path stores Parameter(weight.t()): not contiguous,
        # but its transpose is, and its address is the storage's.
        s = span("w", Fake(4096, 1024, contiguous=False, ptr=0x8000))
        self.assertEqual((s.ptr, s.nbytes), (0x8000, 4096 * 1024))

    def test_refuses_what_it_cannot_serve(self):
        class Strided(Fake):
            def t(self):
                return Fake(self.shape[1], self.shape[0], contiguous=False)

        with self.assertRaises(Refused):
            span("strided", Strided(8, 8, contiguous=False))
        with self.assertRaises(Refused):
            span("misaligned", Fake(8, 8, ptr=0x1004))
        with self.assertRaises(Refused):
            span("odd", Fake(3, 3))
        with self.assertRaises(Refused):
            span("cpu", Fake(8, 8, cuda=False))
        with self.assertRaises(Refused):
            collect([("only bf16", Fake(8, 8, dtype="torch.bfloat16", item=2))], {"torch.int8"})

    def test_marks_offloaded_parameters(self):
        s = span("w", Fake(8, 8, _vllm_is_uva_offloaded=True))
        self.assertTrue(s.offloaded)
        self.assertFalse(span("w", Fake(8, 8)).offloaded)


class ManifestTest(unittest.TestCase):
    """The plugin reads the same manifest the verifier does, and must agree on it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        ckpt = os.path.join(self.tmp.name, "ckpt")
        subprocess.run([sys.executable, os.path.join(ROOT, "tests", "make_checkpoint.py"), ckpt], check=True)
        self.manifest = os.path.join(self.tmp.name, "w.weights")
        shards = sorted(os.path.join(ckpt, f) for f in os.listdir(ckpt))
        subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "weights_manifest.py"), self.manifest, *shards],
                       check=True, capture_output=True)

    def tearDown(self):
        self.tmp.cleanup()

    def model_like_vllm(self):
        """Parameters named and sized the way vLLM holds the synthetic checkpoint."""
        params, ptr = [], 0x10000
        for i in range(3):
            for name, rows in [("self_attn.qkv_proj", 256 + 64 + 64), ("self_attn.o_proj", 256),
                               ("mlp.gate_up_proj", 512 + 512)]:
                params.append((f"model.layers.{i}.{name}.weight", Fake(rows, 256, ptr=ptr, contiguous=False)))
                ptr += rows * 256
        return params

    def test_segments_follow_pieces(self):
        segs = read_manifest(self.manifest)
        qkv = next(g for g in segs if g.name == "model.layers.1.self_attn.qkv_proj.weight")
        self.assertEqual(len(qkv.pieces), 3)
        self.assertEqual(qkv.nbytes, (256 + 64 + 64) * 256)
        self.assertTrue(all(os.path.isabs(p) and os.path.exists(p) for p, _, _ in qkv.pieces))

    def test_a_vllm_shaped_model_matches(self):
        spans = collect(self.model_like_vllm(), {"torch.int8"})
        self.assertEqual(compare(spans, read_manifest(self.manifest)), [])

    def test_a_missing_or_resized_tensor_is_named(self):
        params = self.model_like_vllm()
        params = [p for p in params if p[0] != "model.layers.2.mlp.gate_up_proj.weight"]
        params[0] = (params[0][0], Fake(100, 256))
        diffs = compare(collect(params, {"torch.int8"}), read_manifest(self.manifest))
        self.assertTrue(any("model.layers.2.mlp.gate_up_proj.weight is in the manifest, not the model" in d
                            for d in diffs))
        self.assertTrue(any("model.layers.0.self_attn.qkv_proj.weight is 25600 bytes" in d for d in diffs))


if __name__ == "__main__":
    unittest.main()
