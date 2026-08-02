#!/usr/bin/env python3
"""Overlay latency ECDFs for the prover modes.

Usage: python3 plot_compare.py [out.png]
Reads latency_honest.csv / latency_disk.csv / latency_partial.csv /
latency_fake.csv (whichever exist), produced by compare_modes.sh.
Requires matplotlib.
"""
import sys, csv, os

MODES = [
    ("honest",  "#2a8f2a", "honest (RAM, pinned)"),
    ("disk",    "#a00000", "disk (all probes -> disk)"),
    ("partial", "#c07000", "partial (50% RAM / 50% disk)"),
    ("fake",    "#3b6ea5", "fake (misses fabricated)"),
]

def load(mode):
    path = f"latency_{mode}.csv"
    if not os.path.exists(path):
        return None
    xs = []
    with open(path) as f:
        for row in csv.DictReader(f):
            xs.append(float(row["latency_us"]))
    xs.sort()
    return xs

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "latency_compare.png"
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(8, 5))
    any_data = False
    for mode, color, label in MODES:
        xs = load(mode)
        if not xs:
            continue
        any_data = True
        n = len(xs)
        ecdf = [(i + 1) / n for i in range(n)]
        p50 = xs[n // 2]
        ax.plot(xs, ecdf, color=color, lw=1.8, label=f"{label}  (p50={p50:.0f}us)")
    if not any_data:
        print("no latency_*.csv files found — run compare_modes.sh first")
        return
    ax.set_xscale("log")
    ax.set_xlabel("challenge latency (us, log scale)")
    ax.set_ylabel("cumulative fraction")
    ax.set_title("Latency profile by prover honesty")
    ax.set_ylim(0, 1)
    ax.grid(alpha=0.3, which="both")
    ax.legend(fontsize=8, loc="lower right")
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    print(f"wrote {out}")

if __name__ == "__main__":
    main()
