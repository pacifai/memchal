# ADR 0002 — Verify the lock took effect; refuse to run unpinned

**Status:** accepted

## Context
The honest baseline's entire value is that its buffer is genuinely resident and
non-swappable. `mlock()` can return success without the pages being wired (and its
return value is all the prototype trusted). An honest baseline that silently ran
unpinned would produce plausible-but-meaningless numbers — exactly the failure this
phase guards against.

Measured: on Linux, `mlock` of the buffer does wire the pages (`/proc/self/status
VmLck` reflects it); natively on macOS 26.5 a non-root `mlock` also wired ~512 MiB
(seen via `vm_stat` "Pages wired down"). The return value was truthful in every
configuration tested — but we verify anyway, because the *guarantee* is what's
load-bearing and its trustworthiness is platform/version dependent.

## Decision
`prover.cpp` verifies the lock by reading `VmLck` from `/proc/self/status`
(`mc::locked_bytes()` in `common.hpp`) and sets `locked=1` only if `VmLck >=`
buffer bytes (on no-`/proc` platforms it falls back to the return value, marked
unverified). If the buffer is **not genuinely pinned**, the prover prints a FATAL
banner and **refuses to run**. `--allow-unlocked` overrides, prints a prominent
UNLOCKED banner, and sends `locked=false` down the wire so it propagates into the
JSON/CSV/plots. `cheating_prover` applies the same verification (no refuse —
disk/partial/fake are legitimately unlocked).

## Consequences
- No result can silently rest on an unpinned baseline; lock status is visible per
  run in the harness output and stamped into artifacts.
- `run.sh` grants `IPC_LOCK` + unlimited memlock so the honest path locks cleanly.
