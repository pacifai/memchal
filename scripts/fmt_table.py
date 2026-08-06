#!/usr/bin/env python3
"""fmt_table.py — aligned box tables/blocks for the experiment scripts.

The experiment output is read off a projector, so alignment is not cosmetic.
Widths are computed from the content (never hand-counted), so the borders always
close. Content is ASCII-only; the frame is box-drawing.

Usage from Python:
    from fmt_table import table, block
    print(table([("scenario", "l"), ("p50 (us)", "r")], rows, title="RESULTS"))
    print(block("VERDICT", ["line one", "line two"]))

Usage as a filter (JSON on stdin):
    {"title": "...", "columns": [["scenario","l"], ["p50","r"]],
     "rows": [["honest","64.5"]], "notes": ["..."]}
"""
import json
import sys

PAD = 2  # spaces of padding either side of every cell

# Content is folded to ASCII before it is measured. Box alignment depends on one
# char == one terminal column, and typographic characters (em dash, ellipsis,
# curly quotes) are ambiguous-width in several terminal fonts — on a projector
# that shows up as a frame that doesn't close.
_FOLD = {"—": "--", "–": "-", "…": "...", "’": "'", "‘": "'", "“": '"', "”": '"',
         "×": "x", "≈": "~", "≥": ">=", "≤": "<=", "→": "->", "µ": "u"}


def _ascii(s):
    s = "".join(_FOLD.get(c, c) for c in str(s))
    return s.encode("ascii", "replace").decode("ascii")


def _frame(widths, left, mid, right, fill="═"):
    return left + mid.join(fill * (w + 2 * PAD) for w in widths) + right


def _row(cells, widths, aligns):
    out = []
    for cell, w, a in zip(cells, widths, aligns):
        body = cell.rjust(w) if a == "r" else cell.ljust(w)
        out.append(" " * PAD + body + " " * PAD)
    return "║" + "║".join(out) + "║"


def table(columns, rows, title=None, notes=()):
    """columns: [(header, align)] with align 'l' or 'r'; rows: [[str, ...]]."""
    headers = [_ascii(c[0]) for c in columns]
    aligns = [c[1] if len(c) > 1 else "l" for c in columns]
    rows = [[("" if c is None else _ascii(c)) for c in r] for r in rows]
    title = _ascii(title) if title else title
    notes = [_ascii(n) for n in notes]

    widths = [len(h) for h in headers]
    for r in rows:
        for i, cell in enumerate(r):
            widths[i] = max(widths[i], len(cell))

    inner = sum(w + 2 * PAD for w in widths) + (len(widths) - 1)  # + column seams

    lines = []
    if title:
        # A title spanning all columns needs the table at least as wide as it is.
        if len(title) + 2 * PAD > inner:
            widths[-1] += len(title) + 2 * PAD - inner
            inner = len(title) + 2 * PAD
        lines.append("╔" + "═" * inner + "╗")
        lines.append("║" + title.center(inner) + "║")
        lines.append(_frame(widths, "╠", "╦", "╣"))
    else:
        lines.append(_frame(widths, "╔", "╦", "╗"))
    lines.append(_row(headers, widths, ["l"] * len(widths)))
    lines.append(_frame(widths, "╟", "┼", "╢", "─"))
    for r in rows:
        lines.append(_row(r, widths, aligns))
    lines.append(_frame(widths, "╚", "╩", "╝"))
    lines += ["  " + n for n in notes]
    return "\n".join(lines)


def block(title, lines):
    """A boxed block of prose lines (e.g. a VERDICT), padded to a common width."""
    body = [_ascii(l) for l in lines]
    title = _ascii(title)
    inner = max([len(title)] + [len(l) for l in body]) + 2 * PAD
    out = ["╔" + "═" * inner + "╗",
           "║" + title.center(inner) + "║",
           "╠" + "═" * inner + "╣"]
    for l in body:
        out.append("║" + " " * PAD + l.ljust(inner - 2 * PAD) + " " * PAD + "║")
    out.append("╚" + "═" * inner + "╝")
    return "\n".join(out)


def main():
    spec = json.load(sys.stdin)
    print(table([tuple(c) for c in spec["columns"]], spec["rows"],
                spec.get("title"), spec.get("notes", [])))


if __name__ == "__main__":
    main()
