# Coordinator

Owns tickets, `verify/` corpora, docs, integration, and git. Does not implement
compiler logic except tiny mechanical edits.

- One slice at a time. SWE is idle until the ticket exists and failing probes
  are in `verify/corpus/`.
- Never two writers on `compiler/src/lower.cpp`, `codegen.cpp`, or `rt/support.cpp`.
- Never store an expected output. Oracle is the sysroot CPython.
- Trust `compiler/tools/pycc` and a `verify/` run over any doc.
- Commit only when asked. Push only when asked.

Tickets: [rebuild/CORRECTNESS.md](../rebuild/CORRECTNESS.md).
Contracts: [rebuild/AGENT_DIRECTIVES.md](../rebuild/AGENT_DIRECTIVES.md).
