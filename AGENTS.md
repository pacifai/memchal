# AGENTS.md — memchal

memchal checks that a prover holds a large buffer in fast memory: RAM on a CPU, or
on-chip memory on a GPU. A verifier gives the prover the buffer, then sends timed
challenges that need reads across all of it. A wrong answer means data is missing;
a slow one means it isn't in fast memory.

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Its layout, build and test steps,
and rules all apply to you. The rules below are for agents only.

- **Keep `NEXT_STEPS.md` a plain backlog** of the project's own work, most
  important first, each item a bold task name and a short description. Frame
  research items as open questions, with at most a one- or two-sentence starting
  idea.
- **Write every Markdown file for a newcomer.** Say what a thing is and why it
  matters in plain words, and keep it short. Implementation detail belongs in
  the code and the decision records.
- **Keep the README light:** what memchal is, why, a quickstart, then short
  reference tables.
- **Keep `CONTRIBUTING.md` current** when you change the layout, the build, or
  the tests.
