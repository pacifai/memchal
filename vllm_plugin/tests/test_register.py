#!/usr/bin/env python3
"""The plugin's hooks, against a fake vllm module: what it wraps, when, and what it refuses."""

from __future__ import annotations

import multiprocessing
import os
import sys
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from test_weights import Fake  # noqa: E402


def fake_vllm(calls: list) -> type:
    """Install vllm.v1.worker.gpu_worker with a Worker that records its calls."""

    class Model:
        def named_parameters(self):
            return [("model.layers.0.self_attn.o_proj.weight", Fake(64, 64, ptr=0x4000, contiguous=False))]

    class Worker:
        def __init__(self, sleep=False):
            mc = types.SimpleNamespace(enable_sleep_mode=sleep)
            self.vllm_config = types.SimpleNamespace(model_config=mc)

        def load_model(self, **kwargs):
            calls.append("load_model")

        def compile_or_warm_up_model(self):
            calls.append("warm_up")
            return "times"

        def get_model(self):
            return Model()

    mods = {}
    for name in ["vllm", "vllm.v1", "vllm.v1.worker", "vllm.v1.worker.gpu_worker"]:
        mods[name] = sys.modules[name] = types.ModuleType(name)
    mods["vllm.v1.worker.gpu_worker"].Worker = Worker
    mods["vllm.v1.worker"].gpu_worker = mods["vllm.v1.worker.gpu_worker"]
    return Worker


class RegisterTest(unittest.TestCase):
    def setUp(self):
        self.calls: list = []
        self.Worker = fake_vllm(self.calls)
        import memchal_vllm

        self.plugin = memchal_vllm
        self.served: list = []
        self.plugin._serve = lambda: self.served.append(list(self.plugin._spans or []))
        self.proc = multiprocessing.current_process()
        self.old_name = self.proc.name
        os.environ.pop("MEMCHAL_WEIGHTS_MANIFEST", None)

    def tearDown(self):
        self.proc.name = self.old_name
        os.environ.pop("MEMCHAL_WEIGHTS_PORT", None)
        for name in [m for m in sys.modules if m == "vllm" or m.startswith("vllm.")]:
            del sys.modules[name]

    def test_off_unless_a_port_is_set(self):
        self.proc.name = "EngineCore"
        self.plugin.register()
        self.assertFalse(getattr(self.Worker, "_memchal_wrapped", False))

    def test_off_outside_the_gpu_process(self):
        os.environ["MEMCHAL_WEIGHTS_PORT"] = "9100"
        self.proc.name = "MainProcess"   # the API server
        self.plugin.register()
        self.assertFalse(getattr(self.Worker, "_memchal_wrapped", False))

    def test_collects_after_load_and_serves_after_warm_up(self):
        os.environ["MEMCHAL_WEIGHTS_PORT"] = "9100"
        self.proc.name = "EngineCore"
        self.plugin.register()
        self.plugin.register()           # vLLM may load plugins again; wrap once
        w = self.Worker()
        w.load_model()
        self.assertEqual(self.served, [])
        self.assertEqual(w.compile_or_warm_up_model(), "times")
        self.assertEqual(self.calls, ["load_model", "warm_up"])
        self.assertEqual([[s.name for s in spans] for spans in self.served],
                         [["model.layers.0.self_attn.o_proj.weight"]])

    def test_sleep_mode_is_refused_without_stopping_the_engine(self):
        os.environ["MEMCHAL_WEIGHTS_PORT"] = "9100"
        self.proc.name = "EngineCore"
        self.plugin.register()
        w = self.Worker(sleep=True)
        with self.assertLogs("memchal", level="ERROR") as logs:
            w.load_model()
        self.assertIn("sleep mode", logs.output[0])
        self.assertIsNone(self.plugin._spans)
        self.assertEqual(w.compile_or_warm_up_model(), "times")   # vLLM carries on


if __name__ == "__main__":
    unittest.main()
