#!/usr/bin/env python3
"""Local web server for the CPU memory-challenge demo.

Usage: python3 demo/bridge.py, then open http://127.0.0.1:8777.
Environment: PORT, IMAGE, PLATFORM, CPUS, VERIFIER_MEM.
"""
import os
import re
import shutil
import subprocess
import threading
import time

from bridge_common import (STATIC, Handler, Hub, Manager, QuietServer, Run, chain_error, clamp,
                           pick)

ROOT = STATIC.parent.parent

PORT = int(os.environ.get("PORT", "8777"))
IMAGE = os.environ.get("IMAGE", "memchal")
PLATFORM = os.environ.get("PLATFORM", "linux/arm64")
CPUS = os.environ.get("CPUS", "1")
VERIFIER_MEM = os.environ.get("VERIFIER_MEM", "1g")
PROVER_PORT = 9700
CONTAINER = "memchal_demo_prover"

MODES = ("honest", "disk", "partial", "fake", "mmapdisk", "recompute", "prefetch", "precompute")
GENS = ("splitmix64", "chacha", "argon2d")
CHAINS = ("independent", "dependent")
NONCES = ("sequential", "random")
MEM_RE = re.compile(r"^[0-9]{1,5}[mg]$")
LIMITS = {
    "mb":     (1, 1024, 64),
    "samples":    (1, 65536, 4096),
    "lanes":  (1, 1024, 32),
    "challenges": (1, 20000, 200),
    "warmup": (0, 20000, 20),
    "trace":  (0, 10000, 20),
    "probe":  (0, 10000, 20),
    "frac":   (0, 1, 0.5),
}
DEFAULTS = {"mode": "honest", "gen": "argon2d", "chain": "independent", "nonce": "sequential", "mem": "96m", **{k: v[2] for k, v in LIMITS.items()}}
STARVED_FALLBACK_GEN = "chacha"
BUILD_PASSES = 2
PRECOMPUTE_GAP_US = 80000
BUILD_HEADROOM = 1.2


def build_parallelism(cpus=CPUS):
    """Return the Argon2d parallelism: the largest power of two at most min(cpus, 8)."""
    try:
        n = int(float(cpus))
    except ValueError:
        n = 1
    n = max(1, min(n, 8))
    return 1 << (n.bit_length() - 1)


def mem_mib(mem):
    """Convert a validated memory string such as '96m' or '1g' to MiB."""
    return int(mem[:-1]) * (1024 if mem.endswith("g") else 1)


def starved_for_build(mode, mem, mb):
    """Return True when a non-honest prover's budget is below BUILD_HEADROOM times the buffer."""
    return mode != "honest" and mem_mib(mem) < BUILD_HEADROOM * mb


def validate(req):
    """Return the whitelisted run config, with defaults for anything unrecognized."""
    mem = str(req.get("mem", DEFAULTS["mem"])).lower()
    out = {
        "mode": pick(req, "mode", MODES, DEFAULTS["mode"]),
        "gen": pick(req, "gen", GENS, DEFAULTS["gen"]),
        "chain": pick(req, "chain", CHAINS, DEFAULTS["chain"]),
        "nonce": pick(req, "nonce", NONCES, DEFAULTS["nonce"]),
        "mem": mem if MEM_RE.match(mem) else DEFAULTS["mem"],
    }
    for name in LIMITS:
        out[name] = clamp(LIMITS, name, req.get(name, LIMITS[name][2]))
    out["gen_fallback"] = (req.get("gen") not in GENS and out["gen"] == "argon2d"
                           and starved_for_build(out["mode"], out["mem"], out["mb"]))
    if out["gen_fallback"]:
        out["gen"] = STARVED_FALLBACK_GEN
    return out


HUB = Hub(live=("run", "p"))


def docker(*args, **kw):
    return subprocess.run(["docker", *args], capture_output=True, text=True, **kw)


def build_image():
    """Build the toolchain image."""
    return docker("build", "--platform", PLATFORM, "-t", IMAGE,
                  "-f", str(ROOT / "docker" / "Dockerfile"), str(ROOT / "docker"))


def docker_hint(proc):
    """Return the last non-empty line of docker's output, truncated."""
    text = (proc.stderr or "") + (proc.stdout or "")
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    return lines[-1][:300] if lines else "no output from docker"


def remove_container(name=None, timeout=8.0):
    """Remove the prover container and wait until its name is free; return False on timeout."""
    name = name or CONTAINER
    docker("rm", "-f", name)
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = docker("ps", "-a", "--filter", f"name=^{name}$", "--format", "{{.Names}}").stdout
        if name not in out:
            return True
        time.sleep(0.2)
    return False


class DockerRun(Run):
    """One prover and verifier run in Docker containers."""

    def __init__(self, cfg):
        super().__init__(HUB, cfg)
        self.verifier = None
        self.logs = None

    def stop(self):
        super().stop()
        for p in (self.verifier, self.logs):
            if p and p.poll() is None:
                p.kill()
        remove_container()

    def _prover_cmd(self):
        c = self.cfg
        return [
            "run", "-d", "--name", CONTAINER, "--platform", PLATFORM,
            "--cpus", CPUS, "--cap-add=IPC_LOCK", "--ulimit", "memlock=-1",
            "--memory", c["mem"], "--memory-swap", c["mem"],
            "-v", f"{ROOT}:/work", IMAGE,
        ] + self._prover_args()

    def _prover_args(self):
        c = self.cfg
        if c["mode"] == "honest":
            return ["build/cpu/prover", "--port", str(PROVER_PORT), "--trace", str(c["trace"])]
        return ["build/attacks/cpu/cheating_prover", "--port", str(PROVER_PORT), "--mode", c["mode"],
                "--frac", str(c["frac"]), "--datafile", "/var/tmp/x.bin", "--trace", str(c["trace"])]

    def _verifier_cmd(self):
        c = self.cfg
        cmd = [
            "docker", "run", "--rm", "--network", f"container:{CONTAINER}",
            "--platform", PLATFORM, "--cpus", CPUS, "--memory", VERIFIER_MEM,
            "-v", f"{ROOT}:/work", IMAGE,
            "build/verifier", "--host", "127.0.0.1", "--port", str(PROVER_PORT),
            "--mb", str(c["mb"]), "--samples", str(c["samples"]), "--lanes", str(c["lanes"]),
            "--challenges", str(c["challenges"]), "--warmup", str(c["warmup"]),
            "--gen", c["gen"],
            "--chain", c["chain"],
            "--nonce", c["nonce"],
            "--label", f"demo-{c['mode']}-{c['mem']}",
            "--csv", "/dev/null", "--stream", "-", "--probe-map", str(c["probe"]),
        ]
        if c["gen"] == "argon2d":
            cmd += ["--build-passes", str(BUILD_PASSES), "--build-parallelism", str(build_parallelism())]
        if c["mode"] == "precompute":
            cmd += ["--gap-us", str(PRECOMPUTE_GAP_US)]
        return cmd

    def _build(self):
        """Build the image and the binaries; return an error message or None."""
        self.status("building")
        image = build_image()
        if image.returncode != 0:
            for line in (image.stderr or "").splitlines()[-8:]:
                self.log(line, "docker build")
            return f"could not build the {IMAGE} image: {docker_hint(image)}"
        if self.stopping:
            return None
        build = docker("run", "--rm", "--platform", PLATFORM, "-v", f"{ROOT}:/work", IMAGE, "make")
        for line in (build.stdout or "").splitlines():
            self.log(line, "make")
        if build.returncode != 0:
            for line in (build.stderr or "").splitlines()[-8:]:
                self.log(line, "make")
            return f"make failed: {docker_hint(build)}"
        return None

    def _start_prover(self):
        """Start the prover container and wait for it to listen; return an error message or None."""
        remove_container()
        self.status("starting prover")
        up = docker(*self._prover_cmd())
        if up.returncode != 0:
            return up.stderr.strip()[:400]
        deadline = time.time() + 30
        while time.time() < deadline and not self.stopping:
            if "listening on" in docker("logs", CONTAINER).stdout:
                return None
            time.sleep(0.15)
        return None if self.stopping else "prover did not come up"

    def _verify(self):
        """Stream the prover's output, run the verifier, and return its exit code."""
        self.logs = subprocess.Popen(["docker", "logs", "-f", CONTAINER], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self.pump, args=(self.logs.stdout, "prover"), daemon=True).start()
        self.status("running")
        self.verifier = subprocess.Popen(self._verifier_cmd(), stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.pump(self.verifier.stdout, "verifier")
        rc = self.verifier.wait()
        time.sleep(0.6)
        return rc

    def execute(self):
        why = self._build()
        if why:
            return self.status("error", error=why)
        for attempt in range(2):
            if self.stopping:
                return self.status("stopped")
            why = self._start_prover()
            if why:
                return self.status("error", error=why)
            if self.stopping:
                return self.status("stopped")
            self.saw_end, self.failures = False, {}
            rc = self._verify()
            oom = docker("inspect", CONTAINER, "--format", "{{.State.OOMKilled}}").stdout.strip() == "true"
            if not oom or self.saw_end or self.stopping:
                return self.finish(rc)
            if attempt == 0:
                self.log("prover was OOM-killed before it finished taking the buffer; retrying once.")
                remove_container()
                time.sleep(1.0)
        self.finish(rc, why="the prover was OOM-killed twice: raise the RAM budget or lower the "
                            "buffer, since the prover needs headroom to stream the data to disk")

    def cleanup(self):
        remove_container()
        if self.logs and self.logs.poll() is None:
            self.logs.kill()


MANAGER = Manager()


class CpuHandler(Handler):
    hub = HUB
    manager = MANAGER
    index = "index.html"
    get_routes = {"/config": lambda: {
        "defaults": DEFAULTS, "modes": list(MODES),
        "build": {"passes": BUILD_PASSES, "parallelism": build_parallelism()},
        "limits": {k: v[:2] for k, v in LIMITS.items()},
        "status": HUB.last_status}}

    def start_run(self, req):
        cfg = validate(req)
        why = chain_error(cfg)
        if why:
            return 400, {"error": why}
        why = MANAGER.start(DockerRun(cfg))
        if why:
            return 409, {"error": why}
        HUB.publish({"t": "log", "src": "bridge",
                     "line": f"run requested from {self.client_address[0]}: "
                             f"{cfg['mode']} at {cfg['mem']}, buffer {cfg['mb']} MiB, "
                             f"gen={cfg['gen']} chain={cfg['chain']} "
                             f"challenges={cfg['challenges']} warmup={cfg['warmup']}"})
        if cfg["gen_fallback"]:
            HUB.publish({"t": "log", "src": "bridge",
                         "line": f"no generator named, and {cfg['mem']} is below "
                                 f"{BUILD_HEADROOM}x the {cfg['mb']} MiB buffer: using "
                                 f"{cfg['gen']}, because argon2d needs the whole buffer "
                                 f"in RAM while building and this prover could not start."})
        return 200, {"started": cfg}


def preflight():
    if not shutil.which("docker"):
        raise SystemExit("ERROR: docker CLI not found. This demo drives real containers; "
                         "run it on the host with Docker running.")
    if docker("version", "--format", "{{.Server.Version}}").returncode != 0:
        raise SystemExit("ERROR: docker is installed but not responding. Is Docker Desktop up?")
    if not (STATIC / "index.html").is_file():
        raise SystemExit(f"ERROR: missing {STATIC/'index.html'}")
    print(f"[bridge] building the {IMAGE} image (cached after the first time)...")
    image = build_image()
    if image.returncode != 0:
        raise SystemExit("ERROR: could not build the toolchain image:\n"
                         + (image.stderr or image.stdout or "").strip()[-1500:])


def main():
    preflight()
    server = QuietServer(("127.0.0.1", PORT), CpuHandler)
    print(f"[bridge] memory-challenge demo on http://127.0.0.1:{PORT}")
    print(f"[bridge] image={IMAGE} platform={PLATFORM} cpus={CPUS} repo={ROOT}")
    print(f"[bridge] argon2d builds use passes={BUILD_PASSES} parallelism={build_parallelism()}")
    print("[bridge] Ctrl-C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] stopping")
    finally:
        MANAGER.stop()
        server.server_close()


if __name__ == "__main__":
    main()
