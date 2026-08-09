# Original prototype results (preserved)

The untouched **single-run** outputs that shipped with the original prototype,
moved here verbatim (they used to sit at the repo root). Kept for provenance and
comparison — they are the artifacts behind the *old* README's "Measured results"
tables.

Data:
- `latency.csv`, `latency.png` — one honest run's per-challenge latencies + plot.
- `latency_{honest,disk,partial,fake}.csv`, `latency_compare.png` — the old
  four-mode contrast (n=300 each, single run).

Scripts that produced them (superseded, kept for provenance):
- `plot_latency.py` — histogram + ECDF for one run → superseded by `scripts/plot.py`.
- `plot_compare.py` — ECDF overlay across modes → superseded by `scripts/plot.py`.
- `compare_modes.sh` — the awk/sed-parsing mode harness → superseded by
  `scripts/run_trials.py` (and it relied on `setsid`/`timeout`, absent on macOS).

**Do not trust these as evidence.** They are single runs with no repeats, no error
bars, and absolute microseconds that are not portable across sessions; the "disk is
~10–19× slower = RAM-vs-storage" reading they were used for was later shown to be
`pread` syscall overhead. See:
- [ADR 0003 — timing detector scope & measurement](../decisions/0003-timing-detector-scope-and-measurement.md)
- [investigations/](../investigations/) for the measurements that superseded these.

Regenerate trustworthy results with the stabilized harness:
`./run.sh bash -c 'make && python3 scripts/run_trials.py ... && python3 scripts/plot.py results/trials.json'`.
