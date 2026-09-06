# Correctness plan

CHARTER order: correctness, completeness, performance. This file is the work
that remains on the first of those. Performance is [UNBOXING.md](UNBOXING.md)
and does not start until C1 is closed.

Every item below was measured. Probes that currently fail live in
`verify/corpus/known-gaps/` and are in the PR gate so the published number
carries the gap.

## C1 — per-function Python frames (P0)

**Status: C1a complete.** CHARTER deferred this on 2026-08-25; taking it on is
now the job. Not opt-in (I2). known-gaps frame probes: **21/22** (the
remaining timeout is C2).

**C1a (eager `PyFrameObject`).** Measured first: `PyFrame_New` plus linking
`tstate->current_frame` makes `locals()` return the dict. Wired into the
function trampoline, the module body, and class bodies (`pyc_rt_push_frame`
on the namespace, capsule dtor pops so landing pads unwind). The three
silent P0s and `frame_builtins.py` match CPython.

CHARTER cost: 59.88 ns/call — slower than CPython. **C1b** replaces this
with a C-stack `_PyInterpreterFrame` (est. 8–12 ns).

Compiled functions push no Python frame. `sys._getframe` raises rather than
lying (I1-clean at that boundary), but callers degrade:

| probe | pyc | CPython | class |
|---|---|---|---|
| `locals_none_p0.py` | `None` | `{'a': 1}` | silent, exit 0 |
| `globals_none_p0.py` | `True` (`is None`) | `False` | silent, exit 0 |
| `locals_bool_p0.py` | `False` | `True` | silent, exit 0 |
| `vars` / `eval` / `exec` | raise | work | loud, same hole |

`doctest._normalize_module` walks one frame too few and returns an empty
suite, so compiled `Lib/test/test_unpack.py` reports OK while running half
its tests. `logging.findCaller`, `warnings` `stacklevel`,
`dataclasses`/`namedtuple` module resolution: same gap.

**Approach (already costed in CHARTER):** `_PyInterpreterFrame` with lazy
`PyFrameObject`. Eager `PyFrameObject` is 59.88 ns/call — **2.43× slower
than CPython**, non-starter. Interpreter-frame estimate ~8–12 ns vs 3.42 ns
today, still ~2–3× faster than CPython.

Coupling: `_PyInterpreterFrame` is `Py_BUILD_CORE` and its layout differs
between `cp314` and `cp314t`. A layout-conformance check must fail the build
when it shifts (I8).

Do **not** revive the module-level trampoline (ce52560, reverted): it made
the doctest failure quieter.

**Done when:** the nine known-gaps frame probes match CPython; they move into
`verify/corpus/language/`; the language baseline refreshes; no new P0.

**Verify:** `make -C verify fast` with `--fail-on-silent-wrong`, then the
known-gaps corpus, then a `Lib/test/test_unpack.py` doctest probe.

## C2 — compiled loops never offer the GIL (hang)

**Status: blocked on mechanism.** Open on `cp314`. Absent on `cp314t`.

A worker in a compiled loop holds the GIL to completion
(`thread_starvation.py` times out). Signal handlers already run
(`loop_periodic.py` passes). `PyEval_SaveThread`/`RestoreThread` at each
loop head made `test_syslog` pass in 0.2s and **deadlocked** `test_logging`
(`test_config_queue_handler`). Two hypotheses refuted; no stack trace
(`ptrace_scope`). Tuning the interval would tune a race.

Do not revert the alloca hoist to buy `test_syslog` back — that hang was
luck, and reverting reintroduces C-stack exhaustion.

**Done when:** `thread_starvation.py` matches CPython on `cp314` and
`test_logging` still completes. Mechanism written down, not guessed.

## C3 — remaining language diffs that are not C1/C2

**Status: after C1.** The language baseline's `STDOUT_DIFFERS` / `EXIT_DIFFERS`
are the frame probes. `STDERR_DIFFERS` (traceback carets) do not count
against the rate and are not C1.

Do not start "drive `Lib/test` under unittest" here. That is a completeness
increment and **will lower** the published I6 number; it is not a
correctness fix.

## Order

C1 → C2 (investigation can overlap) → C3. Unboxing stays behind C1.
