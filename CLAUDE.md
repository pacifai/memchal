# CLAUDE.md — memchal

memchal checks that a prover holds a large buffer in fast memory: RAM on a CPU, or
HBM on a GPU. A verifier gives the prover the buffer, then sends timed challenges
that need reads across all of it. A wrong answer means data is missing; a slow one
means it isn't in fast memory.

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Its layout, build and test steps,
and rules all apply to you. The rules below are for agents only.

- **Investigations stay local.** Each lives in its own folder under
  `docs/investigations/` (question, script, results), which `.git/info/exclude`
  keeps out of git. Tracked files cite one by its folder name in backticks.
- **Keep `NEXT_STEPS.md` a plain backlog** of the project's own work, most
  important first, each item a bold task name and a short description. Frame
  research items as open questions, with at most a one- or two-sentence starting
  idea.
- **Keep `CONTRIBUTING.md` current** when you change the layout, the build, or
  the tests.
