#!/usr/bin/env python3
"""Summarize one gpu_resolution.sh session: the honest spread, each offload
point's p50 against it, and the smallest offload that cleared it.

Usage: summarize_resolution.py RESULTS_DIR
"""
import json, sys, glob, os, statistics
out = sys.argv[1]

def p50(path):
    try:
        r = json.load(open(path))["result"]
        return r["percentiles_us"]["p50"]["value"], r["errors"]
    except Exception:
        return None, None

base = [p50(f)[0] for f in sorted(glob.glob(os.path.join(out, "honest-*.json")))]
base = [b for b in base if b]
if not base:
    print("no honest baseline — nothing to compare against")
    raise SystemExit(0)

bmed = statistics.median(base)
bsd = statistics.pstdev(base) if len(base) > 1 else 0.0
# A point counts as separated only if it clears the honest spread, not just the
# honest median. With clocks unlockable that spread is the real noise floor.
thresh = bmed + 3 * bsd
print(f"honest p50 median {bmed:.1f} us, spread (sd) {bsd:.1f} us over {len(base)} baselines")
print(f"separation threshold (median + 3sd): {thresh:.1f} us\n")
print(f"{'offload':>9} {'p50 us':>10} {'x honest':>10} {'errors':>8}  separated?")

rows = []
for f in sorted(glob.glob(os.path.join(out, "off-*.json"))):
    frac = os.path.basename(f)[4:-5]
    v, err = p50(f)
    if v is None:
        continue
    rows.append((float(frac), v, err))
rows.sort()
smallest = None
for frac, v, err in rows:
    sep = v > thresh
    if sep and smallest is None and frac > 0:
        smallest = frac
    print(f"{frac:>9.3f} {v:>10.1f} {v/bmed:>9.2f}x {err:>8}  {'yes' if sep else 'no'}")
print()
if smallest is not None:
    print(f"Smallest offload fraction this detector separated: {smallest:.3f} "
          f"({smallest*100:.1f}% of the buffer off the card).")
    print("That is the detector's resolution at this geometry, on this card, in")
    print("this session. It is not a portable number.")
else:
    print("No offload fraction in this sweep cleared the honest spread. Either the")
    print("detector's resolution is coarser than the largest fraction tried, or the")
    print("noise floor is too wide at this geometry. Report it as measured.")
