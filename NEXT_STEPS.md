# Next steps

Open work, most important first. Each item says what is open and why it
matters. When an item is done, delete it here and record the result in the
README or a decision record.

- [ ] **Find out why small amounts of off-chip data go unnoticed.** A GPU prover
  that keeps about 5% of the buffer in the server's main memory answers about
  as fast as an honest one, so the timing check passes it. That 5% is on-chip
  memory a cheater gets to use for something else. Open questions: how much of
  this gap comes from noise in the measurement setup and how much from the
  method itself, why filling a whole card is hard to account for, how much data
  a cheater could realistically keep elsewhere for a given card, buffer size,
  and number of reads, and what the freed memory is worth to it. A first step
  could be to measure the gap while changing one variable at a time.

- [ ] **Make combining the answer cheaper.** A challenge splits its reads into
  parallel chains, and the GPU combines the chains' results into one answer in
  rounds. Each round starts a small GPU program, which costs a few
  microseconds, so with many chains most of the combining time goes to starting
  programs. Every answer pays this fixed time, honest or not, and a cheater's
  extra delay is easier to see when the fixed part is small. One option is to
  run the first several rounds in one program.

- [ ] **Build the buffer on the GPU.** The GPU provers build the buffer on the
  server's CPU and copy it to the card, so setting up needs main memory and CPU
  time. A slow build is acceptable, because it happens once at startup. The
  open question is how many parallel parts the build should have: choose by how
  quickly a cheater could rebuild a discarded piece during a challenge, and
  accept the build time that results. Once the card builds the buffer, the mode
  where the verifier uploads the whole buffer can be removed.

- [ ] **Test that a cheater can't rebuild discarded data in time.** The buffer
  is built with Argon2d so that a prover that throws part of it away can't
  regenerate that part quickly. In one H100 run, a prover that discarded pieces
  and rebuilt them on demand answered every challenge wrong when Argon2d made
  two passes over the memory. That one run is the only evidence. Add an
  automated test, and record the chosen Argon2d settings in ADR 0007.

- [ ] **Try a smarter rebuilding cheater.** The current one discards pieces of
  the buffer at random. A real cheater could work out which pieces are cheapest
  to rebuild and discard those. The open question is whether it still fails.

- [ ] **Measure a cheater that stores the buffer compressed.** The buffer is
  random today, so it can't be compressed. Model weights often can. If the
  buffer is ever derived from weights, a prover could keep it compressed and
  decompress only what each challenge reads. Measure how much memory that saves
  and whether the timing check notices.

- [ ] **Decide how to handle a neighboring GPU.** On a server with several
  GPUs, a prover could keep part of the buffer in another GPU's memory, which
  it reaches over a direct GPU-to-GPU link. That is much closer to on-chip
  speed than the server's main memory, and no test covers it. Decide whether it
  needs an experiment, a decision record, or both.

- [ ] **Add a pass or fail rule.** memchal reports how far apart honest and
  cheating provers are. A verifier needs a rule that turns an answer into pass
  or fail, with known rates of failing an honest prover and passing a cheater.

- [ ] **Make checking cheaper than answering.** The verifier redoes every read
  to check an answer, which costs as much as answering, and the answer is one
  64-bit value. If the prover also returned a few chains' results, the verifier
  could check only those. Checking would be cheaper and the answer harder to
  guess.

- [ ] **Tie the buffer to the model weights.** Today the prover shows that it
  holds random data from the verifier. The goal is to show that it holds data
  tied to the workload it declared, while model providers keep working as they
  do today.

- [ ] **Run the CPU version on a real machine with a buffer larger than RAM.**
  The CPU numbers come from Docker on a Mac, where storage is virtualized. This
  is optional, because the GPU is where the timing check matters.

- [ ] **Fix the byte order of messages.** The verifier and the prover send
  numbers in their machine's own byte order and assume the other end uses the
  same one. Two different kinds of machine would misread each other.
