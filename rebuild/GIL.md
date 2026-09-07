# GIL notes (after C2)

How compiled code holds the GIL, what went wrong, and where a *smaller*
critical section might be legal. CHARTER I2 still binds: dropping the GIL
must not change an observable result. Numbers below are measured unless
marked as hypothesis.

## What C2 measured

A compiled `while not stop: n += 1` held the GIL to completion. Another
thread's store of `stop` was never observed (`thread_starvation.py`).

| approach | `thread_starvation` | `test_syslog` | `test_logging` |
|---|---|---|---|
| no yield (pre-C2) | hang | hang | ~23s, passes |
| `PyEval_SaveThread` / `RestoreThread` every 2048 loop heads | pass | pass 0.2s | **deadlock** (`test_config_queue_handler`) |
| `_Py_HandlePending` every 2048 loop heads (shipped) | pass | 11/11 in 0.18s | **finishes** in 25s (no deadlock) |

Unconditional drop is not "more concurrent". It is a different schedule.
`QueueListener.stop` enqueues a sentinel and joins: yielding the GIL from a
loop that still logically owns the listener's protocol produced one thread,
0% CPU, no progress. Two guesses at that deadlock (finalisation,
`PyGILState_Check`) were refuted; we never got a stack. The mechanism that
*is* established is CPython's: **detach only when another thread set
`_PY_GIL_DROP_REQUEST_BIT`.**

`cp314t` never had the hang. There is no GIL to starve. Same 2M-iter loop:
pyc 0.84s (`cp314`) vs 1.05s (`cp314t`); CPython 0.13s vs 0.21s. PEP 703's
trade, not a reason to make free-threading the default.

Gate corpus after C2: **788/788** impactful.

## What the GIL actually serialises

Under `cp314`, the GIL is the lock on **the process-wide Python object
graph**: `PyObject*` identity, refcounts, allocator, dict/list mutation,
type objects, the import table, exceptions, the current thread state /
frame. It is not a lock on arithmetic.

So a compiled loop of boxed `n += 1` *does* need the GIL on every
iteration: that is `PyNumber_Add` plus refcount traffic on shared heap
objects. A loop that never materialises a `PyObject*` does not, **if**
nothing else can observe those values (the same proof `UNBOXING.md`
already requires).

CPython itself already splits the two: bytecode holds the GIL; `time.sleep`,
I/O, and `queue.get` drop it. NumPy and PyTorch drop it around native
kernels. We should not invent a third policy.

## Layers, cheapest first

### 0. What we have (C2)

`pyc_rt_periodic` → `_Py_HandlePending`. Signals, pending calls, GC bit,
and GIL drop **if requested**. Amortised 2048 loop heads, same as the
signal-only check that landed before.

Hypothesis, not measured: 2048 is coarse compared to CPython's per-bytecode
eval-breaker load. Tight loops that do almost no Python work may still
look sticky to a waiter for ~2048 iters. Tightening the interval is a
measurement, not a design change. Do not tighten it to "fix" a hang
without a breaker bit — that is tuning a race again.

### 1. Unboxed regions already have the right shape

`int_locals()` accepts only uncaptured, function-local ints. Those values
are not in `%locals`, not in a cell, not a global. No other thread can
name them. Steps 2–3 of unboxing (tagged i64 slots, native `range`) are
exactly "this loop is not a Python object graph walk".

Hypothesis: once the accumulator lives in `%s.v`, the loop body can run
**without the GIL**, and must:

- still call `_Py_HandlePending` (signals, stop-the-world on `cp314t`,
  GIL drop if this thread later needs to box);
- **reacquire before any deopt** (overflow → `PyLong_*`, boxed fallback);
- **reacquire before any object op** (print, store to a list, call).

That is Cython `nogil` with a proof instead of an annotation. The proof
is the existing analysis. Shipping GIL-free unboxed loops before the
unboxed IR exists would be dropping the GIL around `PyNumber_Add`, which
is undefined.

Do not drop the GIL around a boxed loop because "it's only ints". They
are still heap objects with refcounts.

### 2. Object ops stay in a critical section; compute does not

Where unboxing is not proved, the next cut is **span of the hold**, not
"no GIL":

- Hold across `INCREF` / `DECREF` / `PyDict_SetItem` / anything that
  reads `ob_type`.
- Drop across a run of i64/f64 that has already been unboxed *inside*
  an expression even if the store is boxed (UNBOXING's "fused
  expression, one allocation" row: 46.3 → 22.1 ns). The allocation
  re-enters the critical section.

Hypothesis: LLVM already sees the inlined refcount macros (`c2ad7df`).
A pair of `pyc_rt_gil_release` / `acquire` around a pure arithmetic
subgraph would be visible as well, but only if the subgraph has no
calls into libpython. One missed `Py_DECREF` in the "nogil" region is a
data race on every CPython build, including ours.

### 3. Do not build a second lock

Per-object mutexes, biased refcounts, and biased locking **are** the
`cp314t` ABI. CHARTER I8 already treats `cp314` and `cp314t` as separate
targets. Reimplementing a mini-GIL around `PyObject*` on `cp314` while
linking libpython that assumes the real GIL is the cpyext problem the
charter rejected.

If the workload is "lots of threads, little shared Python state", the
measured answer is: compile against `cp314t`. If the workload is "one
hot numeric loop", the measured answer is unboxing, then (1).

### 4. External C / wheels already drop it

NumPy/PyTorch in a Tier-1 binary run their kernels without the GIL
because *they* released it. pyc should not hold the GIL across
`PyObject_Call` into those just to "keep things simple" — the callee
will drop it anyway. Worth measuring later: does a compiled
`numpy.dot` in a loop spend its time in our trampoline/frame rather
than in BLAS? Unknown. Not a GIL-policy change until measured.

## What not to do

- **Unconditional yield** in `pyc_rt_periodic`. Deadlocks a real stdlib
  test. C2 exists so nobody retries this.
- **Drop GIL because a loop looks numeric.** Proof or C-API. I2.
- **Skip `_Py_HandlePending` in a nogil region.** Ctrl-C and
  stop-the-world still have to run. CPython's nogil sections still
  attach before handling events.
- **A pyc-owned shadow lock** that Python code cannot see. Same class of
  bug as the no-frame `locals()` P0: the rest of the runtime (wheels,
  `threading`, `queue`) will disagree with you.

## Suggested order when we pick this up

1. Finish unboxing steps 2–3 (`UNBOXING.md`). Until then there is no
   region that is *proved* GIL-free.
2. Measure a nested `i*j` loop with GIL held vs a hand-written nogil
   C equivalent of the unboxed IR (the 0.136s vs 0.001s bar is still
   the right one).
3. If the unboxed body is the residue, emit attach/detach around it
   with HandlePending on the same 2048 cadence, deopt reattaches first.
4. Only then consider finer critical sections around individual C-API
   calls — and only with a race test that fails if a decref happens
   detached.

Until (1), C2 is the whole GIL policy: hold it like CPython, drop it
when asked.
