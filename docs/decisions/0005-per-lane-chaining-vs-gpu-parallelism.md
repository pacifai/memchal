# ADR 0005: Each challenge is many short chains of dependent reads

**Status:** accepted

## Context

The defense against prefetching ([ADR 0004](0004-adversary-taxonomy-and-defenses.md))
makes each read's place depend on the word the previous read returned. The
reads can't be predicted, so they can't be fetched early.

But dependent reads can't run in parallel: read `i+1` waits for read `i`. A GPU
is fast because it runs tens of thousands of threads at once. Full dependence
and full parallelism pull in opposite directions.

## Decision

Split each challenge into many lanes. Within a lane, reads form a dependent
chain. Lanes are independent of each other, so they run in parallel, one GPU
thread each.

A chain's length is `samples / lanes`. The verifier refuses challenges where
that's below 16, so a dependent challenge can't quietly shrink to chains of one
read.

## Alternatives considered

| Shape | Resists prefetching | Parallel | Verdict |
|---|---|---|---|
| One chain through every read | Fully | Not at all | Rejected: on a GPU it would use one thread |
| Independent reads (`--chain independent`) | Not at all | Fully | Kept only as the weak setting for measurements |
| Many lanes, each a chain (chosen) | Up to the chain length | Across lanes | The balance between the two |

## Consequences

- **Resistance grows with chain length, not with the number of reads.** Each
  lane's first place comes from the nonce alone, so it's predictable. A cheat
  can keep one read in flight per lane, so a challenge costs it about one
  chain's worth of slow reads in a row. Longer chains mean fewer lanes, and
  less parallel work for the honest prover.
- **The honest prover pays too.** Dependent reads stop the hardware from
  fetching the next place early, which cost the honest CPU prover about 3 to 4
  times in throughput.
- **On a GPU, the number of reads grows with the lane count.** The GPU runs use
  16,384 lanes and 262,144 reads, which makes each chain 16 reads long.
- **It doesn't stop regeneration.** Chains stop a cheat from predicting where
  the challenge reads. They do nothing against a cheat that rebuilds the data
  it discarded, which [ADR 0007](0007-buffer-generator-memory-hard-function.md)
  covers.
