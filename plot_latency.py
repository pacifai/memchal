#!/usr/bin/env python3
"""Plot the latency profile written by the verifier (latency.csv).

Usage: python3 plot_latency.py [latency.csv] [out.png]
Requires matplotlib (pip install matplotlib). Produces a histogram + ECDF so
tail behaviour (the tell-tale of swapping / cache misses) is easy to read.
"""
import sys, csv

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "latency.csv"
    out  = sys.argv[2] if len(sys.argv) > 2 else "latency.png"
    xs = []
    with open(path) as f:
        for row in csv.DictReader(f):
            xs.append(float(row["latency_us"]))
    if not xs:
        print("no data"); return
    xs.sort()
    n = len(xs)
    def pct(p): return xs[min(n - 1, int(p / 100.0 * (n - 1)))]

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4))
    ax1.hist(xs, bins=80, color="#3b6ea5", edgecolor="white", linewidth=0.3)
    ax1.set_title("Challenge latency histogram")
    ax1.set_xlabel("latency (us)"); ax1.set_ylabel("count")
    for p, c in [(50, "#2a2"), (99, "#c60"), (99.9, "#a00")]:
        ax1.axvline(pct(p), color=c, ls="--", lw=1, label=f"p{p}={pct(p):.0f}us")
    ax1.legend(fontsize=8)

    ecdf = [(i + 1) / n for i in range(n)]
    ax2.plot(xs, ecdf, color="#3b6ea5")
    ax2.set_title("Latency ECDF"); ax2.set_xlabel("latency (us)"); ax2.set_ylabel("cumulative")
    ax2.set_ylim(0, 1); ax2.grid(alpha=0.3)

    fig.tight_layout(); fig.savefig(out, dpi=130)
    print(f"wrote {out}  (min={xs[0]:.1f} p50={pct(50):.1f} p99={pct(99):.1f} "
          f"p99.9={pct(99.9):.1f} max={xs[-1]:.1f} us)")

if __name__ == "__main__":
    main()
