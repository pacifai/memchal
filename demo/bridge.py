#!/usr/bin/env python3
"""Local web server for the CPU memory-challenge demo.

Usage: python3 demo/bridge.py, then open http://127.0.0.1:8777.
Environment: PORT, IMAGE, PLATFORM, CPUS, VERIFIER_MEM.
"""
import json
import os
import queue
import re
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATIC = Path(__file__).resolve().parent / "static"

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


def clamp(name, value):
    lo, hi, default = LIMITS[name]
    try:
        v = float(value) if name == "frac" else int(value)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, v))


def validate(req):
    """Return the whitelisted run config, with defaults for anything unrecognized."""
    mode = req.get("mode")
    gen = req.get("gen")
    mem = str(req.get("mem", DEFAULTS["mem"])).lower()
    chain = req.get("chain")
    nonce = req.get("nonce")
    out = {
        "mode": mode if mode in MODES else DEFAULTS["mode"],
        "gen": gen if gen in GENS else DEFAULTS["gen"],
        "chain": chain if chain in CHAINS else DEFAULTS["chain"],
        "nonce": nonce if nonce in NONCES else DEFAULTS["nonce"],
        "mem": mem if MEM_RE.match(mem) else DEFAULTS["mem"],
    }
    for name in LIMITS:
        out[name] = clamp(name, req.get(name, LIMITS[name][2]))
    out["gen_fallback"] = (gen not in GENS and out["gen"] == "argon2d"
                           and starved_for_build(out["mode"], out["mem"], out["mb"]))
    if out["gen_fallback"]:
        out["gen"] = STARVED_FALLBACK_GEN
    return out


class Hub:
    """Fan events out to browser tabs and keep enough state to replay to a late tab."""

    def __init__(self):
        self.lock = threading.Lock()
        self.subscribers = []
        self.last_status = {"t": "status", "phase": "idle"}
        self.last_run = None
        self.last_prover = None
        self.history = []

    def replay(self):
        events = []
        if self.history:
            events.append({"t": "history", "runs": self.history})
        events.append(self.last_status)
        if self.last_status.get("phase") == "running":
            events += [e for e in (self.last_run, self.last_prover) if e]
        return events

    def record(self, end_event):
        """Append a finished run's summary to the history, keeping the last 40."""
        if not self.last_run:
            return
        self.history.append({
            "cfg": self.last_run.get("cfg"),
            "p50": end_event.get("p50"),
            "errors": end_event.get("errors"),
            "challenges": end_event.get("challenges"),
        })
        del self.history[:-40]

    def subscribe(self):
        q = queue.Queue(maxsize=10000)
        with self.lock:
            self.subscribers.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subscribers:
                self.subscribers.remove(q)

    def publish(self, event):
        kind = event.get("t")
        if kind == "status":
            self.last_status = event
            if event.get("phase") in ("building", "starting prover"):
                self.last_run = self.last_prover = None
        elif kind == "run":
            self.last_run, self.last_prover = event, None
        elif kind == "p":
            self.last_prover = event
        elif kind == "end":
            self.record(event)
        with self.lock:
            subs = list(self.subscribers)
        for q in subs:
            try:
                q.put_nowait(event)
            except queue.Full:
                pass


HUB = Hub()


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


class Run:
    """One prover and verifier run, owning its containers."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.saw_end = False
        self.stopping = False
        self.verifier = None
        self.logs = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping = True
        for p in (self.verifier, self.logs):
            if p and p.poll() is None:
                p.kill()
        remove_container()

    def _status(self, phase, **kw):
        HUB.publish({"t": "status", "phase": phase, "cfg": self.cfg, **kw})

    def _log(self, line, src="bridge"):
        HUB.publish({"t": "log", "src": src, "line": line.rstrip()})

    def _pump(self, stream, src):
        """Publish '@' lines from a process's stdout as events and the rest as log lines."""
        for raw in stream:
            line = raw.rstrip("\n")
            if line.startswith("@"):
                try:
                    event = json.loads(line[1:])
                except json.JSONDecodeError:
                    self._log(f"unparseable event: {line[:80]}", src)
                    continue
                event["src"] = src
                if event.get("t") == "end":
                    self.saw_end = True
                if event.get("t") == "run":
                    event["cfg"] = self.cfg
                HUB.publish(event)
            elif line:
                self._log(line, src)

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
        return ["build/cpu/cheating_prover", "--port", str(PROVER_PORT), "--mode", c["mode"],
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

    def _run(self):
        self._attempt(0)

    def _attempt(self, attempt):
        self.saw_end = False
        try:
            self._status("building")
            image = build_image()
            if image.returncode != 0:
                for line in (image.stderr or "").splitlines()[-8:]:
                    self._log(line, "docker build")
                return self._status("error",
                                    error=f"could not build the {IMAGE} image: "
                                          f"{docker_hint(image)}")

            build = docker("run", "--rm", "--platform", PLATFORM,
                           "-v", f"{ROOT}:/work", IMAGE, "make")
            for line in (build.stdout or "").splitlines():
                self._log(line, "make")
            if build.returncode != 0:
                for line in (build.stderr or "").splitlines()[-8:]:
                    self._log(line, "make")
                return self._status("error", error=f"make failed: {docker_hint(build)}")

            remove_container()
            self._status("starting prover")
            up = docker(*self._prover_cmd())
            if up.returncode != 0:
                return self._status("error", error=up.stderr.strip()[:400])

            deadline = time.time() + 30
            while time.time() < deadline and not self.stopping:
                if "listening on" in docker("logs", CONTAINER).stdout:
                    break
                time.sleep(0.15)
            else:
                if not self.stopping:
                    return self._status("error", error="prover did not come up")
            if self.stopping:
                return self._status("stopped")

            self.logs = subprocess.Popen(["docker", "logs", "-f", CONTAINER],
                                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                         text=True, bufsize=1)
            threading.Thread(target=self._pump, args=(self.logs.stdout, "prover"),
                             daemon=True).start()

            self._status("running")
            self.verifier = subprocess.Popen(self._verifier_cmd(), stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, text=True, bufsize=1)
            self._pump(self.verifier.stdout, "verifier")
            rc = self.verifier.wait()

            time.sleep(0.6)
            oom = docker("inspect", CONTAINER, "--format", "{{.State.OOMKilled}}").stdout.strip()
            if oom == "true":
                self._log("prover was OOM-KILLED before it finished taking the buffer.",
                          "bridge")
            if oom == "true" and not self.saw_end and not self.stopping and attempt == 0:
                self._log("retrying once — if it fails again the budget really is too "
                          "small for this buffer.", "bridge")
                remove_container()
                time.sleep(1.0)
                return self._attempt(1)
            if oom == "true":
                self._log("still OOM-killed. Raise the RAM budget (or lower the buffer): "
                          "the prover needs enough headroom to stream the data to disk.",
                          "bridge")
            self._status("stopped" if self.stopping else "done", rc=rc, oom=(oom == "true"))
        except Exception as exc:
            self._status("error", error=f"{type(exc).__name__}: {exc}")
        finally:
            remove_container()
            if self.logs and self.logs.poll() is None:
                self.logs.kill()


class Manager:
    def __init__(self):
        self.lock = threading.Lock()
        self.current = None

    def start(self, cfg):
        with self.lock:
            if self.current and self.current.thread.is_alive():
                self.current.stop()
                self.current.thread.join(timeout=5)
            self.current = Run(cfg)
            self.current.start()

    def stop(self):
        with self.lock:
            if self.current:
                self.current.stop()


MANAGER = Manager()


CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "memchal-demo"

    def log_message(self, fmt, *args):
        pass

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (ConnectionResetError, BrokenPipeError):
            self.close_connection = True

    def _send(self, code, body=b"", ctype="text/plain; charset=utf-8", **headers):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers.items():
            self.send_header(k.replace("_", "-"), v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj).encode(), "application/json")

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/events":
            return self._events()
        if path == "/config":
            return self._json({"defaults": DEFAULTS, "modes": list(MODES),
                               "build": {"passes": BUILD_PASSES, "parallelism": build_parallelism()},
                               "limits": {k: v[:2] for k, v in LIMITS.items()},
                               "status": HUB.last_status})
        name = "index.html" if path == "/" else path.lstrip("/")
        target = (STATIC / name).resolve()
        if target.parent != STATIC or not target.is_file():
            return self._send(404, b"not found")
        body = target.read_bytes()
        self._send(200, body, CONTENT_TYPES.get(target.suffix, "application/octet-stream"),
                   Cache_Control="no-store")

    def do_POST(self):
        path = self.path.split("?")[0]
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            req = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            return self._json({"error": "bad json"}, 400)
        if path == "/run":
            cfg = validate(req if isinstance(req, dict) else {})
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
            MANAGER.start(cfg)
            return self._json({"started": cfg})
        if path == "/stop":
            MANAGER.stop()
            HUB.publish({"t": "status", "phase": "idle"})
            return self._json({"stopped": True})
        return self._send(404, b"not found")

    def _events(self):
        q = HUB.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            for event in HUB.replay():
                self._write_event(event)
            while True:
                try:
                    self._write_event(q.get(timeout=15))
                except queue.Empty:
                    self.wfile.write(b": keep-alive\n\n")
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            HUB.unsubscribe(q)

    def _write_event(self, event):
        self.wfile.write(f"data: {json.dumps(event)}\n\n".encode())
        self.wfile.flush()


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
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
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
