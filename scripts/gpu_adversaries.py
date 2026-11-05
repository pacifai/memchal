#!/usr/bin/env python3
"""Read the verifier JSON that gpu_adversaries.sh collects.

  p50 FILE      print "<p50 us> <errors>" for one run
  summary DIR   print the per-mode table for a whole session
"""
import glob
import json
import os
import sys

import mc_stats as ms


def p50(path):
    d = json.load(open(path))["result"]
    print(d["percentiles_us"]["p50"]["value"], d["errors"])


def summary(out):
    by = {}
    for f in sorted(glob.glob(os.path.join(out, "*-c*.json"))):
        mode = os.path.basename(f).rsplit("-c", 1)[0]
        try:
            d = json.load(open(f))
        except (OSError, ValueError):
            continue
        r = d.get("result") or {}
        v = ((r.get("percentiles_us") or {}).get("p50") or {}).get("value")
        if v is None:
            continue
        by.setdefault(mode, {"p50": [], "err": 0})
        by[mode]["p50"].append(v)
        by[mode]["err"] += int(r.get("errors") or 0)

    base = ms.aggregate(by["honest"]["p50"]) if "honest" in by else None
    print(f"{'mode':<10} {'p50 us (mean ± sd)':>22} {'x honest':>16} {'errors':>8}")
    for m in ("honest", "fake", "uvm", "partial"):
        if m not in by:
            print(f"{m:<10} {'did not run':>22}")
            continue
        agg = ms.aggregate(by[m]["p50"])
        r = str(ms.ratio(agg, base)) if base else "-"
        print(f"{m:<10} {f'{agg.mean:.1f} ± {agg.sd:.1f}':>22} {r:>16} {by[m]['err']:>8}")
    print()
    print("fake is meant to be FAST and WRONG: judge it by the error column, not p50.")
    print("uvm with HBM taken away, and partial, are meant to be SLOW and RIGHT.")


if __name__ == "__main__":
    {"p50": p50, "summary": summary}[sys.argv[1]](*sys.argv[2:])
