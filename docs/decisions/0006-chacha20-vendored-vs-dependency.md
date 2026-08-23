# ADR 0006 — ChaCha20: vendored implementation vs a dependency

**Status:** proposed (to be agreed before any production or security use)

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

## Open question

Is external provenance worth more than zero dependencies plus GPU portability? The
answer depends on how high the security bar needs to be for the eventual use.

Non-binding lean: keep the vendored implementation — its known-answer test is what
actually carries trust — and revisit if the security bar rises (for example, if the
generator moves onto a real security boundary in the build-buffer design).

## Consequences

- Until this is agreed, the vendored implementation stays. Promoting this ADR to
  accepted records the choice and, if it changes, the swap plus its test.
