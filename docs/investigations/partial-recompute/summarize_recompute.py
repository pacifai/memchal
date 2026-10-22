#!/usr/bin/env python3
"""Summarize one gpu_recompute.sh session: for each pass count, the honest
spread, and each kept fraction's p50, wrong answers, and rebuild work against it.

Usage: summarize_recompute.py RESULTS_DIR
"""
import glob, json, os, statistics, sys
out = sys.argv[1]

def result(path):
    try:
        r = json.load(open(path))["result"]
        return r["percentiles_us"]["p50"]["value"], r["errors"], r.get("n")
    except Exception:
        return None, None, None

def prover_line(path, kind):
    try:
        for line in open(path):
            if line.startswith("@") and f'"t":"{kind}"' in line:
                return json.loads(line[1:])
    except Exception:
        pass
    return {}

passes = sorted({os.path.basename(f).split("-")[1] for f in glob.glob(os.path.join(out, "keep-t*.json"))})
for t in passes:
    base = [result(f)[0] for f in sorted(glob.glob(os.path.join(out, f"honest-{t}-*.json")))]
    base = [b for b in base if b]
    if not base:
        print(f"{t}: no honest baseline — nothing to compare against\n")
        continue
    bmed = statistics.median(base)
    bsd = statistics.pstdev(base) if len(base) > 1 else 0.0
    thresh = bmed + 3 * bsd
    print(f"{t}: honest p50 median {bmed:.1f} us, sd {bsd:.1f} us over {len(base)} baselines; "
          f"separation threshold (median + 3sd) {thresh:.1f} us")
    print(f"{'keep':>7} {'HBM saved':>9} {'p50 us':>11} {'x honest':>9} {'errors':>7} "
          f"{'rebuilt/ch':>10} {'rounds/ch':>9} {'rebuild ms/ch':>13} {'gave up':>7}  separated?")
    rows = []
    for f in glob.glob(os.path.join(out, f"keep-{t}-*.json")):
        keep = os.path.basename(f)[len(f"keep-{t}-"):-5]
        tag = os.path.basename(f)[:-5]
        p50, err, n = result(f)
        if p50 is None:
            continue
        info = prover_line(os.path.join(out, f"p-{tag}.log"), "cheatinfo")
        summ = prover_line(os.path.join(out, f"p-{tag}.log"), "rc_summary")
        saved = 1 - info["kept_blocks"] / info["blocks"] if info.get("blocks") else float("nan")
        rows.append((float(keep), saved, p50, err, summ))
    for keep, saved, p50, err, summ in sorted(rows, reverse=True):
        sep = "yes" if p50 > thresh else "no"
        print(f"{keep:>7.3f} {saved*100:>8.2f}% {p50:>11.1f} {p50/bmed:>8.2f}x {err:>7} "
              f"{summ.get('rebuilt_per_ch', float('nan')):>10.1f} "
              f"{summ.get('rounds_per_ch', float('nan')):>9.1f} "
              f"{summ.get('rebuild_ms_per_ch', float('nan')):>13.3f} "
              f"{summ.get('gave_up_challenges', '?'):>7}  {sep}")
    print()
print("Errors are the verifier's count of wrong answers. A cheat that ran out of")
print("rebuild budget answers wrong, so a nonzero count there means it gave up.")
print("Only ratios within this session are quotable; absolute microseconds are not.")
