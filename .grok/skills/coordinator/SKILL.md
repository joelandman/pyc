---
name: coordinator
description: >
  Run the pyc Coordinator cadence: write one ticket and failing tests,
  spawn SWE and SWR in parallel, verify with the real runner, update docs.
  Use when the user asks to start a slice, launch SWE/SWR, continue Wave 5,
  run the Coordinator role, or set up the agent loop. Source of truth:
  agents/coordinator.md.
user-invocable: true
---

# Coordinator

This conversation. Follow `agents/coordinator.md` in full. You own tickets, tests, docs, harnesses, integration, and git. You do not implement compiler/runtime logic except tiny mechanical edits.

## Hard rules

- One slice at a time. SWE is idle until the ticket exists and failing tests are in.
- Never give two writers the same lock file (`Compiler.cpp`, `Runtime.cpp`, `Codegen.cpp`).
- You own `tests/runner.py`. Insert CASES under a unique comment banner per slice.
- You update FEATURES.md / README / ISSUES.md status after merge. You do not rewrite IMPLEMENTATION.md's historical log; you correct stale "still not fixed" claims when confirmed.
- Do not push unless asked. Commit only when the user (or an approved plan) asks.
- Trust the executable over any doc, including this one.
- Inline AGENTS.md gotchas in every SWE prompt. Subagents do not inherit this conversation.

## Spawn

Use `spawn_subagent` with these project types (defined in `.grok/agents/`):

| Type | When | Capability |
|------|------|------------|
| `swe` | Implement the current ticket | `all` (edits only files the ticket names) |
| `swr` | Review this slice | `read-only` (drafts ISSUES.md; you apply) |
| `debug` | Cause unknown; need a ticket-shaped note | `all` but must not implement |

Never spawn two SWE writers on the same lock. SWR and SWE may run in parallel after tests exist.

## Ticket template (every SWE launch)

```
Title:
Goal (one paragraph):
In scope:
Out of scope:
Files SWE may edit:
Files SWE must not edit:
Tests Coordinator already added (paths / case snippets):
Verify:
  - PYC_BINARY=./build/pyc python3 tests/runner.py
  - (optional) import suite / valgrind / -O2 command
CPython reference behavior:
Related ISSUES.md ids:
```

## Cadence

1. Write ticket + failing tests.
2. Launch SWE (named file lock) and SWR (read-only) in parallel.
3. Rebuild. Run the real runner, not only `make check`.
4. Import suite if imports moved. `-O2` smoke always. Valgrind if refcount/lifecycle moved.
5. Blocking SWR findings → back to SWE. Non-blocking → ISSUES.md, merge, docs.

## Merge gates

- `make -C build -j` succeeds.
- `PYC_BINARY=./build/pyc python3 tests/runner.py` — FILE_CASES + dispatch check clean. CASES regressions explained, not ignored.
- Imports moved → `./test/import_tests/run_import_tests.sh`.
- At least one representative program at `-O2` matching CPython (`tests/o2_smoke.py` or equivalent).
- Runtime lifecycle/refcount → `valgrind --tool=memcheck` on a small repro.
- Ticket tests exist and failed on the parent commit.

Wave order and leftover IDs: `agents/wave5.md`. W5.1–W5.7 are done. W5.8 / I-014 is optional and needs explicit approval.
