# Correctness plan

CHARTER order: correctness, completeness, performance. This file is the work
that remains on the first of those. Performance is [UNBOXING.md](UNBOXING.md).

Every item below was measured. Probes that currently fail live in
`verify/corpus/known-gaps/` and are in the PR gate so the published number
carries the gap.

## C1 — per-function Python frames (P0)

**Status: closed.** Not opt-in (I2). Frame probes live in `language/`.

**C1a** was eager `PyFrame_New` (59.88 ns/call, 2.43× slower than CPython).
**C1b** pushes `_PyInterpreterFrame` on the thread datastack via
`_PyThreadState_PushFrame`. `PyFrameObject` is created only if something
asks (`sys._getframe`); `_PyFrame_ClearExceptCode` takes ownership so it
does not dangle. Measured: `locals()` returns the dict, `incomplete=0`,
`FRAME_SPECIALS_SIZE == 10` on cp314. Function, module, and class bodies
all use this path.

**Done.** Nine frame probes in `language/`.

## C2 — compiled loops never offer the GIL (hang)

**Status: closed.** `pyc_rt_periodic` calls `_Py_HandlePending`, which
detaches the GIL only when another thread set `_PY_GIL_DROP_REQUEST_BIT`.
That is CPython's eval-breaker path, not unconditional SaveThread.

Measured: `thread_starvation.py` matches CPython; `loop_periodic.py` still
matches; concurrency corpus 10/10; `test_syslog` 11/11 in 0.18s;
`test_logging` **finishes** in 25s (no deadlock). Unconditional
SaveThread/RestoreThread was the deadlock: it dropped the GIL on every
2048 iterations whether anyone was waiting.

## C3 — remaining language diffs that are not C1/C2

**Status: residual.** Gate corpus **788/788** impactful. `STDERR_DIFFERS`
(traceback carets) do not count against the rate.

Do not start "drive `Lib/test` under unittest" here. That is a completeness
increment and **will lower** the published I6 number; it is not a
correctness fix.

## Order

C1 and C2 closed. Unboxing may proceed.
