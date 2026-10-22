# Investigations

Self-contained analyses that answered a technical question during stabilization.
Each folder has a `README.md` (question → method → finding → reproduce), the
script that produced it, and a `results/` snapshot. These are supporting analysis,
not part of the core project — they explain why the harness and the claims are what
they are.

Absolute microseconds in any result are **within-session illustrative only** and
not comparable across sessions (see [decisions/0003](../decisions/0003-timing-detector-scope-and-measurement.md)).

| Investigation | Question | Headline finding |
|---|---|---|
| [disk-is-syscalls](disk-is-syscalls/) | Is the "disk" gap storage distance or syscalls? | Syscall overhead; the mmap adversary looks honest because the page cache holds its file |
| [mmap-needs-ram](mmap-needs-ram/) | Does the mmap adversary survive without RAM to cache the file? | No — starved below the buffer it's ~840× honest; timing tracks residency, not device |
| [prefetch-needs-pattern](prefetch-needs-pattern/) | Is the prefetch adversary real, and does chaining stop it? | Real only under memory pressure (~4× win starved; a strawman cached); dependent access removes the edge |
| [latency-decomposition](latency-decomposition/) | Where does a challenge's time go? Why is `fake` faster? | ~45 µs transport floor + ~half ALU / ~half DRAM per probe; `fake` is genuinely cheaper (no memory reads) |
| [partial-recompute](partial-recompute/) | Does Argon2d stop a GPU prover that discards blocks and rebuilds them on demand? | Yes. At `t=1`, discarding 0.1% is 3.8x honest and 10% is 34x, all answers correct. At `t=2` any discard runs out of budget and answers wrong. **H100.** |
| [partial-residency-resolution](partial-residency-resolution/) | How little can a prover offload before the detector stops seeing it? | ~5% of the buffer. A gradient, not a cliff — which is what a decision rule needs. **H100.** |
| [tree-fold-payoff](tree-fold-payoff/) | Did moving the cross-lane fold onto the card pay off? | Yes: 8x the lanes cost the old fold 8.1x and the tree fold 1.24x. 8.9x faster response at 131k lanes. **H100.** |
| [uvm-needs-hbm](uvm-needs-hbm/) | Does the GPU's UVM cheat survive without HBM to hold the buffer? | No — indistinguishable from honest until free HBM drops below the buffer size, then ~4,600x. No early warning. **First GPU measurement.** |
| [session-variance](session-variance/) | How much of absolute latency is the environment? | Within-session stable; between-session P/E-core swing up to ~1.6× (load-dependent) → compare within-session only |

Run from the repo root via `./run.sh bash <path>` — except session-variance and
mmap-needs-ram, which launch their own containers, so run those as `bash <path>`.
