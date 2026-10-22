#!/usr/bin/env python3
"""bridge.py — local web server for the interactive memory-challenge demo.

WHAT IT IS. A thin bridge between a browser page and the real binaries. It does
not simulate anything: every number the page draws comes from a prover and a
verifier running in containers, exactly as the terminal experiments run them.

  browser  --POST /run-->  bridge  --docker run-->  cheating_prover (own RAM budget)
                                   --docker run-->  verifier (shares prover's netns)
  browser  <--SSE /events--  bridge  <--stdout '@' JSON lines-- both processes

HOW TO RUN (on the host, not inside run.sh — it drives docker):
    python3 demo/bridge.py            # then open http://127.0.0.1:8777
    PORT=9000 python3 demo/bridge.py

Env: PORT IMAGE PLATFORM CPUS VERIFIER_MEM. CPUS also sets the Argon2d build
lanes (see build_lanes()).

WHAT THE PAGE IS ALLOWED TO ASK FOR is whitelisted below (modes, memory strings,
numeric ranges) and every container is spawned with an argument list, never a
shell string — a browser page must not be able to talk the bridge into running
something else.

TWO PROTOCOLS. gen=splitmix64 and gen=chacha upload the buffer. gen=argon2d sends
a 92-byte recipe instead and both sides build the buffer with Argon2d (ADR 0007).
argon2d is the default. Building needs buffer-sized RAM plus some headroom, so a
prover whose memory budget is below about 1.2x the buffer cannot set up at all
under argon2d — it is OOM-killed during the build rather than running slowly.
(Measured with a 64 MiB buffer: mmapdisk is killed at 64 MiB and runs at 77 MiB.) That collides with the starved-budget runs
the demo exists to show, so:
  - the default run is the honest prover with room to spare, which argon2d can build;
  - the page switches a starved run to chacha and says why;
  - a request that names no generator gets chacha when it is starved (see
    validate()), and the bridge logs the substitution;
  - a request that asks for argon2d explicitly gets argon2d. The page warns about
    that pairing; the bridge lets it through and reports the real error.

MEASUREMENT NOTE. Live streaming makes both processes emit an event per challenge
(and a mincore() residency snapshot every N), all of it after the response has
been sent, so it is outside the verifier's timed window — but it does space the
challenges out. The headline numbers in README.md come from the terminal
experiments, which never enable tracing. This is a demo, not the measurement.
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

# ── what the browser may ask for ─────────────────────────────────────────────
MODES = ("honest", "disk", "partial", "fake", "mmapdisk", "recompute", "prefetch", "precompute")
GENS = ("splitmix64", "chacha", "argon2d")
CHAINS = ("independent", "dependent")
NONCES = ("sequential", "random")
MEM_RE = re.compile(r"^[0-9]{1,5}[mg]$")
LIMITS = {           # field: (min, max, default)
    "mb":     (1, 1024, 64),
    "k":      (1, 65536, 4096),
    "lanes":  (1, 1024, 32),
    "n":      (1, 20000, 200),
    "warmup": (0, 20000, 20),
    "trace":  (0, 10000, 20),     # prover residency snapshot every N challenges
    "probe":  (0, 10000, 20),     # verifier probed-page map every N challenges
    "frac":   (0, 1, 0.5),
}
# The opening run is the honest prover at 1.5x a 64 MiB buffer: the page tells
# you to run it first, and it is a run argon2d can build (an honest prover needs
# about 1.2x the buffer anyway).
DEFAULTS = {"mode": "honest", "gen": "argon2d", "chain": "independent", "nonce": "sequential", "mem": "96m", **{k: v[2] for k, v in LIMITS.items()}}
STARVED_FALLBACK_GEN = "chacha"
BUILD_T = 2


def build_lanes(cpus=CPUS):
    """Argon2d lanes (--build-p) for a container with `cpus` CPUs.

    Lanes fill on threads, so p only pays off up to the CPUs the container may
    use; past that the threads queue for the same quota. The answer is the largest
    power of two no greater than min(cpus, 8): a power of two divides any whole
    MiB buffer cleanly (Argon2 rounds m down to a multiple of 4*p KiB), and past
    4 lanes a 1 GiB build gains little (0.30 s at p=4, 0.21 s at p=16). With the
    default CPUS=1 this is 1. Both containers get the same CPUS, and the verifier
    and prover build the same recipe, so one value fits both."""
    try:
        n = int(float(cpus))
    except ValueError:
        n = 1
    n = max(1, min(n, 8))
    return 1 << (n.bit_length() - 1)


def mem_mib(mem):
    """'96m' -> 96, '1g' -> 1024. mem has already passed MEM_RE."""
    return int(mem[:-1]) * (1024 if mem.endswith("g") else 1)


# A prover building an Argon2d buffer needs this multiple of the buffer as its
# budget: the whole matrix, plus the process and the spill chunk. It matches the
# ~1.2x the page already asks of the honest prover.
BUILD_HEADROOM = 1.2


def starved_for_build(mode, mem, mb):
    """True when this prover cannot build an Argon2d buffer: a non-honest prover
    whose budget is below BUILD_HEADROOM times the buffer. The honest prover is
    left out because it needs that much under every generator, and the page warns
    about that separately. app.js starvedForBuild() is the same rule."""
    return mode != "honest" and mem_mib(mem) < BUILD_HEADROOM * mb


def clamp(name, value):
    lo, hi, default = LIMITS[name]
    try:
        v = float(value) if name == "frac" else int(value)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, v))


def validate(req):
    """Whitelist the run request. Anything unrecognized falls back to a default."""
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
    # Only a missing or unknown generator is replaced here. An explicit argon2d
    # request is honored even when it cannot build, so its failure stays visible.
    out["gen_fallback"] = (gen not in GENS and out["gen"] == "argon2d"
                           and starved_for_build(out["mode"], out["mem"], out["mb"]))
    if out["gen_fallback"]:
        out["gen"] = STARVED_FALLBACK_GEN
    return out


# ── event hub (server-sent events) ───────────────────────────────────────────
class Hub:
    """Fan-out to browser tabs, plus just enough retained state that a tab which
    connects (or reloads) mid-run can start drawing immediately instead of waiting
    for the next run: the run header describes the buffer geometry every later
    event is interpreted against, so without it a late tab shows nothing."""

    def __init__(self):
        self.lock = threading.Lock()
        self.subscribers = []
        self.last_status = {"t": "status", "phase": "idle"}
        self.last_run = None
        self.last_prover = None
        self.history = []        # finished runs, so a reloaded page keeps its baseline

    def replay(self):
        events = []
        if self.history:
            events.append({"t": "history", "runs": self.history})
        events.append(self.last_status)
        # Only hand a late tab the live run's context. Replaying a FINISHED run's
        # header gives the page a run with no challenges in it, which renders as a
        # verdict with no measurements behind it — worse than showing nothing.
        if self.last_status.get("phase") == "running":
            events += [e for e in (self.last_run, self.last_prover) if e]
        return events

    def record(self, end_event):
        """Remember a finished run: the page needs the honest baseline and the
        collapse marks to survive a reload in the middle of a demo."""
        if not self.last_run:
            return
        self.history.append({
            "cfg": self.last_run.get("cfg"),
            "p50": end_event.get("p50"),
            "errors": end_event.get("errors"),
            "samples": end_event.get("samples"),
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
                self.last_run = self.last_prover = None   # a new run invalidates the old context
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
                pass  # a stalled browser tab must not block a run


HUB = Hub()


def docker(*args, **kw):
    return subprocess.run(["docker", *args], capture_output=True, text=True, **kw)


def build_image():
    """Build the toolchain image. run.sh and both experiment scripts build it
    themselves, and the bridge used not to — so on a machine that had never built
    it (or had its images pruned) every run died with a generic "build failed"
    while the real reason, `pull access denied for memchal`, sat in the log."""
    return docker("build", "--platform", PLATFORM, "-t", IMAGE, str(ROOT))


def docker_hint(proc):
    """The most useful line docker gave us, for a status line that has one line."""
    text = (proc.stderr or "") + (proc.stdout or "")
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    return lines[-1][:300] if lines else "no output from docker"


def remove_container(name=None, timeout=8.0):
    """Remove the prover container and wait until the name is actually free.

    Starting the next prover while the previous one is still tearing down was
    enough, intermittently, to get a memory-starved prover OOM-killed mid-transfer
    — the two cgroups overlap and the new one loses the race for reclaim. Waiting
    costs a second and removes a coin-flip from a live demo."""
    name = name or CONTAINER
    docker("rm", "-f", name)
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = docker("ps", "-a", "--filter", f"name=^{name}$", "--format", "{{.Names}}").stdout
        if name not in out:
            return True
        time.sleep(0.2)
    return False


# ── the run itself ───────────────────────────────────────────────────────────
class Run:
    """One prover+verifier run. Owns its containers and kills them on stop()."""

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

    # -- helpers ----------------------------------------------------------
    def _status(self, phase, **kw):
        HUB.publish({"t": "status", "phase": phase, "cfg": self.cfg, **kw})

    def _log(self, line, src="bridge"):
        HUB.publish({"t": "log", "src": src, "line": line.rstrip()})

    def _pump(self, stream, src):
        """Forward one process's stdout: '@' lines are events, the rest are log."""
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
                    # The verifier knows the buffer geometry; only the bridge knows
                    # the prover's RAM budget. Stamp it so a page never has to infer
                    # which run it is looking at from its own form fields.
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
            "./cheating_prover", "--port", str(PROVER_PORT), "--mode", c["mode"],
            "--frac", str(c["frac"]), "--datafile", "/var/tmp/x.bin",
            "--trace", str(c["trace"]),
        ]

    def _verifier_cmd(self):
        c = self.cfg
        cmd = [
            "docker", "run", "--rm", "--network", f"container:{CONTAINER}",
            "--platform", PLATFORM, "--cpus", CPUS, "--memory", VERIFIER_MEM,
            "-v", f"{ROOT}:/work", IMAGE,
            "./verifier", "--host", "127.0.0.1", "--port", str(PROVER_PORT),
            "--mb", str(c["mb"]), "--k", str(c["k"]), "--lanes", str(c["lanes"]),
            "--n", str(c["n"]), "--warmup", str(c["warmup"]),
            "--gen", c["gen"],
            "--chain", c["chain"],
            "--nonce", c["nonce"],
            "--label", f"demo-{c['mode']}-{c['mem']}",
            "--csv", "/dev/null", "--stream", "-", "--probe-map", str(c["probe"]),
        ]
        if c["gen"] == "argon2d":
            cmd += ["--build-t", str(BUILD_T), "--build-p", str(build_lanes())]
        if c["mode"] == "precompute":
            # The attack precomputes each answer offline in the gap between
            # challenges, so the gap must exceed the cost of one answer. In the
            # starved regime (where the attack matters — on-the-spot reads are cold
            # and slow) that cost is tens of ms, so 400us would leave even the attack
            # unable to get ahead. Give it a generous gap so a starved-budget run
            # actually shows sequential(fast, cache-ahead) vs random(slow, cold read).
            cmd += ["--gap-us", "80000"]
        return cmd

    # -- the sequence -----------------------------------------------------
    def _run(self):
        self._attempt(0)

    def _attempt(self, attempt):
        self.saw_end = False
        try:
            self._status("building")
            image = build_image()          # cached and quick once it exists
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

            # Wait for the listener the prover announces before it accepts.
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

            # Follow the prover's stdout (trace events + its own report lines).
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

            # Let the prover flush its final trace + report_io lines.
            time.sleep(0.6)
            oom = docker("inspect", CONTAINER, "--format", "{{.State.OOMKilled}}").stdout.strip()
            if oom == "true":
                self._log("prover was OOM-KILLED before it finished taking the buffer.",
                          "bridge")
            if oom == "true" and not self.saw_end and not self.stopping and attempt == 0:
                # A starved prover is close to the edge by design, and losing the
                # transfer is occasionally a race rather than a verdict. Retry once,
                # out loud, before telling the operator the budget is too small.
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
        except Exception as exc:                      # a crashed run must not kill the server
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

# ── HTTP ─────────────────────────────────────────────────────────────────────
CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "memchal-demo"

    def log_message(self, fmt, *args):        # keep the console for run output
        pass

    def handle_one_request(self):
        # A browser tab closing mid-SSE resets the connection; that is routine, not
        # an error worth a traceback on the demo operator's console.
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
                               "build": {"t": BUILD_T, "p": build_lanes()},
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
            # Every run is announced in the process log with what was asked for and
            # who asked: a run nobody remembers starting should be attributable.
            HUB.publish({"t": "log", "src": "bridge",
                         "line": f"run requested from {self.client_address[0]}: "
                                 f"{cfg['mode']} at {cfg['mem']}, buffer {cfg['mb']} MiB, "
                                 f"gen={cfg['gen']} chain={cfg['chain']} "
                                 f"n={cfg['n']} warmup={cfg['warmup']}"})
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
                    self.wfile.write(b": keep-alive\n\n")   # hold the connection open
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
    print(f"[bridge] argon2d builds use t={BUILD_T} p={build_lanes()}")
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
