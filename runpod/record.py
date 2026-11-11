#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["runpod>=2.1,<3", "fabric>=3.2,<4", "python-dotenv>=1.0"]
# ///
"""Rents a RunPod GPU, records a session on it, and releases it.

Copies this tree to the card, builds, runs the known-answer gate, records the
honest prover and each attack into results/sessions/, and terminates the pod.

Reads RUNPOD_API_KEY and MEMCHAL_SSH_KEY from the shell or the repo's .env.
Run with: uv run runpod/record.py
Exit: 0 done, 1 a prerequisite is missing, 2 renting failed, 3 the pod failed.
"""
import argparse
import io
import os
import secrets
import subprocess
import sys
import tarfile
import time
from datetime import datetime, timezone
from pathlib import Path

import runpod
from dotenv import load_dotenv
from fabric import Connection
from runpod.cli.utils.rp_info import get_pod_ssh_ip_port

ROOT = Path(__file__).resolve().parent.parent
POD_REPO = "/root/memchal"
POD_SESSION = "/root/session.jsonl"
SESSIONS = ROOT / "results" / "sessions"
SSH_WAIT_S = 900
START_TIMEOUT_S = 1800
RECORD_TIMEOUT_S = 3600


def say(title):
    print(f"\n== {title} ==", flush=True)


def fail(message, code):
    print(f"\nFAILED: {message}", file=sys.stderr, flush=True)
    sys.exit(code)


def connect(host, port, key):
    return Connection(host, user="root", port=port, connect_timeout=15,
                      connect_kwargs={"key_filename": key, "look_for_keys": False, "allow_agent": False})


def confirm_price(args):
    say("price")
    try:
        gpu = runpod.get_gpu(args.gpu)
    except ValueError:
        fail(f"no card with id {args.gpu!r}; list them with runpod.get_gpus()", 1)
    price = (gpu.get("price") or {}).get(args.cloud.lower())
    stock = gpu.get("availability", "?")
    if price is None:
        fail(f"no {args.cloud} price for {args.gpu}; not renting without one", 2)
    print(f"{args.gpu}, {args.cloud} cloud: ${price}/hr, stock {stock}")
    if not args.yes and input("\nRent it? It bills by the second until the recording ends. [y/N] ").strip().lower() != "y":
        print("Nothing rented.")
        sys.exit(0)


def rent(args):
    """Creates the pod and returns its id. A pod created despite an error is found by name and terminated."""
    say("renting")
    name = f"memchal-record-{datetime.now(timezone.utc):%Y%m%dT%H%M%S}-{secrets.token_hex(3)}"
    try:
        return runpod.create_pod(name, image_name=args.image, gpu_type_id=args.gpu, cloud_type=args.cloud,
                                 container_disk_in_gb=args.disk, ports="22/tcp")["id"]
    except Exception as err:
        print(f"create_pod failed: {err}")
    for pod in runpod.get_pods():
        if pod.get("name") == name:
            runpod.terminate_pod(pod["id"])
            fail(f"pod {pod['id']} was created despite the error and has been terminated", 2)
    fail(f"could not rent a {args.gpu}. Try --gpu with another card, or --cloud COMMUNITY.", 2)


def wait_for_ssh(pod_id, key):
    """Returns a connection once the pod answers over ssh, or None after SSH_WAIT_S."""
    say("waiting for ssh")
    deadline = time.monotonic() + SSH_WAIT_S
    try:
        host, port = get_pod_ssh_ip_port(pod_id, timeout=SSH_WAIT_S)
    except TimeoutError as err:
        print(err)
        return None
    conn = connect(host, port, key)
    while time.monotonic() < deadline:
        try:
            conn.run("true", hide=True)
            print(f"ssh -i {key} -p {port} root@{host}")
            return conn
        except Exception:
            time.sleep(10)
    return None


def ship(conn):
    """Copies every file git would track, committed or not, to POD_REPO."""
    say("copying this working tree")
    listed = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                            cwd=ROOT, capture_output=True, check=True).stdout.decode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode="w:gz") as tar:
        for name in filter(None, listed.split("\0")):
            if (ROOT / name).is_file():
                tar.add(ROOT / name, arcname=name)
    archive.seek(0)
    conn.put(archive, "/root/memchal.tar.gz")
    conn.run(f"mkdir -p {POD_REPO} && tar xzf /root/memchal.tar.gz -C {POD_REPO}", hide=True)


def prepare(conn, args):
    say("building and running the known-answer gate")
    result = conn.run(f"bash {POD_REPO}/runpod/gpu_pod_prepare.sh {args.idle_min} {args.max_hours}",
                      warn=True, timeout=START_TIMEOUT_S)
    reasons = {1: "this card is unusable (see above)",
               2: "the build or the known-answer gate failed; do not trust anything this card measures"}
    if result.exited != 0:
        fail(reasons.get(result.exited, f"gpu_pod_prepare.sh exited {result.exited}"), 3)


def record(args):
    confirm_price(args)
    pod_id = rent(args)
    print(f"pod {pod_id}")
    try:
        conn = wait_for_ssh(pod_id, args.key)
        if conn is None:
            fail(f"pod {pod_id} never answered over ssh", 2)
        ship(conn)
        prepare(conn, args)
        say("recording")
        recorded = conn.run(f"python3 {POD_REPO}/scripts/gpu_record.py --out {POD_SESSION}",
                            warn=True, timeout=RECORD_TIMEOUT_S)
        SESSIONS.mkdir(parents=True, exist_ok=True)
        dest = SESSIONS / f"{datetime.now(timezone.utc):%Y%m%d-%H%M%S}.jsonl"
        conn.get(POD_SESSION, str(dest))
    finally:
        terminate(pod_id)
    say("recorded")
    print(f"  {dest.relative_to(ROOT)}")
    if recorded.exited != 0:
        fail("a run failed and is missing from the session (see above)", 3)


def terminate(pod_id):
    say("terminating")
    runpod.terminate_pod(pod_id)
    for _ in range(10):
        pod = runpod.get_pod(pod_id)
        if pod is None or pod.get("status") == "TERMINATED":
            print(f"pod {pod_id} is terminated and no longer billing")
            return
        time.sleep(3)
    fail(f"{pod_id} still reports {pod.get('status')}; check the RunPod console", 3)


def main():
    load_dotenv(ROOT / ".env")
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--gpu", default="NVIDIA H100 80GB HBM3", help="RunPod GPU type id")
    p.add_argument("--cloud", default="SECURE", choices=["SECURE", "COMMUNITY"])
    p.add_argument("--image", default="runpod/pytorch:1.0.2-cu1281-torch280-ubuntu2404",
                   help="an image that starts sshd with the account's keys and has the CUDA compiler")
    p.add_argument("--disk", type=int, default=60, help="container disk in GB")
    p.add_argument("--key", default=os.environ.get("MEMCHAL_SSH_KEY"),
                   help="SSH private key registered on the account (default: MEMCHAL_SSH_KEY)")
    p.add_argument("--idle-min", type=int, default=90, help="the pod terminates itself after this many idle minutes")
    p.add_argument("--max-hours", type=float, default=6, help="the pod terminates itself this long after it starts")
    p.add_argument("--yes", "-y", action="store_true", help="rent without asking")
    args = p.parse_args()

    if not os.environ.get("RUNPOD_API_KEY"):
        fail("RUNPOD_API_KEY is not set. Export it or put it in .env (see .env.example).", 1)
    runpod.api_key = os.environ["RUNPOD_API_KEY"]
    args.key = os.path.expanduser(args.key or "")
    if not os.path.isfile(args.key):
        fail("MEMCHAL_SSH_KEY or --key must name an SSH private key registered on your RunPod account.", 1)
    if args.idle_min <= 0 or args.max_hours <= 0:
        fail("--idle-min and --max-hours take positive numbers", 1)
    record(args)


if __name__ == "__main__":
    main()
