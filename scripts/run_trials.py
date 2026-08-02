#!/usr/bin/env python3
"""Repeat-trials driver for the memory-challenge PoC.

Runs each prover mode R independent times (each run = a fresh verifier connection,
which triggers a fresh upload + fresh alloc/mlock/file-spill on the server), reads
the verifier's JSON summary (never its printf text), and reports central tendency
AND run-to-run spread. Percentiles the sample size can't support are withheld, not
printed as if they were real.

Environment (VM vs bare-metal, page size, cpus, memory limit) is stamped into the
output so nobody later mistakes a VM number for a bare-metal one.

Usage (inside the container, after `make`):
    python3 scripts/run_trials.py --repeats 10 --n 3000 --k 4096 --mb 64 \\
        --modes honest,disk,partial,fake --out results/trials.json
"""
from __future__ import annotations
import argparse, subprocess, time, socket, os, sys, json, platform

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mc_stats as m

# mode -> how to launch its server. {port} is substituted at run time.
# honest uses the real honest server (prover.cpp); cheats use cheating_prover.
MODE_SERVERS = {
    "honest":   ["./prover", "{port}"],
    "disk":     ["./cheating_prover", "--port", "{port}", "--mode", "disk"],
    "partial":  ["./cheating_prover", "--port", "{port}", "--mode", "partial", "--frac", "0.5"],
    "fake":     ["./cheating_prover", "--port", "{port}", "--mode", "fake", "--frac", "0.5"],
}

PCTLS = ["min", "p50", "p90", "p99", "p99.9", "max"]


def wait_port(port: int, timeout: float = 10.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket() as s:
            s.settimeout(0.3)
            try:
                s.connect(("127.0.0.1", port))
                return True
            except OSError:
                time.sleep(0.1)
    return False


def detect_env() -> dict:
    """Best-effort environment stamp."""
    env = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "in_container": os.path.exists("/.dockerenv"),
        "online_cpus": os.sysconf("SC_NPROCESSORS_ONLN") if hasattr(os, "sysconf") else None,
        "page_size": os.sysconf("SC_PAGESIZE") if hasattr(os, "sysconf") else None,
    }
    # cgroup v2 limits (set by run.sh via --cpus/--memory)
    try:
        with open("/sys/fs/cgroup/cpu.max") as f:
            raw = f.read().strip()
            env["cgroup_cpu_max"] = raw
            quota, period = raw.split()
            env["cpus"] = "max" if quota == "max" else round(int(quota) / int(period), 2)
    except (OSError, ValueError):
        pass
    try:
        with open("/sys/fs/cgroup/memory.max") as f:
            env["cgroup_memory_max"] = f.read().strip()
    except OSError:
        pass
    return env


def run_one(port: int, a, out_json: str) -> dict:
    """One verifier run against an already-listening server; returns parsed JSON."""
    cmd = ["./verifier", "--host", "127.0.0.1", "--port", str(port),
           "--mb", str(a.mb), "--k", str(a.k), "--lanes", str(a.lanes),
           "--n", str(a.n), "--warmup", str(a.warmup), "--sweep", "0",
           "--csv", "/dev/null", "--json", out_json, "--label", a.label]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return m.load_json(out_json)


def run_mode(mode: str, port: int, a) -> dict:
    server = [tok.format(port=port) for tok in MODE_SERVERS[mode]]
    proc = subprocess.Popen(server, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_port(port):
            raise RuntimeError(f"server for mode '{mode}' did not come up on port {port}")
        runs = []
        for i in range(a.repeats):
            j = run_one(port, a, f"{a.tmp}/{mode}_run{i}.json")
            runs.append(j)
            print(f"    {mode} run {i+1}/{a.repeats}: "
                  f"p50={j['result']['percentiles_us']['p50']['value']:.1f}us "
                  f"errors={j['result']['errors']} "
                  f"locked={j['prover']['locked']}", flush=True)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()

    # aggregate across runs
    n_samples = runs[0]["result"]["samples"]
    pct_aggs = {name: m.aggregate([r["result"]["percentiles_us"][name]["value"] for r in runs])
                for name in PCTLS}
    return {
        "mode": mode,
        "repeats": a.repeats,
        "n_per_run": n_samples,
        "all_locked": all(r["prover"]["locked"] for r in runs),
        "any_errors": any(r["result"]["errors"] for r in runs),
        "errors_per_run": [r["result"]["errors"] for r in runs],
        "percentiles": {name: pct_aggs[name].__dict__ for name in PCTLS},
        "p50_runs": [r["result"]["percentiles_us"]["p50"]["value"] for r in runs],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modes", default="honest,disk,partial,fake")
    ap.add_argument("--repeats", type=int, default=10)
    ap.add_argument("--n", type=int, default=3000)
    ap.add_argument("--k", type=int, default=4096)
    ap.add_argument("--lanes", type=int, default=32)
    ap.add_argument("--mb", type=float, default=64.0)
    ap.add_argument("--warmup", type=int, default=300)
    ap.add_argument("--port", type=int, default=9700)
    ap.add_argument("--label", default="")
    ap.add_argument("--tmp", default="/tmp/mc_trials")
    ap.add_argument("--out", default="results/trials.json")
    a = ap.parse_args()
    os.makedirs(a.tmp, exist_ok=True)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)

    env = detect_env()
    if not a.label:
        a.label = ("container" if env["in_container"] else "host") + \
                  f"/{env['machine']}/{env.get('cpus','?')}cpu/{env['page_size']}Bpage"
    print(f"env: {env}")
    print(f"config: modes={a.modes} repeats={a.repeats} n={a.n} k={a.k} "
          f"lanes={a.lanes} mb={a.mb} warmup={a.warmup}")
    print(f"label: {a.label}\n")

    results = []
    for mode in a.modes.split(","):
        mode = mode.strip()
        if mode not in MODE_SERVERS:
            print(f"  SKIP unknown mode '{mode}'"); continue
        print(f"  === mode: {mode} ===", flush=True)
        results.append(run_mode(mode, a.port, a))

    report = {"schema": "memchal.trials.v1", "env": env, "label": a.label,
              "config": {"repeats": a.repeats, "n": a.n, "k": a.k, "lanes": a.lanes,
                         "mb": a.mb, "warmup": a.warmup},
              "modes": results}
    with open(a.out, "w") as f:
        json.dump(report, f, indent=2)

    print_table(report)
    print(f"\nwrote {a.out}")


def print_table(report: dict):
    n = report["config"]["n"]
    print(f"\n=== repeat-trials summary  (per-run p50 across {report['config']['repeats']} runs; "
          f"n={n} samples/run; k={report['config']['k']}; {report['label']}) ===")
    print(f"{'mode':<9} {'p50 mean±sd (us)':<22} {'p99 (us)':<18} "
          f"{'p99.9':<14} {'locked':<7} {'errors/run'}")
    for r in report["modes"]:
        p50 = m.Agg(**r["percentiles"]["p50"])
        p50s = f"{p50.mean:.1f} ± {p50.sd:.1f}"
        # inner percentiles: withhold if the sample size can't support them
        def cell(name):
            if not m.reliable(float(name[1:]) if name != "min" and name != "max" else 0, n):
                return f"n/a (n={n})"
            a_ = m.Agg(**r["percentiles"][name])
            return f"{a_.mean:.1f}±{a_.sd:.1f}"
        p99 = cell("p99")
        p999 = cell("p99.9")
        errs = "/".join(str(e) for e in r["errors_per_run"])
        print(f"{r['mode']:<9} {p50s:<22} {p99:<18} {p999:<14} "
              f"{str(r['all_locked']):<7} {errs}")


if __name__ == "__main__":
    main()
