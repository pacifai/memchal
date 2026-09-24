#!/usr/bin/env python3
"""bridge_gpu.py — web server for the GPU demo, run ON the GPU box.

WHAT IT IS. The same idea as bridge.py: a thin bridge between a browser page
and the real binaries, simulating nothing. Every number the page draws comes
from a prover and a verifier actually running, plus nvidia-smi for the card's
own view of itself.

WHAT IS DIFFERENT from bridge.py, and why:

  - No Docker. RunPod runs the container for you and you cannot nest another
    one inside it, so the processes are spawned directly. That also removes the
    container-teardown race bridge.py has to work around.
  - No page map. The CPU demo colours pages blue or orange from mincore(),
    which reports the kernel's own view of which pages are resident. There is
    no GPU equivalent — the driver will not tell you which lines of HBM hold
    what. The honest replacement is HBM occupancy from cudaMemGetInfo plus the
    driver's own word on what backs the pointer.
  - An HBM budget instead of a RAM budget. The CPU demo starves a prover with a
    container memory limit. Here --hog holds device memory so a managed (UVM)
    buffer cannot fit. Same experiment, different lever.
  - Clocks are recorded, not assumed. A container provider will almost
    certainly refuse to let you lock clocks, so the page shows SM clock,
    temperature, and any active throttle reason next to every measurement. A
    timing result whose clock history you did not record is not a measurement.

HOW TO RUN (on the pod, after scripts/gpu_pod_setup.sh passes):
    python3 demo/bridge_gpu.py          # then tunnel 8777 and open it
    PORT=9000 python3 demo/bridge_gpu.py

Reach it with:  ssh -L 8777:127.0.0.1:8777 <pod>     (or RunPod's HTTP proxy)

MEASUREMENT NOTE. Tracing happens after each response is sent, so it is outside
the verifier's timed window, but it does space the challenges out. This is a
demo. Headline numbers come from scripts/run_trials.py with tracing off.
"""
import json
import os
import queue
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATIC = Path(__file__).resolve().parent / "static"

PORT = int(os.environ.get("PORT", "8777"))
PROVER_PORT = int(os.environ.get("PROVER_PORT", "9700"))
DEVICE = os.environ.get("DEVICE", "0")
SMI_INTERVAL = float(os.environ.get("SMI_INTERVAL", "1.0"))

# ── what the browser may ask for ─────────────────────────────────────────────
MODES = ("honest", "fake", "uvm", "zerocopy", "partial")
GENS = ("splitmix64", "chacha")
CHAINS = ("independent", "dependent")
NONCES = ("sequential", "random")
FOLDS = ("tree", "sequential")   # tree is the parallel Merkle fold
LIMITS = {                    # field: (min, max, default)
    "mb":     (1, 262144, 4096),      # may exceed HBM on purpose, for uvm
    "k":      (1, 1 << 24, 262144),
    "lanes":  (1, 1 << 18, 16384),
    "n":      (1, 20000, 200),
    "warmup": (0, 20000, 20),
    "hog":    (0, 262144, 0),         # MiB of device memory to reserve outright
    "free":   (0, 262144, 0),         # MiB to leave free; the prover reserves
                                      # the rest itself. 0 = no reserve. Wins
                                      # over hog, because only the prover can
                                      # see how much is actually allocatable.
    "trace":  (0, 10000, 1),
    "frac":   (0, 1, 0.5),
    "offload": (0, 1, 0.1),   # partial: fraction kept off the card
    "verify": (1, 1000, 10),   # check 1 answer in N. Checking costs ~100ns per
                               # probe on the CPU, so at large k it dwarfs the
                               # prover's own work and would make a demo crawl.
    "gap":    (0, 100000, 0),  # us of idle after each challenge, outside the
                               # timed window — for watching or recording a run
                               # that would otherwise finish in a flicker
}
DEFAULTS = {"mode": "honest", "gen": "chacha", "chain": "dependent",
            "nonce": "random", "fold": "tree",
            **{k: v[2] for k, v in LIMITS.items()}}


def clamp(name, value):
    """Coerce one field to the type of its own default.

    This used to hardcode which field was a float, which silently turned an
    offload of 0.25 into int(0.25) == 0 — the knob appeared to work and did
    nothing. Deriving the type from the default means a new fractional knob
    cannot repeat it.
    """
    lo, hi, default = LIMITS[name]
    cast = float if isinstance(default, float) else int
    try:
        v = cast(value)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, v))


def validate(req):
    """Whitelist the request. Anything unrecognized falls back to a default.

    The page must not be able to talk the bridge into running something else,
    so every process is spawned with an argument list and never a shell string.
    """
    out = {
        "mode":  req.get("mode")  if req.get("mode")  in MODES  else DEFAULTS["mode"],
        "gen":   req.get("gen")   if req.get("gen")   in GENS   else DEFAULTS["gen"],
        "chain": req.get("chain") if req.get("chain") in CHAINS else DEFAULTS["chain"],
        "nonce": req.get("nonce") if req.get("nonce") in NONCES else DEFAULTS["nonce"],
        "fold":  req.get("fold")  if req.get("fold")  in FOLDS  else DEFAULTS["fold"],
    }
    for name in LIMITS:
        out[name] = clamp(name, req.get(name, LIMITS[name][2]))

    # The verifier refuses a dependent chain shallower than 16, so tell the page
    # why rather than letting it watch the process exit non-zero.
    depth = out["k"] // max(1, out["lanes"])
    out["chain_depth"] = depth
    return out


# ── event hub (server-sent events) ───────────────────────────────────────────
class Hub:
    def __init__(self):
        self.lock = threading.Lock()
        self.subscribers = []
        self.last_status = {"t": "status", "phase": "idle"}
        self.last_run = None
        self.last_gpuinfo = None
        self.last_smi = None
        self.history = []

    def replay(self):
        events = []
        if self.last_gpuinfo:
            events.append(self.last_gpuinfo)
        if self.history:
            events.append({"t": "history", "runs": self.history})
        events.append(self.last_status)
        if self.last_smi:
            events.append(self.last_smi)
        if self.last_status.get("phase") in ("running", "starting prover") and self.last_run:
            events.append(self.last_run)
        return events

    def subscribe(self):
        q = queue.Queue(maxsize=4096)
        with self.lock:
            for event in self.replay():
                q.put_nowait(event)
            self.subscribers.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subscribers:
                self.subscribers.remove(q)

    def publish(self, event):
        with self.lock:
            kind = event.get("t")
            if kind == "status":
                self.last_status = event
            elif kind == "run":
                self.last_run = event
            elif kind == "gpuinfo":
                self.last_gpuinfo = event
            elif kind == "smi":
                self.last_smi = event
            elif kind == "end":
                summary = dict(event)
                if self.last_run:
                    summary["cfg"] = self.last_run.get("cfg")
                self.history = (self.history + [summary])[-8:]
            dead = []
            for q in self.subscribers:
                try:
                    q.put_nowait(event)
                except queue.Full:
                    dead.append(q)
            for q in dead:
                self.subscribers.remove(q)


HUB = Hub()


# ── the card's own view of itself ────────────────────────────────────────────
SMI_FIELDS = ("clocks.sm", "clocks.mem", "temperature.gpu", "utilization.gpu",
              "memory.used", "memory.total", "power.draw",
              "clocks_throttle_reasons.active")


def smi_sample():
    """One nvidia-smi reading, or None if the tool is unavailable."""
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


# ── the run itself ───────────────────────────────────────────────────────────
RUNS_DIR = ROOT / "results" / "demo_runs"


class Run:
    """One prover+verifier run. Owns both processes and kills them on stop().

    Every event the run produces is also appended to
    results/demo_runs/<timestamp>-<mode>.jsonl. The page keeps only the last
    few thousand points in memory and loses them on reload, so without this
    file a run that showed something interesting cannot be looked at again.
    """

    def __init__(self, cfg):
        self.cfg = cfg
        self.saw_end = False
        self.stopping = False
        self.last_failure = None
        self.prover = None
        self.verifier = None
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.record_path = RUNS_DIR / f"{stamp}-{cfg['mode']}.jsonl"
        self.record = None
        self.record_lock = threading.Lock()
        try:
            RUNS_DIR.mkdir(parents=True, exist_ok=True)
            self.record = self.record_path.open("w", buffering=1)
            self._write({"t": "meta", "started": stamp, "cfg": cfg,
                         "host": os.uname().nodename})
        except OSError as exc:
            print(f"[bridge] cannot record run: {exc}", flush=True)
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _write(self, event):
        """Append one event to the run file. Never let recording break a run."""
        if not self.record:
            return
        try:
            with self.record_lock:
                self.record.write(json.dumps(event) + "\n")
        except (OSError, ValueError):
            self.record = None

    def _close_record(self):
        if self.record:
            try:
                self.record.close()
            except OSError:
                pass
            self.record = None

    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping = True
        for p in (self.verifier, self.prover):
            if p and p.poll() is None:
                p.kill()

    def _status(self, phase, **kw):
        event = {"t": "status", "phase": phase, "cfg": self.cfg, **kw}
        self._write(event)
        HUB.publish(event)

    def _log(self, line, src="bridge"):
        event = {"t": "log", "src": src, "line": line.rstrip()}
        self._write(event)
        HUB.publish(event)

    def _pump(self, stream, src):
        """Forward one process's stdout: '@' lines are events, the rest are log."""
        for raw in stream:
            line = raw.rstrip("\n")
            # Keep the cause, not its echo: when the prover dies, the verifier
            # then fails to send, and that later line would otherwise replace
            # the one that says why.
            if "FATAL" in line:
                self.last_failure = line.strip()
            elif "send fail" in line and self.last_failure is None:
                self.last_failure = line.strip()
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
                self._write(event)
                HUB.publish(event)
            elif line:
                self._log(line, src)

    def _prover_cmd(self):
        c = self.cfg
        # honest mode runs the honest binary, so the baseline really is the
        # verify-and-refuse path rather than the cheat binary being polite.
        if c["mode"] == "honest":
            return [str(ROOT / "prover_cuda"), "--port", str(PROVER_PORT),
                    "--device", DEVICE, "--trace", str(c["trace"]),
                    "--lane-cap", str(max(1, c["lanes"]))]
        reserve = (["--leave-free", str(c["free"])] if c["free"] > 0
                   else ["--hog", str(c["hog"])])
        return [str(ROOT / "cheating_prover_cuda"), "--port", str(PROVER_PORT),
                "--device", DEVICE, "--mode", c["mode"], "--frac", str(c["frac"]),
                *reserve, "--offload", str(c["offload"]),
                "--trace", str(c["trace"]),
                "--lane-cap", str(max(1, c["lanes"]))]

    def _verifier_cmd(self):
        c = self.cfg
        return [str(ROOT / "verifier"), "--host", "127.0.0.1",
                "--port", str(PROVER_PORT),
                "--mb", str(c["mb"]), "--k", str(c["k"]), "--lanes", str(c["lanes"]),
                "--n", str(c["n"]), "--warmup", str(c["warmup"]),
                "--gen", c["gen"], "--chain", c["chain"], "--nonce", c["nonce"],
                "--fold", c["fold"],
                "--label", f"gpu-{c['mode']}",
                "--csv", "/dev/null", "--stream", "-", "--probe-map", "0",
                "--gap-us", str(c["gap"]),
                "--verify-every", str(c["verify"])]

    def _run(self):
        try:
            c = self.cfg
            for binary in ("verifier",
                           "prover_cuda" if c["mode"] == "honest" else "cheating_prover_cuda"):
                if not (ROOT / binary).exists():
                    return self._status("error",
                                        error=f"{binary} is not built. Run `make gpu` "
                                              f"(and `make` for the verifier) first.")

            self._status("starting prover")
            self.prover = subprocess.Popen(self._prover_cmd(), stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, text=True, bufsize=1)
            threading.Thread(target=self._pump, args=(self.prover.stdout, "prover"),
                             daemon=True).start()

            deadline = time.time() + 60
            while time.time() < deadline and not self.stopping:
                if self.prover.poll() is not None:
                    return self._status("error",
                                        error="the prover exited during startup — see the log")
                if _port_open(PROVER_PORT):
                    break
                time.sleep(0.1)
            else:
                if not self.stopping:
                    return self._status("error", error="prover did not come up")
            if self.stopping:
                return self._status("stopped")

            # Generating a multi-GB buffer with ChaCha20 and pushing it over the
            # wire takes seconds to tens of seconds, and until it finishes there
            # is nothing to draw. Saying so beats looking hung.
            self._status("uploading", mb=c["mb"])
            self.verifier = subprocess.Popen(self._verifier_cmd(), stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, text=True, bufsize=1)
            self._pump(self.verifier.stdout, "verifier")
            rc = self.verifier.wait()
            time.sleep(0.5)          # let the prover flush its closing lines
            if self.stopping:
                self._status("stopped", rc=rc)
            elif rc != 0 or not self.saw_end or (self.last_failure and "FATAL" in self.last_failure):
                # A run that ends without a result is a failure, whatever the
                # exit code. Reporting it as "done" made a prover that died
                # before its first challenge look like a run that did nothing.
                # So is one whose prover died mid-run: the verifier still
                # prints a summary of the challenges it got, which set saw_end.
                why = self.last_failure or f"the verifier exited {rc} without a result"
                self._status("error", rc=rc, error=why)
            else:
                self._status("done", rc=rc)
        except Exception as exc:                  # a crashed run must not kill the server
            self._status("error", error=f"{type(exc).__name__}: {exc}")
        finally:
            for p in (self.verifier, self.prover):
                if p and p.poll() is None:
                    p.kill()
            if self.record:
                print(f"[bridge] run recorded: {self.record_path}", flush=True)
            self._close_record()


def _port_open(port):
    import socket
    with socket.socket() as s:
        s.settimeout(0.2)
        return s.connect_ex(("127.0.0.1", port)) == 0


class Manager:
    def __init__(self):
        self.lock = threading.Lock()
        self.run = None

    def start(self, cfg):
        with self.lock:
            if self.run and self.run.thread.is_alive():
                return False, "a run is already in flight"
            self.run = Run(cfg)
            self.run.start()
            return True, None

    def stop(self):
        with self.lock:
            if self.run:
                self.run.stop()


MANAGER = Manager()

class QuietServer(ThreadingHTTPServer):
    """ThreadingHTTPServer that does not treat a dropped client as an incident.

    Every closed browser tab resets an SSE connection, and the default handler
    prints a full traceback for each one — which buries the output the operator
    is actually reading.
    """
    daemon_threads = True

    def handle_error(self, request, client_address):
        import sys
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionResetError, BrokenPipeError, ConnectionAbortedError)):
            return
        super().handle_error(request, client_address)


CONTENT_TYPES = {".html": "text/html; charset=utf-8",
                 ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):            # keep the console for the run's own output
        pass

    def _send(self, code, body, ctype="application/json"):
        raw = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/events":
            return self._events()
        name = "gpu.html" if path in ("/", "/index.html") else path.lstrip("/")
        target = (STATIC / name).resolve()
        if not str(target).startswith(str(STATIC.resolve())) or not target.is_file():
            return self._send(404, json.dumps({"error": "not found"}))
        ctype = CONTENT_TYPES.get(target.suffix, "application/octet-stream")
        return self._send(200, target.read_bytes(), ctype)

    def _events(self):
        q = HUB.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            while True:
                try:
                    event = q.get(timeout=15)
                    payload = f"data: {json.dumps(event)}\n\n"
                except queue.Empty:
                    payload = ": keepalive\n\n"
                self.wfile.write(payload.encode())
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            HUB.unsubscribe(q)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            req = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            return self._send(400, json.dumps({"error": "bad json"}))

        if path == "/run":
            cfg = validate(req if isinstance(req, dict) else {})
            # The verifier refuses a dependent chain shallower than 16, so say so
            # now with a number the operator can act on, rather than starting a
            # prover and reporting a process exit.
            if cfg["chain"] == "dependent" and cfg["chain_depth"] < 16:
                return self._send(400, json.dumps({
                    "error": f"chain depth k/lanes = {cfg['chain_depth']} is below the "
                             f"floor of 16, so the verifier would refuse. Raise k to at "
                             f"least {16 * cfg['lanes']}, or lower lanes."}))
            ok, err = MANAGER.start(cfg)
            if not ok:
                return self._send(409, json.dumps({"error": err}))
            return self._send(200, json.dumps({"ok": True, "cfg": cfg}))
        if path == "/stop":
            MANAGER.stop()
            return self._send(200, json.dumps({"ok": True}))
        return self._send(404, json.dumps({"error": "not found"}))


def preflight():
    print(f"[bridge] repo root: {ROOT}", flush=True)
    missing = [b for b in ("verifier", "prover_cuda", "cheating_prover_cuda")
               if not (ROOT / b).exists()]
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
    server = QuietServer(("127.0.0.1", PORT), Handler)
    print(f"[bridge] http://127.0.0.1:{PORT}  (tunnel this port to reach it)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] stopping")
        MANAGER.stop()


if __name__ == "__main__":
    main()
