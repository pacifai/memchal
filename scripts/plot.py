#!/usr/bin/env python3
"""Produce a self-describing chart from a memchal.trials.v2 JSON file.

Usage:
    python3 scripts/plot.py [results/trials.json]

Reads the trials JSON produced by scripts/run_trials.py and writes a PNG to
results/trials_<session_id>.png.  Also prints the output path and the full
caption text to stdout.

Imports mc_stats to reuse Agg/Ratio dataclasses — does NOT re-implement any
aggregation or percentile math.
"""
from __future__ import annotations
import json, os, sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Locate mc_stats relative to this file (scripts/mc_stats.py)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mc_stats as ms


def _load(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def _agg_from_dict(d: dict) -> ms.Agg:
    return ms.Agg(n=d["n"], mean=d["mean"], sd=d["sd"],
                  sem=d["sem"], lo=d["lo"], hi=d["hi"])


def _ratio_from_dict(d: dict) -> ms.Ratio:
    return ms.Ratio(value=d["value"], rel_error=d["rel_error"], error=d["error"])


def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else "results/trials.json"
    doc = _load(path)

    schema = doc.get("schema", "")
    if schema != "memchal.trials.v2":
        print(f"WARNING: unexpected schema '{schema}', expected 'memchal.trials.v2'",
              file=sys.stderr)

    session_id = doc["session_id"]
    env        = doc.get("env", {})
    label      = doc.get("label", "")
    cfg        = doc.get("config", {})
    modes_doc  = doc.get("modes", {})
    ratios_doc = doc.get("ratios", {})

    # Config values
    repeats = cfg.get("repeats", "?")
    n       = cfg.get("n", "?")
    k       = cfg.get("k", "?")
    lanes   = cfg.get("lanes", "?")
    mb      = cfg.get("mb", "?")
    warmup  = cfg.get("warmup", "?")

    # Environment
    in_container = env.get("in_container", False)
    machine      = env.get("machine", "unknown")
    cpus         = env.get("cpus", env.get("online_cpus", "?"))
    page_size    = env.get("page_size", "?")
    env_desc     = ("container" if in_container else "host") + f"/{machine}"

    # Ordered modes as they appear in the JSON
    mode_names = list(modes_doc.keys())

    # Reconstruct Agg objects
    aggs: dict[str, ms.Agg] = {
        md: _agg_from_dict(modes_doc[md]["agg_p50"])
        for md in mode_names
    }

    # Reconstruct Ratio objects
    ratios: dict[str, ms.Ratio] = {
        md: _ratio_from_dict(ratios_doc[md])
        for md in ratios_doc
    }

    # Transport floor reference line (may not be present)
    floor_mean = aggs["floor"].mean if "floor" in aggs else None

    # -------------------------------------------------------------------------
    # Build plot data — exclude "floor" from the bar chart bars (it's a ref line)
    # -------------------------------------------------------------------------
    bar_modes = [md for md in mode_names if md != "floor"]

    means  = np.array([aggs[md].mean for md in bar_modes])
    sds    = np.array([aggs[md].sd   for md in bar_modes])

    # Colour palette: honest = blue-ish, cheating modes = orange/red, floor (line) = grey
    _palette = {
        "honest":    "#3b6ea5",
        "disk":      "#e07b30",
        "mmapdisk":  "#c44e52",
        "fake":      "#8e44ad",
        "partial":   "#d4ac0d",
    }
    colours = [_palette.get(md, "#777777") for md in bar_modes]

    x = np.arange(len(bar_modes))

    # -------------------------------------------------------------------------
    # Caption / title text (built before the figure so we can print it first)
    # -------------------------------------------------------------------------
    caveat = (
        "Absolute latencies are within-session illustrative only; "
        "NOT comparable across sessions "
        "(Apple Silicon P/E-core placement, ~1.6x swing)."
    )

    caption_lines = [
        f"Memory-challenge PoC — p50 latency by mode (µs, log scale)",
        f"session: {session_id}   env: {env_desc}   cpus: {cpus}   page_size: {page_size} B",
        f"n={n} samples/run   repeats={repeats} cycles   k={k}   lanes={lanes}   mb={mb} MiB",
        f"Error bars = run-to-run sd across {repeats} interleaved cycles.",
        caveat,
    ]
    caption_text = "\n".join(caption_lines)

    # -------------------------------------------------------------------------
    # Figure
    # -------------------------------------------------------------------------
    fig, ax = plt.subplots(figsize=(max(6, len(bar_modes) * 1.6 + 2), 5.5))
    fig.subplots_adjust(bottom=0.30)   # room for sub-caption

    bars = ax.bar(x, means, yerr=sds, capsize=5, color=colours, edgecolor="white",
                  linewidth=0.5, error_kw={"elinewidth": 1.2, "ecolor": "#333"})

    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels(bar_modes, fontsize=11)
    ax.set_ylabel("p50 latency (µs)  [log scale]", fontsize=10)
    ax.yaxis.grid(True, which="both", linestyle="--", linewidth=0.5, alpha=0.6)
    ax.set_axisbelow(True)

    # Transport floor reference line
    if floor_mean is not None:
        ax.axhline(floor_mean, color="#555555", linestyle=":", linewidth=1.5,
                   label=f"transport floor (k=0): {floor_mean:.1f} µs")
        ax.legend(fontsize=8, loc="upper left")

    # Annotate cheating bars with ratio ± error
    for i, md in enumerate(bar_modes):
        if md in ratios:
            r = ratios[md]
            label_txt = f"{r.value:.2f}x ± {r.error:.2f}"
            bar_top = means[i] + sds[i]
            ax.text(i, bar_top * 1.08, label_txt,
                    ha="center", va="bottom", fontsize=8, color="#333333")

    # Value labels on bars
    for i, (md, v) in enumerate(zip(bar_modes, means)):
        ax.text(i, v * 0.85, f"{v:.1f}", ha="center", va="top",
                fontsize=8, color="white", fontweight="bold")

    # Title (first caption line)
    ax.set_title(caption_lines[0], fontsize=11, pad=8)

    # Sub-caption: env + caveat lines below the axes
    subcap = "\n".join(caption_lines[1:])
    fig.text(0.5, 0.01, subcap, ha="center", va="bottom",
             fontsize=7.5, color="#444444",
             wrap=True, transform=fig.transFigure)

    # -------------------------------------------------------------------------
    # Save
    # -------------------------------------------------------------------------
    os.makedirs("results", exist_ok=True)
    out_path = f"results/trials_{session_id}.png"
    fig.savefig(out_path, dpi=130, bbox_inches="tight")
    plt.close(fig)

    print(f"Wrote: {out_path}")
    print()
    print("=== CAPTION ===")
    print(caption_text)


if __name__ == "__main__":
    main()
