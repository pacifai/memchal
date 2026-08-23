# ADR 0007 — Buffer generator: a memory-hard function for the build-buffer design

**Status:** proposed (the construction and the decision fork are settled here; the
cost parameters and the vendor/dependency call are finalized alongside the CPU
prototype).

**Supersedes, in part:** [ADR 0006](0006-chacha20-vendored-vs-dependency.md) — that ADR
kept a vendored O(1) ChaCha20 for the *send-buffer* scope and named its own trigger to
revisit ("if the generator moves onto a real security boundary in the build-buffer
design"). This is that trigger firing. ADR 0006's decision stands for the scope it was
made in; for the build-buffer scope the crypto primitive is re-decided here.

## Context

Today the verifier generates the whole buffer and **uploads every byte** to the prover
(`Hello.buffer_bytes`, then the bytes on the wire). The prover never sees the seed. The
`recompute` defense (`--gen chacha`) works *only* because of that: without the key the
prover can't invert the generator, so its fabricated words are wrong and **correctness**
catches it ([ADR 0004](0004-adversary-taxonomy-and-defenses.md)).

That design can't reach a real workload. A model's weights are gigabytes to terabytes;
you can't ship them over the wire per challenge, and random filler proves nothing about
the workload. The `build-buffer` design ships a short **recipe** instead, and both sides
build the buffer locally — but now the prover **holds the recipe**. If building word `i`
from the recipe is cheap and O(1) (as ChaCha20-CTR is), then "hold the buffer" and
"regenerate on demand" cost the same, and retention is unprovable: `recompute` wins
outright, and correctness can't see it because the regenerated words are *correct*.

A **memory-hard function (MHF)** closes this. Making the buffer expensive to rebuild —
with a steep time-memory tradeoff — leaves storing it as the only cheap way to have the
bytes ready at answer time. This ADR chooses the MHF and records what it changes.

One framing point that governs everything below: memchal does not want the MHF's output
*digest*. It wants the large scratch memory the MHF fills along the way (scrypt's `V`
array, Argon2's block matrix) to **become the buffer**. "You can only have these bytes
ready if you built the structure and kept it" is the whole proof.

## What memchal needs from the generator

Three properties decide whether an MHF does the job. A warehouse analogy keeps them
concrete: the buffer is a warehouse of numbered crates; floor space is RAM/VRAM (rented,
what the cheat wants to avoid); labor is compute; the recipe is the blueprint. The
honest prover builds the warehouse once and keeps it standing. The cheat rents less
floor and rebuilds a crate on the spot when a challenge asks for it (the `recompute` /
`partial` adversary at buffer scale).

- **TMTO (time-memory tradeoff):** if the cheat rents half the floor, how much extra
  labor does answering cost? *Steep* ("far more than double") is good; *flat* is useless.
- **Partial recompute:** the specific cheat of keeping some crates and rebuilding the
  rest. How bad it is for the cheat depends almost entirely on the addressing dial below.
- **Side channels:** if someone watches *which* crates the build fetches, do they learn
  the blueprint's secret? This is the headline concern for password hashing — and, as
  argued below, largely moot for memchal.

Two memchal-specific facts also shape the choice:

- **Build-once vs rebuild-per-challenge.** The MHF's heavy cost lands once, at
  construction. The per-challenge answer stays cheap (k random reads from resident
  memory). We are not trying to make the honest build fast; we are trying to make the
  *attacker's on-demand rebuild during a timed challenge* intolerable. The steeper the
  per-block rebuild, the wider that gap. This is the same asymmetry the whole PoC relies
  on.
- **GPU parallelism tension.** On the real target the honest prover builds in VRAM and
  *wants* parallelism to build fast, but these functions are deliberately hard to speed
  up in parallel (that is how they deny a cheat cheap parallel rebuilds). The lane knob
  has to give the honest builder enough parallelism without handing the attacker a cheap
  shrink. The CPU prototype can shape this; the port must confirm it.

## The decision fork: data-dependent vs data-independent addressing

Everything turns on one choice: **when the build fetches an earlier crate to make the
next one, is the choice of which crate data-dependent or data-independent?**

- **Data-dependent:** "open the crate whose number is written *inside* the last crate you
  built." The fetch order depends on contents, which depend on the secret recipe.
- **Data-independent:** "open crate `f(i)` for a fixed public schedule `f`." The order is
  the same for everybody, knowable from the recipe's *shape* without its secret contents.

That one dial drives all three properties:

- **Partial recompute + TMTO.** With **data-dependent** addressing, rebuilding a crate
  first needs the crate it referenced — which the cheat may also not have — so a shortfall
  of floor space triggers a **cascade** of rebuilds. Steep tradeoff. With
  **data-independent** addressing the dependency graph is public and fixed, so a cheat
  studies it offline and keeps the *optimal* subset (pebbling); partial recompute gets
  much cheaper. This is not just easier in practice — it is a **proven ceiling**:
  data-independent MHFs cannot match the tradeoff-resistance of data-dependent ones
  (Alwen–Blocki). You give up TMTO strength as the price of the fixed schedule.
- **Side channels.** The fixed schedule is exactly what makes data-independent functions
  safe against a watcher: the fetch order reveals nothing about the secret.
  Data-dependent functions leak the secret through the fetch order.

So the fork is real: **data-dependent buys steep TMTO but leaks through side channels;
data-independent is side-channel-safe but has a weaker TMTO ceiling.**

## The memchal twist that resolves the fork

Side-channel resistance protects a **secret input from a watching attacker** — a password
hashed where someone else can snoop. Memchal has neither half of that threat:

- The "secret" is the seed, and the **prover already holds it** — the verifier handed over
  the recipe. There is nothing to leak to the party we worry about.
- The party we worry about *is* the builder, and its goal is not to learn the seed — it is
  to avoid renting floor space.

So the usual reason to accept a weaker TMTO (buying side-channel safety) **does not apply
here.** Memchal can take the data-dependent branch and its steep tradeoff essentially for
free, because the downside it normally carries is irrelevant to us. Choosing a
data-independent or hybrid variant would mean paying a real TMTO penalty for a property
we have no use for.

## Options

| Function | Addressing | TMTO | Partial recompute | Side channels | Footprint / notes |
|---|---|---|---|---|---|
| PBKDF2 / bcrypt | none (no big memory) | none — flat | ~free | n/a | The anti-pattern; equals today's O(1) rebuild. Rejected. |
| scrypt (2009) | data-dependent | steep (area-time ~ N²) | cascade | present (moot here) | Baroque (Salsa20 + BlockMix + PBKDF2), single cost knob; in libsodium. Viable fallback. |
| **Argon2d (2015)** | **data-dependent** | **steep** | **cascade** | present (moot here) | PHC winner; Blake2b core; separate `m`/`t`/`p` knobs. Best on the merits. |
| Argon2i | data-independent | weaker (proven ceiling) | cheaper (pebbling) | resistant (unneeded) | Pays for a property we don't use. |
| Argon2id | hybrid | middle | middle | half-resistant | The password-hashing default — for a threat memchal lacks. |
| Balloon (2016) | either | steep (dep.) / weaker (indep.) | cascade / pebbling | tunable | Argon2-like hardness on *any* standard hash; could reuse a primitive we already vendor. Less battle-tested. |

## Decision (proposed)

**Adopt the data-dependent branch: Argon2d, with scrypt as the fallback.** Memchal wants
the steepest partial-recompute cascade, and the side-channel weakness that normally argues
against data-dependent addressing is a non-issue here (the prover already holds the seed
and is itself the adversary). Argon2d gives the strongest tradeoff resistance of the
well-analyzed options plus separate memory/time/lane knobs, which is exactly the control
the honest-build-vs-attacker-rebuild gap and the GPU lane count need. scrypt is the
fallback if the Argon2 core proves too heavy to vendor or port.

The **frozen challenge math is untouched.** `answer_lane_t` / `answer_challenge_t` read
`load(idx)` from a flat buffer and do not care how it was filled; the known-answer test
([ADR 0001](0001-challenge-math-frozen-pinned-by-known-answer.md)) stays byte-identical.
`build_buffer` is a new, separate function, not a change to the hot path.

## Consequences

- **Protocol.** The buffer upload is replaced by a small recipe handshake (seed/key plus
  MHF cost parameters `m`, `t`, `p`, block size, and — later — a workload-binding tag).
  For large buffers this also removes a multi-GB transfer: you trade "download B bytes"
  for "compute B bytes memory-hard," which over a real verifier→datacenter link can be a
  net win.
- **The recompute defense migrates from correctness to timing.** In send-buffer, a
  recompute cheat produced *wrong* answers (caught by correctness). In build-buffer it
  produces *right answers slowly* (caught by timing + the TMTO cascade). This leans harder
  on the signal this environment measures least faithfully
  ([ADR 0003](0003-timing-detector-scope-and-measurement.md)) — another reason the
  authoritative check rides on the GPU port.
- **Effect per prover mode.** `honest` now builds the buffer once (expensive) then
  `mlock`s and answers as before — a one-time setup tax, not a per-challenge one.
  `disk` / `mmapdisk` / `partial` build once, then spill; timing catches them as today.
  `fake` is unchanged (correctness). `prefetch` and `precompute` are orthogonal
  (access-pattern and nonce attacks) and keep their existing defenses. Only `recompute`
  changes character, as above. A new `partial-recompute` mode (keep fraction `f`,
  rebuild the rest) is what measures the TMTO curve.
- **Latency, three clocks.** (1) One-time build: all the new cost, by design, does not
  recur. (2) Per-challenge answer: unchanged — the MHF touches generation, not answering.
  (3) Verifier check: the verifier builds the full buffer too (the chained path needs the
  real bytes) and reads its own copy per challenge; it pays the one-time cost, which is
  fine because the verifier is allowed to be memory-heavy and is not the party under test.
- **New known-answer vectors** pin `build_buffer`, exactly as ChaCha20 was pinned.

## Open decision points (settled with the prototype)

1. **Cost parameters `m`, `t`, `p`** such that the honest one-time build is tolerable
   *and* any memory cut below ~full pushes per-probe rebuild past the timing budget —
   measured as a TMTO curve via a new `partial-recompute` attack mode, not asserted.
2. **The GPU parallelism knob** — how much lane parallelism the honest builder needs
   on-GPU without handing the attacker a cheap shrink. Shaped on CPU, confirmed on the
   port.
3. **Vendor vs dependency** — this is the [ADR 0006](0006-chacha20-vendored-vs-dependency.md)
   question re-decided for this scope. Argon2d means a Blake2b-based core (vendor the
   reference implementation, or depend on libsodium); Balloon-on-our-ChaCha keeps the
   footprint but is less proven. Decided alongside the final MHF pick.
4. **Block granularity** — the MHF rebuilds whole blocks (~1 KiB for Argon2) while the
   challenge reads 8-byte words, so the TMTO cost model is per-block, not per-word.

## Scope boundary

This step proves **retention of a generated buffer without uploading it**. It does **not**
yet prove the buffer is tied to the actual model weights — that weight-binding is the
frontier flagged in [NEXT_STEPS](../../NEXT_STEPS.md); conflating the two would sink this
decision. Keep this ADR to: memory-hard generation from a short recipe, and the
recompute-defense migration to timing/TMTO.

## Related

- [ADR 0001](0001-challenge-math-frozen-pinned-by-known-answer.md) — the challenge math
  this does not touch.
- [ADR 0004](0004-adversary-taxonomy-and-defenses.md) — the send-buffer taxonomy and why
  it can't reach the real workload.
- [ADR 0006](0006-chacha20-vendored-vs-dependency.md) — the send-buffer cipher decision
  this revisits for the build-buffer scope.
- [NEXT_STEPS](../../NEXT_STEPS.md) — the roadmap items this ADR covers ("Design the
  generator", "Prototype it on CPU").
