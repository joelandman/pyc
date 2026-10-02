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
The one proved GIL-free region (a heap-free phi-`while`) calls
`pyc_rt_gil_maybe_release`, which does the same check and otherwise
returns still holding the GIL. Detail: [GIL.md](GIL.md).
Further GIL narrowing: [GIL.md](GIL.md).

Measured: `thread_starvation.py` matches CPython; `loop_periodic.py` still
matches; concurrency corpus 10/10; `test_syslog` 11/11 in 0.18s;
`test_logging` **finishes** in 25s (no deadlock). Unconditional
SaveThread/RestoreThread was the deadlock: it dropped the GIL on every
2048 iterations whether anyone was waiting.

## C3 — remaining language diffs that are not C1/C2

**Status: residual.** Gate corpus **881/881** impactful. `STDERR_DIFFERS`
does not count against the rate. `frame_lineno_and_traceback.py` and
`test_with.py` match the sysroot, including caret columns
(`adc0c4e`). Two language-corpus stderr rows (`case_208.py`,
`case_209.py`) were not impactful on the 2026-10-02 fast gate and were
not re-diagnosed. Compiled functions fire PEP 669 `PY_START` / `PY_RETURN` / `PY_UNWIND`
from the trampoline, and Python-level calls fire `CALL` / `C_RETURN` /
`C_RAISE` (`profile_call_return.py`). `test_sys_setprofile`, `test_cprofile`,
and `test_pstats` match the sysroot when run as scripts.

Do not start "drive `Lib/test` under unittest" here. That is a completeness
increment and **will lower** the published I6 number; it is not a
correctness fix.

## Order

C1 and C2 closed. Unboxing may proceed.

## Locations — `instr_ptr` for traceback carets

**Status: closed for the probes that failed.** `pyc_rt_set_lasti` points
`instr_ptr` at a code unit whose line table contains the source span.
The compiler passes a dense slot. The first visit resolves the unit
(exact column span, then start column, then line) and stores it; later
visits are an array load (`adc0c4e`). Eval stubs store unit `8 + slot`
and do not build that map. The per-line call is still there, which is
why it remains visible in profiles. Not calling it unless a tracer is
installed, or an exception pad is recording a traceback, is open.
