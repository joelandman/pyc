# Correctness plan

CHARTER order: correctness, completeness, performance. This file is the work
that remains on the first of those. Performance is [UNBOXING.md](UNBOXING.md).

Every item below was measured. Probes that currently fail live in
`verify/corpus/known-gaps/` and are in the PR gate so the published number
carries the gap.

## C1 — per-function Python frames (P0)

**Status: closed.** Not opt-in (I2). Frame probes live in `language/`.
Gate corpus **787/788** (the timeout is C2 `thread_starvation.py`).

**C1a** was eager `PyFrame_New` (59.88 ns/call, 2.43× slower than CPython).
**C1b** pushes `_PyInterpreterFrame` on the thread datastack via
`_PyThreadState_PushFrame`. `PyFrameObject` is created only if something
asks (`sys._getframe`); `_PyFrame_ClearExceptCode` takes ownership so it
does not dangle. Measured: `locals()` returns the dict, `incomplete=0`,
`FRAME_SPECIALS_SIZE == 10` on cp314. Function, module, and class bodies
all use this path.

**Done.** Nine frame probes moved to `language/`; `compiler/baseline-language.json`
refreshed at 787/788. No new P0. Next is C2.

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

**Status: after C2.** Gate corpus has no `STDOUT_DIFFERS`. `STDERR_DIFFERS`
(traceback carets) do not count against the rate.

Do not start "drive `Lib/test` under unittest" here. That is a completeness
increment and **will lower** the published I6 number; it is not a
correctness fix.

## Order

C1 closed. C2 next. Unboxing stays behind C2 only insofar as compiled
loops must still be interruptible; the unboxing analysis has already landed.
