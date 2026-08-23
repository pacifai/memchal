# ADR 0006 — ChaCha20: vendored implementation vs a dependency

**Status:** accepted — keep the vendored implementation. Revisit if the security bar rises (see "When to revisit").

## Context

The crypto buffer generator (`--gen chacha`) uses a small vendored ChaCha20 in
`src/chacha20.hpp` — about 120 lines, RFC 8439, table-free, pinned by a known-answer
test. During review of PR #1 the question came up: should this be an off-the-shelf
implementation instead of our own bytes?

The tension is provenance versus footprint. An audited library is bytes many people
have reviewed, but it adds a build and link dependency and isn't portable to the GPU
answer path the project is heading toward. The vendored file is dependency-free and
GPU-portable, but it's code we maintain and vouch for ourselves.

## Options

- **Keep the vendored implementation (today's default).** Dependency-free,
  GPU-portable, and pinned by a known-answer test. Cost: we own ~120 lines of crypto.
- **Vendor a public-domain single-file implementation** (DJB's reference `chacha.c`,
  or the ChaCha core from Monocypher). Gains external provenance with still no build
  dependency, but pulls in more code than we need.
- **Depend on libsodium or OpenSSL.** Audited and maintained, but adds a link
  dependency, isn't header-only, and isn't GPU-portable — which conflicts with the
  repo's "g++ + standard library only" rule and the GPU-shaped answer path.

## Decision

Keep the vendored implementation. At the PoC's current bar, what carries trust is the
known-answer test that pins the bytes to the RFC vectors, not the provenance of the
source file — and zero dependencies plus GPU portability are worth more here than
external review of ~120 audited lines. The other options stay on the table for the day
the bar rises.

## When to revisit

Reopen this decision if the generator moves onto a real security boundary — most
concretely, the build-buffer design, where a memory-hard, weight-bound generator would
sit on the trust path rather than filling a benchmark buffer. At that point prefer a
public-domain single-file implementation (DJB's reference `chacha.c` or Monocypher's
core) for external provenance with no build dependency, and take on libsodium/OpenSSL
only if the boundary demands an audited, maintained library and the GPU path can absorb
the dependency.

## Consequences

- The vendored `src/chacha20.hpp` stays as the crypto buffer generator, pinned by its
  known-answer test. If this decision is reversed, this ADR records the swap and the
  test that must accompany it.
