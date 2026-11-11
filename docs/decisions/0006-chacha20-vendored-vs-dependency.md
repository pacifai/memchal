# ADR 0006: ChaCha20 is a small file in the repo, not a library

**Status:** accepted for a buffer the verifier sends. For a buffer built from a
recipe, [ADR 0007](0007-buffer-generator-memory-hard-function.md) decides the
generator.

## Context

`--gen chacha` fills the buffer with ChaCha20 (RFC 8439). The implementation is
about 120 lines in `src/shared/chacha20.hpp`. The question was whether to use a
well-reviewed library instead.

A library brings code many people have reviewed. It also adds a build
dependency, and it can't run inside GPU code. A file in the repo has neither
problem, but the project vouches for it alone.

## Options

- **Keep the file in the repo.** No dependencies, usable on the GPU, and pinned
  to the RFC test vectors.
- **Copy in a public-domain single-file implementation**, such as Daniel J.
  Bernstein's reference code or Monocypher's. It brings outside review with no
  build dependency, but more code than memchal needs.
- **Depend on libsodium or OpenSSL.** Reviewed and maintained, but a link
  dependency that can't run on the GPU, which breaks the rule that the core
  builds with `g++` and the standard library only.

## Decision

Keep the file in the repo. Here, ChaCha20 fills a test buffer rather than
protecting a secret, and the known-answer test against the RFC vectors carries
the trust. No dependencies and GPU support are worth more than outside review
of 120 lines.

Revisit this if the generator starts protecting something real. A buffer built
from a recipe is that case, and ADR 0007 decides it.

## Consequences

- `src/shared/chacha20.hpp` stays, pinned by its known-answer test.
- If this decision is reversed, a new record says so, with the tests that come
  with the change.
