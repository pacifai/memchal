#!/usr/bin/env python3
"""Records one session on a GPU machine: the honest prover, then each attack, against one verifier setting.

The session file holds, for each run, a meta line with the run's settings, then every event the
prover and the verifier printed. The demo site replays it.

Usage: python3 scripts/gpu_record.py --out results/sessions/NAME.jsonl
Exit: 0 every run was recorded, 1 a run failed and was left out.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

BUILD = Path(__file__).resolve().parent.parent / "build"
VERIFIER = BUILD / "verifier"
HONEST_PROVER = BUILD / "gpu" / "prover"
CHEATING_PROVER = BUILD / "attacks" / "gpu" / "cheating_prover"

PROVER_PORT = 9700
PROVER_START_S = 60
PROVER_FLUSH_S = 0.5
VERIFIER_TIMEOUT_S = 900
LOG_TAIL = 8

SETTINGS = {"mb": 4096, "samples": 262144, "lanes": 16384, "challenges": 200, "warmup": 10,
            "gen": "argon2d", "build_passes": 2, "build_parallelism": 16,
            "chain": "dependent", "nonce": "random", "fold": "tree", "verify": 10}
RUNS = [
    {"mode": "honest"},
    {"mode": "fake", "frac": 0.5},
    {"mode": "partial", "offload": 0.1},
    {"mode": "partial", "offload": 1.0},
    {"mode": "uvm", "free": 2400, "challenges": 20, "warmup": 2},
]
ATTACK_FLAGS = {"frac": "--frac", "offload": "--offload", "free": "--leave-free"}


def prover_cmd(cfg):
    common = ["--port", str(PROVER_PORT), "--trace", "1", "--lane-cap", str(cfg["lanes"])]
    if cfg["mode"] == "honest":
        return [str(HONEST_PROVER), *common]
    attack = [arg for key, flag in ATTACK_FLAGS.items() if key in cfg for arg in (flag, str(cfg[key]))]
    return [str(CHEATING_PROVER), "--mode", cfg["mode"], *attack, *common]


def verifier_cmd(cfg):
    return [str(VERIFIER), "--host", "127.0.0.1", "--port", str(PROVER_PORT),
            "--mb", str(cfg["mb"]), "--samples", str(cfg["samples"]), "--lanes", str(cfg["lanes"]),
            "--challenges", str(cfg["challenges"]), "--warmup", str(cfg["warmup"]),
            "--gen", cfg["gen"], "--build-passes", str(cfg["build_passes"]),
            "--build-parallelism", str(cfg["build_parallelism"]),
            "--chain", cfg["chain"], "--nonce", cfg["nonce"], "--fold", cfg["fold"],
            "--verify-every", str(cfg["verify"]), "--label", f"gpu-{cfg['mode']}",
            "--csv", "/dev/null", "--stream", "-", "--probe-map", "0"]


def collect(stream, src, events, lock):
    """Appends each line a process prints: '@' lines as the event they carry, the rest as log lines."""
    for raw in stream:
        line = raw.rstrip("\n")
        if not line:
            continue
        event = {"t": "log", "line": line}
        if line.startswith("@"):
            try:
                event = json.loads(line[1:])
            except json.JSONDecodeError:
                pass
        with lock:
            events.append({**event, "src": src})


def spawn(cmd):
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)


def listening(prover):
    """Waits for the prover to listen. Returns False if it exited or PROVER_START_S passed."""
    deadline = time.monotonic() + PROVER_START_S
    while time.monotonic() < deadline and prover.poll() is None:
        with socket.socket() as s:
            s.settimeout(0.2)
            if s.connect_ex(("127.0.0.1", PROVER_PORT)) == 0:
                return True
        time.sleep(0.1)
    return False


def record_run(cfg):
    """Runs one prover against the verifier. Returns the run's events and why it failed, or None."""
    events = [{"t": "meta", "started": time.strftime("%Y%m%d-%H%M%S", time.gmtime()), "cfg": cfg,
               "host": os.uname().nodename}]
    lock = threading.Lock()
    prover = spawn(prover_cmd(cfg))
    reader = threading.Thread(target=collect, args=(prover.stdout, "prover", events, lock), daemon=True)
    reader.start()
    try:
        if not listening(prover):
            return events, "the prover did not start"
        verifier = spawn(verifier_cmd(cfg))
        timeout = threading.Timer(VERIFIER_TIMEOUT_S, verifier.kill)
        timeout.start()
        collect(verifier.stdout, "verifier", events, lock)
        code = verifier.wait()
        timeout.cancel()
        time.sleep(PROVER_FLUSH_S)
    finally:
        prover.kill()
        prover.wait()
        reader.join()
    if code != 0 or not any(e["t"] == "end" for e in events):
        return events, f"the verifier exited {code} without a result"
    return events, None


def label(run):
    return " ".join([run["mode"], *(f"{key}={run[key]}" for key in ATTACK_FLAGS if key in run)])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True, help="the session file to write")
    args = p.parse_args()
    for binary in (VERIFIER, HONEST_PROVER, CHEATING_PROVER):
        if not binary.exists():
            sys.exit(f"{binary} is not built. Run `make all gpu` first.")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    failed = False
    with args.out.open("w") as out:
        for run in RUNS:
            events, why = record_run({**SETTINGS, **run})
            if why:
                failed = True
                print(f"{label(run)}: FAILED, {why}", flush=True)
                for event in [e for e in events if e["t"] == "log"][-LOG_TAIL:]:
                    print(f"  {event['src']}: {event['line']}", flush=True)
                if run["mode"] == "honest":
                    break
                continue
            out.writelines(json.dumps(event) + "\n" for event in events)
            end = next(e for e in events if e["t"] == "end")
            print(f"{label(run)}: p50 {end['p50']:.1f} us, {end['errors']} wrong of {end['checked']} checked",
                  flush=True)
    print(f"session: {args.out}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
