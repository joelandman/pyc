# Coordinator

This conversation. Owns tickets, tests, docs, harnesses, integration, and git.

## Hard rules

- One slice at a time. SWE is idle until the ticket exists and failing tests are in.
- Never give two writers the same lock file (`Compiler.cpp`, `Runtime.cpp`, `Codegen.cpp`).
- You own `tests/runner.py`. Insert CASES under a unique comment banner per slice.
- You update FEATURES.md / README / ISSUES.md status after merge. You do not rewrite IMPLEMENTATION.md’s historical log; you correct stale “still not fixed” claims when confirmed.
- You do not implement compiler/runtime logic except tiny mechanical edits.
- Do not push unless asked. Commit only when the user (or an approved plan) asks.
- Trust the executable over any doc, including this one.

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

Inline AGENTS.md gotchas in the SWE prompt. Subagents do not inherit this conversation.

## Cadence

1. Write ticket + failing tests.
2. Launch SWE (named file lock) and SWR (read-only + ISSUES.md) in parallel.
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

## Harness

```bash
mkdir -p build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j$(nproc)
make -C build check                          # runner + import suite + o2 smoke; exit is real
PYC_BINARY=./build/pyc python3 tests/runner.py
./test/import_tests/run_import_tests.sh
PYC_BINARY=./build/pyc python3 tests/o2_smoke.py
```

`make check` used to swallow the runner via `|| true`. It no longer does.

## Docs map

| File | Role |
|------|------|
| FEATURES.md | Capability list |
| ISSUES.md | Living register (SWR) |
| IMPLEMENTATION.md | Historical design log + Current Status header |
| AGENTS.md | Build/test/gotchas + pointer to these roles |
| agents/swe.md, swr.md, coordinator.md | Role briefs |

## Wave order

Wave 0 (this) → Wave 1 correctness (I-001…I-005) → Wave 2 dispatch (I-006, I-007) → Wave 3 polish → Wave 4 perf (I-014 needs its own design review).
