# Investigation: how much of the absolute latency is the environment?

## Question
Early on, "honest" measured ~61 µs in one container run and ~98 µs in another —
same binary, same flags. Is that run-to-run noise, a code/harness difference, or
the environment? And is a single long session internally stable, or does it drift
mid-run?

## Method
`run_variance.sh` (runs on the **host** — it launches several containers):
- **Part 1:** one long session, honest 30× back-to-back; check for a mid-session
  step change (first-third vs last-third mean).
- **Part 2:** N independent container sessions, honest baseline mean per session;
  measure the between-session spread.

(Ruled out earlier, in scratch: it is *not* prover-vs-cheat code, *not* persistent-
vs-restart server lifecycle, *not* the Python-vs-shell harness — all matched
within a session.)

## Finding
- **Within a session the baseline is stable** — no mid-run step change (placement
  is fixed at session start; the VM's vCPUs don't migrate P↔E mid-run). Example
  capture: 30 runs, mean 60.8 µs, sd 1.6 µs (cv 2.6%), first-third 60.5 vs
  last-third 61.6.
- **Between sessions the absolute level can swing ~1.6×** — the VM's vCPUs land on
  Apple-Silicon **performance vs efficiency cores**, fixed per session, and macOS
  gives no affinity control. The ~1.6× matches the P/E core performance ratio.
- **But the swing is load-dependent, not constant.** The committed capture
  ([`results/run_variance.txt`](results/run_variance.txt), 2026-08-09) shows only
  **1.05×** — all 6 sessions landed in the fast (~49 µs) regime. The large swing
  was seen earlier (2026-08-02: ~61 µs vs ~98 µs sessions) on a busier host. So
  you *cannot rely* on cross-session absolute comparability, even though it
  sometimes happens to hold.

## Consequence (the load-bearing one)
**Only within-session ratios and paired differences are trustworthy.** The harness
therefore measures every mode in ONE session, interleaved in randomized order, and
reports ratios / `honest − floor` with propagated error — never portable absolute
µs. Every CSV/plot is stamped with its session id and this caveat. See
[ADR 0003](../../decisions/0003-timing-detector-scope-and-measurement.md).

Related open item: a bare-metal Linux rerun with `taskset` (pinned affinity) would
remove this entirely and give portable absolute numbers — see NEXT_STEPS.md.

## Reproduce
```bash
bash docs/investigations/session-variance/run_variance.sh   # from repo root; runs on host
```
