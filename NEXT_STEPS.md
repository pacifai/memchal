# Next steps

Open work, most important first. When an item is done, delete it here and
record the result in the README or a decision record.

- [ ] **Investigate the 5% resolution.** A GPU prover that keeps about 5% of
  the buffer in host memory and the rest on the card answers about as fast as
  an honest one, so the timing check reads it as honest. Open questions: how
  much of that blind spot comes from the measurement setup and how much from
  the method itself, why full occupancy of the card is hard to account for,
  how much memory an adversary can realistically keep elsewhere for a given
  card, buffer size, and number of probes, and what it can do with the memory
  it frees. A first step could be to measure the blind spot while changing one
  variable at a time.

- [ ] **Reduce the cost of the tree fold.** The tree fold combines the per-lane
  hashes on the GPU, one kernel launch per level of the tree. Each launch has a
  fixed cost of a few microseconds, so at high lane counts most of the fold's
  time goes to starting kernels. The fold is a fixed cost
  in every response, so shrinking it gives a cheater's extra time less to hide
  behind. One option is to combine the first several levels into one kernel.

- [ ] **Build the buffer on the GPU.** The GPU provers build the Argon2d buffer
  on the host CPU and copy it to the card, so the build uses host memory and
  CPU time. A slow build is acceptable, because it happens
  once at startup. Choose the number of Argon2d lanes by how quickly a cheater
  could rebuild a discarded block during a challenge, and accept the build time
  that results. Once the card builds the buffer, the mode where the verifier
  uploads the whole buffer can be removed.

- [ ] **Test that two Argon2d passes defeat partial recompute.** In one H100
  run, a prover that discarded blocks and rebuilt them on demand answered wrong
  on every challenge when Argon2d used two passes. That one run is the only
  evidence. Add an automated test, and record the chosen Argon2d memory, pass, and lane settings
  in ADR 0007.

- [ ] **Try a smarter recompute adversary.** The current adversary discards
  blocks at random. A real one could work out which blocks are cheapest to
  rebuild and discard those. Check whether that changes the result.

- [ ] **Measure an adversary that stores the buffer compressed.** Argon2d
  output is random and incompressible, but model weights often compress. If the
  buffer is ever derived from weights, a prover could keep it compressed and
  decompress only what each challenge reads. Measure how much memory that saves
  and whether the timing check notices.

- [ ] **Consider offloading to a neighboring GPU.** On a server with several
  GPUs, a prover could keep part of the buffer in another GPU's memory, reached
  over the direct GPU-to-GPU link (NVLink). That is much closer to local speed
  than host memory, and it is outside the current threat model. Decide whether
  it needs an experiment, a decision record, or both.

- [ ] **Add a decision rule.** The harness reports how far apart honest and
  cheating provers are. Add a rule that turns a response into an accept or
  reject verdict with known false-positive and false-negative rates.

- [ ] **Check a random subset of lanes.** The verifier recomputes every probe,
  which costs as much as answering, and the answer is a single 64-bit value.
  If the prover returned the tree fold's root plus a few lane hashes for the
  verifier to check, verification would be cheaper and the answer harder to
  guess.

- [ ] **Tie the buffer to the model weights.** Today the prover shows it holds
  random data from the verifier. The goal is to show it holds data tied to the
  actual workload, while model providers keep working as they do today.

- [ ] **Run on a bare-metal CPU with a buffer larger than RAM.** The CPU
  numbers come from Docker on a Mac, which virtualizes storage. This is
  optional, since the GPU is where the timing check matters.

- [ ] **Make the wire byte order explicit.** The protocol sends integers in the
  machine's native byte order and assumes both ends use the same one.
