# TRACING — performance tooling for generated code

**Status: implemented (2026-11-08).** `--trace` and `--flamegraph` are live
driver flags (INTERFACES §5, v1.5). Both are measured, not assumed: dormancy
is a gated invariant in `verify.yml`, well-formedness is gated, overhead is
published in STATUS.

A0 request (2026-11-08, the project owner):

- a `--trace` option producing an instrumented binary with the capability to
  emit **Perfetto** traces, and
- a `--flamegraph` option producing an instrumented binary that emits an
  **SVG flamegraph**.

Signed-off decisions (A0, 2026-11-08): default sample rate **99 Hz**;
`C:` boundary slices **on by default** under `--trace`; env names
`PYC_TRACE` / `PYC_TRACE_LINES` / `PYC_FLAMEGRAPH` / `PYC_FLAMEGRAPH_HZ`.

## 1. Why this is cheap to build: what already exists

The hard part of a Python tracer — *where the event sites are* — is already
in the runtime:

1. **Every generated function call goes through one wrapper**
   (`rt/support.cpp`), and it unconditionally fires the CPython monitoring
   events `PY_MONITORING_EVENT_PY_START` / `PY_RETURN` / `PY_UNWIND`
   (`pyc_rt_profile_enter`/`return` → `fire_monitor`); calls crossing the C
   boundary fire `CALL` / `C_RETURN` / `C_RAISE`
   (`pyc_rt_profile_ccall`/`creturn`/`crease`). With no tool registered these
   are a single pointer-read fast path.
2. **Frames are real CPython frames** (C1b): `ts->current_frame` is a
   `_PyInterpreterFrame` owning a `PyFrameObject` with the `f_back` chain
   and `instr_ptr`. Public C API is sufficient for the flamegraph's stack
   walk — no internal headers, no new ABI (§5).
3. **Per-line updates already exist** (`pyc_rt_set_lasti*`, `rt/frame.cpp`):
   a slot-cached store on the no-tracer fast path (the 9.3%-of-runtime
   symbol in the STATUS speed section) and a line-event dispatch when a
   tracer is present.
4. **Generator/coroutine bodies are real CPython bytecode** (`lower.cpp`:
   "A def containing yield, and every async def, has its body compiled" to
   a marshalled code object). They run in the eval loop, so
   `PY_START`/`PY_YIELD`/`PY_RESUME`/`PY_RETURN` fire natively for them.
   **Measured** (2026-11-08): a generator probe produces a
   byte-identical 13-event stream under pyc and the oracle (start,
   yield, resume, yield, resume, return). The design-phase "known gap" on
   generators is therefore not a gap — it was closed before a line was
   written.

Consequence: **neither option changes codegen, lowering, or IR.** Both are
runtime cores selected at link time (driver.cpp adds the TU + a define for
entry.cpp), and both ride the monitoring/frame plumbing above.

## 2. Measured evidence that the mechanism works (2026-11-08)

1. **Monitoring events fire on native frames.** A probe program that
   registers a `sys.monitoring` tool under `PROFILER_ID` and calls its own
   compiled functions produced, **under pyc, a byte-identical event stream
   to the sysroot CPython oracle** — 42 events, same `PY_START` qualnames
   and firstlinenos, same `PY_RETURN`s, same `CALL`s for both Python-level
   and C calls.
2. **LINE events fire.** A 1000-iteration loop produced **2003** LINE events
   under pyc vs the oracle's **2007** (99.8%); program output identical.
   The 4-event delta sits in the window around monitoring setup — a bounded,
   recorded delta, not a value (I1 not engaged). Root-causing it remains an
   open, low-priority item.
3. **Perfetto's UI imports Chrome JSON natively**, so the binary carries no
   protobuf dependency.
4. **Frames are real CPython frames** (§1.2): the walker in §5 needs only
   public C API.

### 3.14 API facts learned while implementing (measured, 2026-11-08)

These diverge from the 3.13-era docs and from the C header; all were
discovered by failing and re-measuring on the target sysroot:

- `sys.monitoring` is **not importable by name** (`import sys.monitoring`
  fails, even in the oracle) — reach it as an attribute of `sys`.
- The **tool is a name** (str), not an object: `use_tool_id(PROFILER_ID,
  "pyc-trace")`; passing a module raises "tool name must be a str".
- **`register_callback` takes exactly one event per callback** — a mask is
  rejected ("The callback can only be set for one event at a time").
- The **runtime event numbering diverges from the C header**: the
  interpreter's `events.PY_START` is 1, but `PY_MONITORING_EVENT_PY_START`
  is 0 (= `NO_EVENTS`, rejected). Event constants therefore come from the
  module at bootstrap, which is also robust across target versions.
- Callback arities: `PY_START`/`PY_RESUME` `(code, off)`; `PY_RETURN`/
  `PY_YIELD`/`PY_UNWIND` `(code, off, value)`; `CALL`/`C_RETURN`/`C_RAISE`
  `(code, off, callable_or_value, arg0)`; `LINE` `(code, line)`.
- `PyGILState_STATE` is an **opaque handle** (no longer a
  `PyThreadState*` typedef); failure of `PyGILState_Ensure` is a fatal
  error, so no NULL guard is possible or needed.

## 3. Contract (INTERFACES §5, v1.5)

```
pyc <source.py> [-o OUT] [--trace] [--flamegraph]
```

| Flag | Binary gains | Runtime activation | Without env var |
|---|---|---|---|
| `--trace` | tracing core linked in | `PYC_TRACE=<file.json>`; `PYC_TRACE_LINES=1` adds LINE events | byte-identical behaviour to an unflagged binary |
| `--flamegraph` | sampling profiler linked in | `PYC_FLAMEGRAPH=<file.svg>`; `PYC_FLAMEGRAPH_HZ=N` (default 99, clamped 1–10000) | byte-identical behaviour to an unflagged binary |

They may be combined (verified: both outputs produced, program output
byte-identical to the plain build). "Instrumented binary" = the capability
is linked; *activation* is a runtime decision, so one binary serves both
clean runs and measured runs. The exit-0-iff-output rule and the
stderr-only diagnostics rule are unchanged. The dormancy guarantee is a
**gated invariant** (verify.yml "Instrumented builds" step), not an
assertion.

While active, the trace owns the reserved `PROFILER_ID`: a program that
tries to install its own profiler under that id fails with a `pyc: trace:`
diagnostic and the program runs on, untraced. Dormant, nothing is
registered, so there is no conflict.

## 4. `--trace`: Perfetto (Chrome-JSON) trace

`rt/trace.cpp` (linked only when `--trace`). One JSON object per line (NDJSON
— the Chrome trace-event format), timestamps `CLOCK_MONOTONIC`, µs.

| Source (monitoring event) | Emits | Payload |
|---|---|---|
| `PY_START` | `B` slice `qualname` | `cat: "pyc.func"` |
| `PY_RETURN`, `PY_UNWIND` | `E` | closes the slice — exception paths close too, with `args: {"exception": true}` |
| `PY_RESUME`, `PY_YIELD` | segment close/open | generator/coroutine bodies: slices are emitted as **complete (`X`) events per executed segment** (start→yield, resume→yield, …, resume→return). Two `B`s before an `E` is not valid Chrome nesting, and suspension must not count as execution time. |
| `CALL` + `C_RETURN`/`C_RAISE` | `X` slice `C:<name>` | `cat: "pyc.ccall"` — time spent *outside* generated code (libpython / stdlib C). The STATUS speed analysis says the cycles live here; this is where you see it. Calls to `PyFunction`s (stdlib Python **and** pyc functions — pyc functions are real `PyFunctionObject`s whose vectorcall is swapped to the native trampoline) open their own slice on `PY_START` instead. |
| `LINE` (only with `PYC_TRACE_LINES=1`) | `i` instant, thread-scoped | `args {file, line}` |
| init / first-sight | `M` metadata | `process_name`, per-thread `thread_name` |

Mechanism:

- An **in-process C extension module** (`PyModule_Create`, no .so on disk)
  owns nine `METH_FASTCALL` C callbacks that always return `None` and never
  leave an exception set (a raised monitoring callback propagates into user
  code — measured once, fixed: an un-cleared `__name__` lookup error
  surfaced as `SystemError` at an unrelated site).
- Bootstrap (one-time, in `pyc_rt_trace_init`, which runs in `pyc_rt_main`
  after the interpreter is ready and before the module body):
  `use_tool_id` → `set_events` → `register_callback` per event (§2's 3.14
  facts). Any bootstrap failure prints `pyc: trace: <reason>` and runs on
  untraced — the tool never takes the program down.
- Callbacks run with the GIL held (the monitoring delivery contract), so
  `code`/`co_qualname`/`co_filename` reads are safe and the writer
  (4 KiB-buffered `FILE*`, 64 KiB `setvbuf`) needs no lock.
- `pyc_rt_trace_end` (after the module body; `atexit` backstop) closes any
  still-open slices so the file is **always balanced**, then flushes.

Scope notes (measured behaviour, not gaps):

- The **module frame is not in the trace**: the module's `PY_START` fires
  before the tool registers (and its exit after `end()`). Top-level code
  therefore appears as its called functions' slices, not under a `<module>`
  root. The file is still balanced (verified: B/E counts equal, stack
  simulation leaves nothing open, including on uncaught-exception exits).
- **Stdlib Python functions are traced too**: the events are
  process-wide, so a call into `importlib` or `re` shows real slices
  (measured: `_find_and_load`, `parse`, … appear in traces). Correct for a
  process profiler; the `C:` slices mark where time leaves Python.
- Comprehensions produce **no `<listcomp>` slices** — CPython 3.12+
  inlines them (PEP 709); the oracle agrees.

## 5. `--flamegraph`: sampling profiler → SVG

`rt/flame.cpp` (linked only when `--flamegraph`):

- `pyc_rt_flame_init` (same hook) spawns a **detached sampler thread** iff
  the env var is set; dormant cost is one `getenv`.
- Loop: sleep to the next tick → `PyGILState_Ensure` → walk →
  `PyGILState_Release`. The GIL-protected walk is the same discipline
  CPython's `sys._current_frames`/faulthandler use; a signal-based
  async-signal-safe walker was considered and rejected for v1 (it can read
  frames mid-teardown).
- **Walk (bounded at 256 frames), public C API only**:
  `_PyThread_CurrentFrames()` (the C implementation of
  `sys._current_frames()`: dict tid → frame), `PyFrame_GetBack` (new
  reference; the chain includes the `<module>` frame — verified against
  `sys._getframe` under pyc and the oracle), `frame.f_code` attribute →
  `PyCodeObject.co_qualname`. (The design doc's `PyFrame_GetCode` does not
  exist in 3.14; `f_code` is the public route.)
- Aggregation: `std::map<folded-stack, uint64>` written by the sampler
  thread only; read after `join()`. No locks anywhere.
- On exit: join, then the **SVG writer** (same TU, no dependencies): tree
  from the folded stacks; children sorted **by name** (deterministic
  layout — stable across runs); x/width from cumulative sample fractions;
  one `<rect>` + `<title>` (name; self samples/%; total samples/%) per
  node; colour = FNV-1a hash → HSL hue (Gregg-style palette); a header
  comment carries samples, unique stacks, Hz, and the sampling window.
  Sub-pixel boxes are skipped. A run that collects zero samples (program
  shorter than the first tick) still writes a valid SVG that says so.

**Two measured concurrency bugs, both found and fixed on this pass:**

1. **Deadline accumulation → GIL ping-pong.** The sampler's sleep deadline
   was accumulated across ticks. When `PyGILState_Ensure` blocked longer
   than one tick (the main thread holds the GIL for long native stretches —
   measured ~5 ms on nbody), the deadline landed in the past and
   `clock_nanosleep` returned immediately, turning the sampler into a
   GIL-hammer that starved the program: **>100× slowdown** on nbody, both
   threads parked on futex at ~6% CPU, non-deterministic across runs.
   Fix: recompute the deadline from "now" each iteration (an early wake
   from a signal costs at most one tick of drift).
2. **Join-with-GIL deadlock.** `pyc_rt_flame_end` ran on the main thread,
   which holds the GIL, and called `thread.join()`; if the sampler was in
   its `Ensure` window (~5 ms of each ~15 ms tick), it waited on the GIL
   that the main thread only released later in `Py_FinalizeEx`, while the
   main thread waited in `join()` — a permanent deadlock (~1 in 3 runs
   hung before the fix). Fix: release the GIL around the join
   (`PyEval_SaveThread` / `PyEval_RestoreThread`). After the fix: 10/10
   clean runs on the reproducer.

What a sample captures while the main thread is inside C code that released
the GIL (numpy, …): the walk shows the Python frame that made the C call —
C time is attributed to its caller, as in any Python-level sampler. For
the C interior use `--trace` (`C:` slices) or `perf`. Note also that sample
granularity is bounded by the main thread's GIL-release frequency (a pure
native loop releases at C-call boundaries only).

## 6. Verification

Gated (verify.yml "Instrumented builds" step, every push):

1. **Dormancy.** Smoke program (functions, a generator, an exception, a C
   call) compiled plain / `--trace` / `--flamegraph` / both; all run
   without the env vars; stdout, stderr, and exit code must be
   **byte-identical** to the plain build.
2. **Well-formedness.** `PYC_TRACE=` run: every line parses as JSON,
   metadata + function slices present, B/E simulate a balanced stack to
   EOF, at least one `C:` slice. `PYC_FLAMEGRAPH=` run (999 Hz): parses as
   XML, root `<svg>`, at least one frame box. The combined build must
   produce both, with program output unchanged.

Measured overhead (2026-11-08, `-O2` unless noted; median of 3):

| Program (workload) | plain | `--trace` dormant | `--trace` active | `--flamegraph` dormant | `--flamegraph` active (99 Hz) |
|---|---:|---:|---:|---:|---:|
| `nbody` 500000 (arithmetic-dense) | 1.316 s | 1.327 s (≈0%) | 1.313 s (≈0%) | 1.326 s (≈0%) | 1.316 s (≈0%) |
| `multi` 200k nested calls (`-O0`, function-dense) | 0.043 s | — | 0.420 s (**≈10×**) | — | 0.036 s (≈0%) |

Reading: dormant cost is within noise everywhere (the gated invariant,
now with a number). **Active tracing costs per event** — an arithmetic
loop fires almost no events (its calls don't cross the Python/C boundary
per iteration), so ≈0%; a function-dense program pays ~10× per 200k
function entries (and ~16× with `PYC_TRACE_LINES=1`; the LINE trace there
is 190 MB vs 83 MB without — per-event JSON is the cost, and it is opt-in).
**Active flamegraph cost is ≈0%** at 99 Hz on both workloads (sampler holds
the GIL ~1–2 µs per tick; the rest of the tick it sleeps). These are
inherent to event-based vs sampling profiling, not bugs; a Perfetto proto
writer would shrink the trace file but not the callback cost.

**Feasibility, measured over the 10-workload perf set (2026-11-10):**
one X event per generator step makes event traces infeasible on
generator-dense workloads — matmul (N=300) wrote a **3.6 GB** trace for a
1 s run (27M `sum(genexpr)` steps), mandelbrot 2.8 GB, spectralnorm 2.3 GB,
nqueens and mail_parse ~1 GB each. There the flamegraph is the
observation: at 99 Hz its cost is bounded by the run, not the call count.
Event traces stay small and are the right tool on the call-dense class
(ast_parse 6 MB, xml_build 51 MB, decimal_pi 0.4 MB). Second refinement of
the ≈0% line above: on nbody the 500k-iteration hot loop produced **zero**
CALL events — the whole run's B/E count is 11 (the epilogue calls only).
Steady-state calls inside native loops are unobserved by the event
trace; their time shows up in the enclosing native frame's self time
(measurably: ast_parse's 222.9 ms of "main self" is the unobserved
`ast.parse` call, its real work visible only as the 521 ms `C:compile`
slice). The sampling flamegraph sees all of it, because it walks the real
stack. Whether the silent call path is a deliberate direct-call fast path
in codegen is an open question — it is plausibly the same mechanism that
makes the loops fast, and it deserves a dedicated look before anyone
"fixes" the trace.

## 7. Files

| File | Change |
|---|---|
| `compiler/src/rt/trace.cpp` | new — module, C callbacks, NDJSON writer, bootstrap |
| `compiler/src/rt/flame.cpp` | new — sampler thread, walk, SVG writer |
| `compiler/include/pyc/rt/trace.hpp` | new — `pyc_rt_trace_init` / `_end` |
| `compiler/include/pyc/rt/flame.hpp` | new — `pyc_rt_flame_init` / `_end` |
| `compiler/src/rt/entry.cpp` | env dispatch around `body()`, `#ifdef`-guarded |
| `compiler/src/driver.cpp` | parses the flags; adds TUs + defines to the per-build link |
| `.github/workflows/verify.yml` | dormancy + well-formedness gate |
| `rebuild/INTERFACES.md` | v1.5 |

`tools/install-pyc.sh` copies `compiler/src/rt` wholesale, so the new cores
ship with the installed prefix without change. The in-tree `make -C
compiler` is unaffected: the compiler binary never links the runtime; the
driver compiles the rt sources per program build.
