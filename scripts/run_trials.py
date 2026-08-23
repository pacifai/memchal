#!/usr/bin/env python3
"""Repeat-trials driver for the memory-challenge PoC — interleaved-cycle edition.

All prover servers are started ONCE at session open and kept alive for the whole
session. Modes are run in INTERLEAVED CYCLES (one run of every mode per cycle,
in a shuffled order that is reproducible from --shuffle-seed) so that
between-cycle VM-level latency drift cancels in within-session comparisons.

Absolute latencies are NOT portable across sessions (Apple Silicon P/E-core
placement is fixed for a whole VM session, causing ~1.6x between-session swings).
Only within-session ratios and paired differences are trustworthy.

Usage (inside the container, after `make`):
    python3 scripts/run_trials.py --repeats 3 --n 1500 \\
        --modes floor,honest,disk,fake --shuffle-seed 42 \\
        --out results/trials.json
"""
from __future__ import annotations
import argparse, subprocess, time, socket, os, sys, json, platform, random, csv, math
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mc_stats as m

# ---------------------------------------------------------------------------
# Mode definitions
# ---------------------------------------------------------------------------
# "floor" uses the honest server but k=0 (transport-floor measurement).
# All cheating modes use cheating_prover with their own port.
MODE_MAP = {
    "floor":    {"server": "prover",          "k_override": 0,    "cheat_mode": None},
    "honest":   {"server": "prover",          "k_override": None, "cheat_mode": None},
    "disk":     {"server": "cheating_prover", "k_override": None, "cheat_mode": "disk"},
    "partial":  {"server": "cheating_prover", "k_override": None, "cheat_mode": "partial"},
    "fake":     {"server": "cheating_prover", "k_override": None, "cheat_mode": "fake"},
    "mmapdisk": {"server": "cheating_prover", "k_override": None, "cheat_mode": "mmapdisk"},
    "recompute":  {"server": "cheating_prover", "k_override": None, "cheat_mode": "recompute"},
    "prefetch":   {"server": "cheating_prover", "k_override": None, "cheat_mode": "prefetch"},
    "precompute": {"server": "cheating_prover", "k_override": None, "cheat_mode": "precompute"},
}

# Port offsets per mode from the base --port
PORT_OFFSET = {
    "floor":    0,   # shares honest prover
    "honest":   0,   # same prover instance
    "disk":     1,
    "partial":  2,
    "fake":     3,
    "mmapdisk": 4,
    "recompute":  5,
    "prefetch":   6,
    "precompute": 7,
}

PCTLS = ["min", "p50", "p90", "p99", "p99.9", "max"]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def wait_port(port: int, timeout: float = 15.0) -> bool:
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


def build_server_cmd(mode: str, port: int, args) -> list[str]:
    """Build the subprocess command to launch the server for a given mode."""
    cfg = MODE_MAP[mode]
    if cfg["server"] == "prover":
        return ["./prover", str(port)]
    else:
        cmd = ["./cheating_prover", "--port", str(port), "--mode", cfg["cheat_mode"]]
        if cfg["cheat_mode"] in ("partial", "fake"):
            cmd += ["--frac", "0.5"]
        return cmd


def run_one(port: int, k: int, a, out_json: str) -> dict:
    """One verifier run against an already-listening server; returns parsed JSON."""
    cmd = ["./verifier", "--host", "127.0.0.1", "--port", str(port),
           "--mb", str(a.mb), "--k", str(k), "--lanes", str(a.lanes),
           "--n", str(a.n), "--warmup", str(a.warmup),
           "--gen", a.gen, "--nonce", a.nonce, "--gap-us", str(a.gap_us),
           "--csv", "/dev/null", "--json", out_json, "--label", a.label]
    # floor is k=0 (no probes) so chaining is meaningless there; skip it to avoid
    # the k/lanes>=16 chain-depth floor tripping on the floor mode.
    if a.chain == "dependent" and k > 0:
        cmd += ["--chain", "dependent"]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return m.load_json(out_json)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modes", default="floor,honest,disk,partial,fake")
    ap.add_argument("--repeats", type=int, default=10)
    ap.add_argument("--n", type=int, default=3000)
    ap.add_argument("--k", type=int, default=4096)
    ap.add_argument("--lanes", type=int, default=32)
    ap.add_argument("--mb", type=float, default=64.0)
    ap.add_argument("--warmup", type=int, default=300)
    # generator / access-pattern / nonce controls (the defenses) — applied to every
    # verifier run in the session, so recompute/prefetch/precompute can be measured
    # by the harness against each defense, not just watched in the demo.
    ap.add_argument("--gen", default="splitmix64", choices=["splitmix64", "chacha"])
    ap.add_argument("--chain", default="independent", choices=["independent", "dependent"])
    ap.add_argument("--nonce", default="sequential", choices=["sequential", "random"])
    ap.add_argument("--gap-us", dest="gap_us", type=int, default=0)
    ap.add_argument("--port", type=int, default=9700)
    ap.add_argument("--label", default="")
    ap.add_argument("--tmp", default="/tmp/mc_trials")
    ap.add_argument("--out", default="results/trials.json")
    ap.add_argument("--shuffle-seed", type=int, default=None,
                    help="Seed for within-cycle mode shuffle (default: random).")
    args = ap.parse_args()

    os.makedirs(args.tmp, exist_ok=True)
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

    # Resolve shuffle seed
    if args.shuffle_seed is None:
        args.shuffle_seed = random.randint(0, 2**31 - 1)
    shuffle_seed = args.shuffle_seed

    # Session ID
    session_id = (time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
                  + f"-{random.randint(0, 9999):04d}")

    env = detect_env()
    if not args.label:
        args.label = ("container" if env["in_container"] else "host") + \
                     f"/{env['machine']}/{env.get('cpus','?')}cpu/{env['page_size']}Bpage"

    print(f"session_id: {session_id}")
    print(f"shuffle_seed: {shuffle_seed}")
    print(f"env: {env}")
    print(f"config: modes={args.modes} repeats={args.repeats} n={args.n} k={args.k} "
          f"lanes={args.lanes} mb={args.mb} warmup={args.warmup}")
    print(f"label: {args.label}\n")

    modes = [m_.strip() for m_ in args.modes.split(",") if m_.strip() in MODE_MAP]
    unknown = [m_.strip() for m_ in args.modes.split(",") if m_.strip() not in MODE_MAP]
    for u in unknown:
        print(f"  SKIP unknown mode '{u}'")

    if not modes:
        print("No valid modes; exiting.")
        return

    # Determine which physical ports/servers we need.
    # "floor" and "honest" share the same prover on args.port; other modes each get
    # their own port.
    need_prover = any(MODE_MAP[md]["server"] == "prover" for md in modes)
    ports = {}  # mode -> port
    for md in modes:
        if MODE_MAP[md]["server"] == "prover":
            ports[md] = args.port
        else:
            ports[md] = args.port + PORT_OFFSET[md]

    # Start servers — one prover (shared by floor+honest), one per cheat mode.
    procs = {}  # tag -> Popen (tag = "prover" or mode name for cheats)
    session_start = time.time()

    def start_servers():
        if need_prover:
            cmd = ["./prover", str(args.port)]
            print(f"  starting prover on port {args.port}: {cmd}")
            procs["prover"] = subprocess.Popen(
                cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if not wait_port(args.port):
                raise RuntimeError(f"prover did not come up on port {args.port}")
            print(f"  prover ready on port {args.port}")

        for md in modes:
            if MODE_MAP[md]["server"] == "cheating_prover":
                port = ports[md]
                cmd = build_server_cmd(md, port, args)
                print(f"  starting {md} server on port {port}: {cmd}")
                procs[md] = subprocess.Popen(
                    cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if not wait_port(port):
                    raise RuntimeError(f"server for '{md}' did not come up on port {port}")
                print(f"  {md} server ready on port {port}")

    def stop_servers():
        for tag, proc in procs.items():
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()

    # Per-run rows for CSV
    rows = []          # list of row dicts
    seq = 0            # global incrementing counter
    executed_orders = []  # list of mode-order lists per cycle

    try:
        start_servers()
        print()

        for cycle in range(args.repeats):
            # Shuffle mode order reproducibly: seed advances per cycle so each
            # cycle has a different (but reproducible) permutation.
            rng = random.Random(shuffle_seed + cycle)
            order = list(modes)
            rng.shuffle(order)
            executed_orders.append(order)
            print(f"--- cycle {cycle+1}/{args.repeats}  order: {order} ---", flush=True)

            for md in order:
                port = ports[md]
                k = 0 if MODE_MAP[md]["k_override"] == 0 else args.k
                out_json = f"{args.tmp}/{md}_cycle{cycle}_seq{seq}.json"
                t_rel = time.time() - session_start

                j = run_one(port, k, args, out_json)

                p50 = j["result"]["percentiles_us"]["p50"]["value"]
                p90 = j["result"]["percentiles_us"]["p90"]["value"]
                p99 = j["result"]["percentiles_us"]["p99"]["value"]
                errors = j["result"]["errors"]
                locked = j["prover"]["locked"]

                print(f"  cycle={cycle+1} seq={seq:3d} t={t_rel:6.1f}s  "
                      f"{md:<8}  p50={p50:.1f}us  p90={p90:.1f}us  "
                      f"errors={errors}  locked={locked}", flush=True)

                rows.append({
                    "session_id": session_id,
                    "cycle": cycle,
                    "seq": seq,
                    "t_rel_s": round(t_rel, 3),
                    "mode": md,
                    "p50_us": p50,
                    "p90_us": p90,
                    "p99_us": p99,
                    "errors": errors,
                    "locked": locked,
                })
                seq += 1

    finally:
        stop_servers()

    print()

    # -----------------------------------------------------------------------
    # Aggregation
    # -----------------------------------------------------------------------
    # Collect p50 values per mode, ordered by cycle (i.e. by cycle index).
    mode_p50 = {}   # mode -> list of p50 (one per cycle, in cycle order)
    mode_p90 = {}
    mode_p99 = {}
    for md in modes:
        cycle_rows = sorted([r for r in rows if r["mode"] == md], key=lambda r: r["cycle"])
        mode_p50[md] = [r["p50_us"] for r in cycle_rows]
        mode_p90[md] = [r["p90_us"] for r in cycle_rows]
        mode_p99[md] = [r["p99_us"] for r in cycle_rows]

    aggs = {md: m.aggregate(mode_p50[md]) for md in modes}

    # Paired memory-time: honest − floor (per cycle, so drift cancels)
    memory_time_diff = None
    if "honest" in modes and "floor" in modes:
        memory_time_diff = m.paired_diff(mode_p50["honest"], mode_p50["floor"])

    # Ratios vs honest
    ratios = {}
    if "honest" in modes:
        honest_agg = aggs["honest"]
        for md in modes:
            if md not in ("honest", "floor"):
                ratios[md] = m.ratio(aggs[md], honest_agg)

    # Drift detection on honest p50 ordered by seq (not cycle) if present
    drift_result = None
    step_result = None
    if "honest" in modes:
        honest_rows_by_seq = sorted([r for r in rows if r["mode"] == "honest"],
                                    key=lambda r: r["seq"])
        honest_p50_by_seq = [r["p50_us"] for r in honest_rows_by_seq]
        if len(honest_p50_by_seq) >= 3:
            drift_result = m.trend(honest_p50_by_seq)
            step_result = m.step_change(honest_p50_by_seq)

    # -----------------------------------------------------------------------
    # Print summary
    # -----------------------------------------------------------------------
    n_per_run = args.n

    print(f"=== repeat-trials summary  (per-run p50 across {args.repeats} interleaved cycles; "
          f"n={n_per_run} samples/run; k={args.k}; {args.label}) ===")
    print(f"session_id: {session_id}  "
          f"[absolute us illustrative for THIS session only; compare within-session only]")
    print()
    print(f"shuffle_seed: {shuffle_seed}")
    print("executed cycle orders:")
    for i, order in enumerate(executed_orders):
        print(f"  cycle {i+1}: {order}")
    print()

    # p99 is withheld unless the per-run sample size supports it (see mc_stats.reliable).
    # p99.9 is deliberately not reported: it needs n>=10000 samples/run, far above
    # what these runs use, and the driver only collects p50/p90/p99 per run.
    print(f"{'mode':<9} {'p50 mean±sd (us)':<24} {'p90 mean±sd (us)':<22} "
          f"{'p99 (us)':<20} {'notes'}")
    print("-" * 92)
    for md in modes:
        agg = aggs[md]
        p50s = f"{agg.mean:.1f} ± {agg.sd:.1f}"

        p90_agg = m.aggregate(mode_p90[md])
        p90s = f"{p90_agg.mean:.1f} ± {p90_agg.sd:.1f}"

        if m.reliable(99, n_per_run):
            p99_agg = m.aggregate(mode_p99[md])
            p99s = f"{p99_agg.mean:.1f} ± {p99_agg.sd:.1f}"
        else:
            p99s = f"n/a (n={n_per_run})"

        notes = []
        error_counts = [r["errors"] for r in rows if r["mode"] == md]
        total_errors = sum(error_counts)
        if total_errors > 0:
            notes.append(f"errors≈{total_errors//len(error_counts)}/run")
        if md == "floor":
            notes.append("k=0 transport floor")
        print(f"{md:<9} {p50s:<24} {p90s:<22} {p99s:<20} {', '.join(notes)}")

    print()
    print("--- memory-time (honest−floor, paired by cycle) ---")
    if memory_time_diff is not None:
        tag = "" if memory_time_diff.separable else "  [NOT SEPARABLE]"
        print(f"  honest−floor: {memory_time_diff}{tag}")
    else:
        print("  (need both 'honest' and 'floor' modes)")

    print()
    print("--- ratios vs honest ---")
    if ratios:
        for md, r_ in ratios.items():
            print(f"  {md}/honest: {r_}  (rel_err={r_.rel_error:.3f})")
    else:
        print("  (no comparison modes)")

    print()
    print("--- drift verdict (honest p50 sequence) ---")
    if drift_result is not None:
        d = m.Diff(**drift_result["diff"])
        first_agg = m.Agg(**drift_result["first"])
        last_agg = m.Agg(**drift_result["last"])
        drifted = drift_result["drifted"]
        insuff = drift_result.get("insufficient_data", False)
        print(f"  first-third mean: {first_agg.mean:.1f}us   last-third mean: {last_agg.mean:.1f}us")
        if insuff:
            print(f"  DRIFT FLAG: undetermined (each third has <2 points; run >=6 cycles for a reliable verdict)")
        else:
            print(f"  trend diff (last−first): {d}")
            print(f"  DRIFT FLAG: {'YES — baseline shifted within session' if drifted else 'no significant drift detected'}")
        if step_result:
            print(f"  step change: max_rel_jump={step_result['max_rel_jump']:.3f} "
                  f"at_index={step_result['at_index']} "
                  f"flag={'YES' if step_result['flag'] else 'no'}")
    else:
        print("  (need 'honest' mode with ≥3 cycles for drift analysis)")

    # -----------------------------------------------------------------------
    # Write CSV
    # -----------------------------------------------------------------------
    os.makedirs("results", exist_ok=True)
    csv_path = f"results/session_{session_id}.csv"
    csv_fields = ["session_id", "cycle", "seq", "t_rel_s", "mode",
                  "p50_us", "p90_us", "p99_us", "errors", "locked"]
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=csv_fields)
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nwrote CSV: {csv_path}")

    # -----------------------------------------------------------------------
    # Write aggregate JSON
    # -----------------------------------------------------------------------
    non_portability_note = (
        "Absolute latencies are NOT comparable across sessions "
        "(Apple Silicon P/E-core placement is fixed per VM session; "
        "~1.6x between-session swing observed). "
        "Use within-session ratios/differences only."
    )

    out_doc = {
        "schema": "memchal.trials.v2",
        "session_id": session_id,
        "absolute_latencies_portable_across_sessions": False,
        "note": non_portability_note,
        "env": env,
        "label": args.label,
        "config": {
            "repeats": args.repeats,
            "n": n_per_run,
            "k": args.k,
            "lanes": args.lanes,
            "mb": args.mb,
            "warmup": args.warmup,
            "shuffle_seed": shuffle_seed,
            "gen": args.gen, "chain": args.chain, "nonce": args.nonce, "gap_us": args.gap_us,
        },
        "executed_orders": executed_orders,
        "modes": {
            md: {
                "agg_p50": aggs[md].__dict__,
                "p50_by_cycle": mode_p50[md],
            }
            for md in modes
        },
        "memory_time": memory_time_diff.__dict__ if memory_time_diff is not None else None,
        "ratios": {md: r_.__dict__ for md, r_ in ratios.items()},
        "drift": {
            "trend": drift_result,
            "step_change": step_result,
        },
    }

    with open(args.out, "w") as f:
        json.dump(out_doc, f, indent=2)
    print(f"wrote JSON: {args.out}")


if __name__ == "__main__":
    main()
