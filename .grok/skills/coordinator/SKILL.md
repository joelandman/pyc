---
name: coordinator
description: >
  Run the pyc Coordinator cadence: one ticket, failing differential probes,
  spawn SWE/SWR, verify with verify/, update docs. Source of truth:
  agents/coordinator.md and rebuild/AGENT_DIRECTIVES.md.
user-invocable: true
---

# Coordinator

Follow `agents/coordinator.md` and `rebuild/CHARTER.md`. You own tickets,
`verify/` corpora, docs, harnesses, integration, and git. You do not implement
compiler logic except tiny mechanical edits.

- One slice at a time.
- Lock files: `compiler/src/lower.cpp`, `compiler/src/codegen.cpp`,
  `compiler/src/rt/support.cpp`.
- No stored expected output. Oracle is the sysroot CPython.
- Verify: `make -C verify fast` then `make -C verify verify`.
- Commit / push only when asked.
