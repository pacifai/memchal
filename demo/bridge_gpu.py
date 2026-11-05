#!/usr/bin/env python3
"""Web server for the GPU memory-challenge demo; run it on the GPU host.

Usage: python3 demo/bridge_gpu.py, then tunnel port 8777 and open it.
Environment: PORT, PROVER_PORT, DEVICE, SMI_INTERVAL.
"""
import json
import os
import shutil
import socket
import subprocess
import threading
import time

from bridge_common import (STATIC, Handler, Hub, Manager, QuietServer, Run, chain_error, clamp,
                           pick)

ROOT = STATIC.parent.parent

PORT = int(os.environ.get("PORT", "8777"))
PROVER_PORT = int(os.environ.get("PROVER_PORT", "9700"))
DEVICE = os.environ.get("DEVICE", "0")
SMI_INTERVAL = float(os.environ.get("SMI_INTERVAL", "1.0"))

MODES = ("honest", "fake", "uvm", "partial")
GENS = ("argon2d", "chacha", "splitmix64")
CHAINS = ("independent", "dependent")
NONCES = ("sequential", "random")
FOLDS = ("tree", "sequential")
LIMITS = {
    "mb":                (1, 262144, 4096),
    "samples":           (1, 1 << 24, 262144),
    "lanes":             (1, 1 << 18, 16384),
    "challenges":        (1, 20000, 200),
    "warmup":            (0, 20000, 20),
    "free":              (0, 262144, 0),
    "trace":             (0, 10000, 1),
    "frac":              (0, 1, 0.5),
    "offload":           (0, 1, 0.1),
    "verify":            (1, 1000, 10),
    "gap":               (0, 100000, 0),
    "build_parallelism": (1, 64, 16),
}
BUILD_PASSES = 2
DEFAULTS = {"mode": "honest", "gen": "argon2d", "chain": "dependent",
            "nonce": "random", "fold": "tree",
            **{k: v[2] for k, v in LIMITS.items()}}


def validate(req):
    """Return the whitelisted run config, with defaults for anything unrecognized."""
    out = {
        "mode":  pick(req, "mode", MODES, DEFAULTS["mode"]),
        "gen":   pick(req, "gen", GENS, DEFAULTS["gen"]),
        "chain": pick(req, "chain", CHAINS, DEFAULTS["chain"]),
        "nonce": pick(req, "nonce", NONCES, DEFAULTS["nonce"]),
        "fold":  pick(req, "fold", FOLDS, DEFAULTS["fold"]),
    }
    for name in LIMITS:
        out[name] = clamp(LIMITS, name, req.get(name, LIMITS[name][2]))
    return out


HUB = Hub(sticky=("gpuinfo", "smi"))


SMI_FIELDS = ("clocks.sm", "clocks.mem", "temperature.gpu", "utilization.gpu",
              "memory.used", "memory.total", "power.draw",
              "clocks_throttle_reasons.active")


def smi_sample():
    """Return one nvidia-smi reading as an smi event, or None if unavailable."""
    if not shutil.which("nvidia-smi"):
        return None
    try:
        out = subprocess.run(
            ["nvidia-smi", "-i", DEVICE,
             "--query-gpu=" + ",".join(SMI_FIELDS),
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=5)
    except (subprocess.TimeoutExpired, OSError):
        return None
    if out.returncode != 0 or not out.stdout.strip():
        return None
    parts = [p.strip() for p in out.stdout.strip().splitlines()[0].split(",")]
    if len(parts) < len(SMI_FIELDS):
        return None

    def num(x):
        try:
            return float(x)
        except ValueError:
            return None

    return {
        "t": "smi",
        "sm_mhz": num(parts[0]), "mem_mhz": num(parts[1]),
        "temp_c": num(parts[2]), "util_pct": num(parts[3]),
        "mem_used_mib": num(parts[4]), "mem_total_mib": num(parts[5]),
        "power_w": num(parts[6]), "throttle": parts[7],
    }


def smi_loop():
    while True:
        s = smi_sample()
        if s:
            HUB.publish(s)
        time.sleep(SMI_INTERVAL)


RUNS_DIR = ROOT / "results" / "demo_runs"


class GpuRun(Run):
    """One prover and verifier run; records every event to results/demo_runs/<stamp>-<mode>.jsonl."""

    def __init__(self, cfg):
        super().__init__(HUB, cfg)
        self.prover = None
        self.verifier = None
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.record_path = RUNS_DIR / f"{stamp}-{cfg['mode']}.jsonl"
        self.record = None
        self.record_lock = threading.Lock()
        try:
            RUNS_DIR.mkdir(parents=True, exist_ok=True)
            self.record = self.record_path.open("w", buffering=1)
            self._write({"t": "meta", "started": stamp, "cfg": cfg, "host": os.uname().nodename})
        except OSError as exc:
            print(f"[bridge] cannot record run: {exc}", flush=True)

    def _write(self, event):
        """Append one event to the run file, disabling recording on error."""
        if not self.record:
            return
        try:
            with self.record_lock:
                self.record.write(json.dumps(event) + "\n")
        except (OSError, ValueError):
            self.record = None

    def emit(self, event):
        self._write(event)
        super().emit(event)
        if event.get("t") == "run":
            self.status("running")

    def stop(self):
        super().stop()
        for p in (self.verifier, self.prover):
            if p and p.poll() is None:
                p.kill()

    def _prover_cmd(self):
        c = self.cfg
        if c["mode"] == "honest":
            return [str(ROOT / "build" / "gpu" / "prover"), "--port", str(PROVER_PORT),
                    "--device", DEVICE, "--trace", str(c["trace"]),
                    "--lane-cap", str(max(1, c["lanes"]))]
        reserve = ["--leave-free", str(c["free"])] if c["free"] > 0 else []
        return [str(ROOT / "build" / "attacks" / "gpu" / "cheating_prover"), "--port", str(PROVER_PORT),
                "--device", DEVICE, "--mode", c["mode"], "--frac", str(c["frac"]),
                *reserve, "--offload", str(c["offload"]),
                "--trace", str(c["trace"]),
                "--lane-cap", str(max(1, c["lanes"]))]

    def _verifier_cmd(self):
        c = self.cfg
        build = (["--build-passes", str(BUILD_PASSES), "--build-parallelism", str(c["build_parallelism"])]
                 if c["gen"] == "argon2d" else [])
        return [str(ROOT / "build" / "verifier"), "--host", "127.0.0.1",
                "--port", str(PROVER_PORT),
                "--mb", str(c["mb"]), "--samples", str(c["samples"]), "--lanes", str(c["lanes"]),
                "--challenges", str(c["challenges"]), "--warmup", str(c["warmup"]),
                "--gen", c["gen"], *build, "--chain", c["chain"], "--nonce", c["nonce"],
                "--fold", c["fold"],
                "--label", f"gpu-{c['mode']}",
                "--csv", "/dev/null", "--stream", "-", "--probe-map", "0",
                "--gap-us", str(c["gap"]),
                "--verify-every", str(c["verify"])]

    def _start_prover(self):
        """Start the prover and wait for it to listen; return an error message or None."""
        self.status("starting prover")
        self.prover = subprocess.Popen(self._prover_cmd(), stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self.pump, args=(self.prover.stdout, "prover"), daemon=True).start()
        deadline = time.time() + 60
        while time.time() < deadline and not self.stopping:
            if self.prover.poll() is not None:
                return "the prover exited during startup — see the log"
            if port_open(PROVER_PORT):
                return None
            time.sleep(0.1)
        return None if self.stopping else "prover did not come up"

    def execute(self):
        binary = "gpu/prover" if self.cfg["mode"] == "honest" else "attacks/gpu/cheating_prover"
        for name in ("verifier", binary):
            if not (ROOT / "build" / name).exists():
                return self.status("error", error=f"{name} is not built. Run `make gpu` "
                                                  f"(and `make` for the verifier) first.")
        why = self._start_prover()
        if why:
            return self.status("error", error=why)
        if self.stopping:
            return self.status("stopped")
        self.status("uploading", mb=self.cfg["mb"])
        self.verifier = subprocess.Popen(self._verifier_cmd(), stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.pump(self.verifier.stdout, "verifier")
        rc = self.verifier.wait()
        time.sleep(0.5)
        self.finish(rc)

    def cleanup(self):
        for p in (self.verifier, self.prover):
            if p and p.poll() is None:
                p.kill()
        if self.record:
            print(f"[bridge] run recorded: {self.record_path}", flush=True)
            try:
                self.record.close()
            except OSError:
                pass
            self.record = None


def port_open(port):
    with socket.socket() as s:
        s.settimeout(0.2)
        return s.connect_ex(("127.0.0.1", port)) == 0


MANAGER = Manager()


class GpuHandler(Handler):
    hub = HUB
    manager = MANAGER
    index = "gpu.html"

    def start_run(self, req):
        cfg = validate(req)
        why = chain_error(cfg)
        if why:
            return 400, {"error": why}
        why = MANAGER.start(GpuRun(cfg))
        if why:
            return 409, {"error": why}
        return 200, {"ok": True, "cfg": cfg}


def preflight():
    print(f"[bridge] repo root: {ROOT}", flush=True)
    missing = [b for b in ("verifier", "gpu/prover", "attacks/gpu/cheating_prover")
               if not (ROOT / "build" / b).exists()]
    if missing:
        print(f"[bridge] NOT BUILT: {', '.join(missing)}", flush=True)
        print("[bridge] run `make` and `make gpu` first, or the page will say so.", flush=True)
    s = smi_sample()
    if s:
        print(f"[bridge] gpu {DEVICE}: {s['mem_used_mib']:.0f}/{s['mem_total_mib']:.0f} MiB used, "
              f"sm {s['sm_mhz']:.0f} MHz, {s['temp_c']:.0f} C, throttle: {s['throttle']}", flush=True)
    else:
        print("[bridge] nvidia-smi unavailable — the clock panel will stay empty.", flush=True)


def main():
    preflight()
    threading.Thread(target=smi_loop, daemon=True).start()
    server = QuietServer(("127.0.0.1", PORT), GpuHandler)
    print(f"[bridge] http://127.0.0.1:{PORT}  (tunnel this port to reach it)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] stopping")
        MANAGER.stop()


if __name__ == "__main__":
    main()
