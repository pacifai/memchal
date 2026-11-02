"""Every rendered report table must close its frame and keep its content ASCII."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from fmt_table import table, block

cases = [
    table([("scenario", "l"), ("p50 (us)", "r")],
          [["honest — mlock'd", "42.2"], ["mmapdisk ≈ honest", "1,451.0"]],
          title="A TITLE LONGER THAN THE COLUMNS PUT TOGETHER",
          notes=["a note — with an em dash"]),
    table([("a", "l"), ("b", "r")], [["x", "1"]]),
    block("VERDICT — SHORT TITLE", ["a line", "a much longer line — with a dash", ""]),
]
FRAME = set("═║╔╗╚╝╠╣╦╩╬╟╢┼─")
bad = 0
for i, rendered in enumerate(cases):
    lines = [l for l in rendered.split("\n") if l[:1] in FRAME]
    widths = {len(l) for l in lines}
    if len(widths) != 1:
        bad += 1
        print(f"  case {i}: ragged frame, line widths {sorted(widths)}")
    # Content must be folded to ASCII; only the frame itself may be non-ASCII,
    # since ambiguous-width glyphs are what break alignment on other terminals.
    stray = {c for l in lines for c in l if ord(c) > 127 and c not in FRAME}
    if stray:
        bad += 1
        print(f"  case {i}: non-ASCII content survived folding: {sorted(stray)}")
print("  all frames closed, content is ASCII" if not bad else f"  {bad} case(s) bad")
sys.exit(1 if bad else 0)
