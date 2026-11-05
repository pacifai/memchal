"""Parts shared by the CPU and GPU demo bridges: event fan-out, run lifecycle, and HTTP."""
import json
import queue
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

STATIC = Path(__file__).resolve().parent / "static"
CONTENT_TYPES = {".html": "text/html; charset=utf-8",
                 ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8"}
LOCAL_HOSTS = ("127.0.0.1", "localhost")
RUN_START_PHASES = ("building", "starting prover")
IDLE_PHASES = ("idle", "done", "stopped", "error")
MIN_CHAIN_DEPTH = 16
FAILURE_MARKS = ("FATAL", "ERROR", "refusing:")
HISTORY_LEN = 40
QUEUE_LEN = 4096


def clamp(limits, name, value):
    """Coerce one field to the type of its default and clamp it to its range."""
    lo, hi, default = limits[name]
    cast = float if isinstance(default, float) else int
    try:
        v = cast(value)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, v))


def pick(req, name, choices, default):
    """Return req[name] if it is one of `choices`, else `default`."""
    value = req.get(name)
    return value if value in choices else default


def chain_error(cfg):
    """Return why the verifier would refuse a dependent chain this shallow, or None."""
    depth = cfg["samples"] // max(1, cfg["lanes"])
    if cfg["chain"] != "dependent" or depth >= MIN_CHAIN_DEPTH:
        return None
    return (f"samples per lane is {depth}, below the minimum of {MIN_CHAIN_DEPTH} for a "
            f"dependent chain. Raise samples to at least {MIN_CHAIN_DEPTH * cfg['lanes']}, "
            f"or lower lanes.")


class Subscriber:
    """One browser tab's event queue. `closed` is set when the tab falls too far behind."""

    def __init__(self):
        self.queue = queue.Queue(maxsize=QUEUE_LEN)
        self.closed = False


class Hub:
    """Fans events out to browser tabs and keeps enough state to replay to a new tab.

    sticky: event kinds whose latest instance is always replayed.
    live:   event kinds whose latest instance is replayed while a run is in progress.
    """

    def __init__(self, sticky=(), live=("run",)):
        self.lock = threading.Lock()
        self.subscribers = []
        self.sticky_kinds, self.live_kinds = sticky, live
        self.sticky, self.live = {}, {}
        self.last_status = {"t": "status", "phase": "idle"}
        self.history = []

    def _replay(self):
        events = [self.sticky[k] for k in self.sticky_kinds if k in self.sticky]
        if self.history:
            events.append({"t": "history", "runs": self.history})
        events.append(self.last_status)
        if self.last_status.get("phase") not in IDLE_PHASES:
            events += [self.live[k] for k in self.live_kinds if k in self.live]
        return events

    def _remember(self, event):
        kind = event.get("t")
        if kind == "status":
            self.last_status = event
            if event.get("phase") in RUN_START_PHASES:
                self.live = {}
        elif kind in self.sticky_kinds:
            self.sticky[kind] = event
        elif kind in self.live_kinds:
            self.live[kind] = event
        if kind == "end":
            self.history = (self.history + [{**event, "cfg": self.live.get("run", {}).get("cfg")}])
            del self.history[:-HISTORY_LEN]

    def subscribe(self):
        sub = Subscriber()
        with self.lock:
            for event in self._replay():
                sub.queue.put_nowait(event)
            self.subscribers.append(sub)
        return sub

    def unsubscribe(self, sub):
        with self.lock:
            if sub in self.subscribers:
                self.subscribers.remove(sub)

    def publish(self, event):
        with self.lock:
            self._remember(event)
            for sub in list(self.subscribers):
                try:
                    sub.queue.put_nowait(event)
                except queue.Full:
                    sub.closed = True
                    self.subscribers.remove(sub)


class Run:
    """One prover and verifier run on a background thread. Subclasses implement `execute`."""

    def __init__(self, hub, cfg):
        self.hub = hub
        self.cfg = cfg
        self.saw_end = False
        self.stopping = False
        self.failures = {}
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping = True

    def emit(self, event):
        self.hub.publish(event)

    def status(self, phase, **kw):
        self.emit({"t": "status", "phase": phase, "cfg": self.cfg, **kw})

    def log(self, line, src="bridge"):
        self.emit({"t": "log", "src": src, "line": line.rstrip()})

    def pump(self, stream, src):
        """Publish '@' lines from a process's output as events and the rest as log lines."""
        for raw in stream:
            line = raw.rstrip("\n")
            if any(mark in line for mark in FAILURE_MARKS):
                self.failures[src] = line.strip()
            if not line.startswith("@"):
                if line:
                    self.log(line, src)
                continue
            try:
                event = json.loads(line[1:])
            except json.JSONDecodeError:
                self.log(f"unparseable event: {line[:80]}", src)
                continue
            event["src"] = src
            if event.get("t") == "end":
                self.saw_end = True
            if event.get("t") == "run":
                event["cfg"] = self.cfg
            self.emit(event)

    def failure(self):
        """The last failure line the run printed, preferring the prover's over the verifier's."""
        return self.failures.get("prover") or self.failures.get("verifier")

    def finish(self, rc, why=None):
        """Publish the final status from the verifier's exit code and what the run printed."""
        if self.stopping:
            self.status("stopped", rc=rc)
        elif why or rc != 0 or not self.saw_end:
            self.status("error", rc=rc,
                        error=why or self.failure() or f"the verifier exited {rc} without a result")
        else:
            self.status("done", rc=rc)

    def _run(self):
        try:
            self.execute()
        except Exception as exc:
            self.status("error", error=f"{type(exc).__name__}: {exc}")
        finally:
            self.cleanup()

    def execute(self):
        raise NotImplementedError

    def cleanup(self):
        pass


class Manager:
    """Runs one Run at a time."""

    def __init__(self):
        self.lock = threading.Lock()
        self.run = None

    def start(self, run):
        """Start `run` and return None, or return why it cannot start."""
        with self.lock:
            if self.run and self.run.thread.is_alive():
                return "a run is already in progress; stop it first"
            self.run = run
            run.start()
            return None

    def stop(self):
        with self.lock:
            if self.run:
                self.run.stop()


class QuietServer(ThreadingHTTPServer):
    """A threading HTTP server that ignores clients that disconnect."""
    daemon_threads = True

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError, ConnectionAbortedError)):
            return
        super().handle_error(request, client_address)


class Handler(BaseHTTPRequestHandler):
    """Serves the static page, the event stream, and the JSON endpoints.

    Subclasses set `hub`, `manager`, and `index`, and implement `start_run(req)`,
    which returns (code, body). `get_routes` maps extra GET paths to callables
    returning a JSON body.
    """
    protocol_version = "HTTP/1.1"
    hub = None
    manager = None
    index = "index.html"
    get_routes = {}

    def log_message(self, *args):
        pass

    def send(self, code, body, ctype="application/json"):
        raw = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def send_json(self, obj, code=200):
        self.send(code, json.dumps(obj))

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/events":
            return self.events()
        if path in self.get_routes:
            return self.send_json(self.get_routes[path]())
        name = self.index if path in ("/", "/index.html") else path.lstrip("/")
        target = (STATIC / name).resolve()
        if target.parent != STATIC or not target.is_file():
            return self.send_json({"error": "not found"}, 404)
        return self.send(200, target.read_bytes(), CONTENT_TYPES.get(target.suffix, "application/octet-stream"))

    def same_origin(self):
        """True for a JSON request from a page served on this machine."""
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        origin = self.headers.get("Origin")
        return ctype == "application/json" and (origin is None or urlsplit(origin).hostname in LOCAL_HOSTS)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if not self.same_origin():
            return self.send_json({"error": "only JSON requests from a local page are accepted"}, 403)
        length = int(self.headers.get("Content-Length") or 0)
        try:
            req = json.loads(self.rfile.read(length) if length else b"{}")
        except json.JSONDecodeError:
            return self.send_json({"error": "bad json"}, 400)
        if path == "/run":
            code, body = self.start_run(req if isinstance(req, dict) else {})
            return self.send_json(body, code)
        if path == "/stop":
            self.manager.stop()
            return self.send_json({"stopped": True})
        return self.send_json({"error": "not found"}, 404)

    def start_run(self, req):
        raise NotImplementedError

    def events(self):
        sub = self.hub.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.close_connection = True
        try:
            while not sub.closed:
                try:
                    payload = f"data: {json.dumps(sub.queue.get(timeout=15))}\n\n"
                except queue.Empty:
                    payload = ": keepalive\n\n"
                self.wfile.write(payload.encode())
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.hub.unsubscribe(sub)
