# TRACING — performance tooling for generated code

**Status: design + mechanism de-risked (2026-11-08). Implementation pending A0
sign-off on §8.** A0 request (2026-11-08, the project owner):

- a `--trace` option producing an instrumented binary with the capability to
  emit **Perfetto** traces, and
- a `--flamegraph` option producing an instrumented binary that emits an
  **SVG flamegraph**.

Contract: INTERFACES §5, v1.5 (the two flags). Everything else — §1–§4, the
gates, the baselines — is untouched.

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
   `_PyInterpreterFrame` owning a `PyFrameObject` with the `f_back` chain,
   `f_lineno`, and `instr_ptr`. The *public* C API (`PyEval_GetFrame`,
   `PyFrame_GetBack`, `PyFrame_GetCode`, `PyFrame_GetLineNumber`,
   `co_qualname`, `co_filename`) is sufficient for a stack walk — no internal
   headers, no new ABI.
3. **Per-line updates already exist** (`pyc_rt_set_lasti*`, `rt/frame.cpp`):
   a slot-cached store on the no-tracer fast path (the 9.3%-of-runtime
   symbol in the STATUS speed section) and a line-event dispatch when a
   tracer is present.

Consequence: **neither option changes codegen, lowering, or IR.** Both are
small runtime cores selected at link time, and both ride the monitoring /
frame plumbing above.

## 2. Measured evidence that the mechanism works (2026-11-08)

Before writing this design, the two load-bearing facts were *measured*, not
assumed:

1. **Monitoring events fire on native frames.** A probe program that
   registers a `sys.monitoring` tool under `PROFILER_ID` (the 3.14 API:
   `use_tool_id` / `set_events` / `register_callback`) and then calls its own
   compiled functions produced, **under pyc, a byte-identical event stream to
   the sysroot CPython oracle** — 42 events, same `PY_START` qualnames and
   firstlinenos, same `PY_RETURN`s, same `CALL`s for both Python-level and C
   calls.
2. **LINE events fire.** A 1000-iteration loop produced **2003** LINE events
   under pyc vs the oracle's **2007** (99.8%); program output identical. The
   4-event delta sits in the module-level lines executed in the window around
   the monitoring setup — a tooling count, not a value (I1 not engaged).
   Root-causing it is an implementation-pass item, recorded here so it is a
   known bounded delta, not a surprise.
3. **Perfetto's UI imports Chrome JSON natively** (documented supported
   input: "Chrome JSON format"), so no protobuf dependency is needed in the
   binary.

## 3. Contract (INTERFACES §5, v1.5)

```
pyc <source.py> [-o OUT] [--trace] [--flamegraph]
```

| Flag | Binary gains | Runtime activation | Without env var |
|---|---|---|---|
| `--trace` | tracing core linked in | `PYC_TRACE=<file.json>`; `PYC_TRACE_LINES=1` adds LINE events | byte-identical behaviour to an unflagged binary |
| `--flamegraph` | sampling profiler linked in | `PYC_FLAMEGRAPH=<file.svg>`; `PYC_FLAMEGRAPH_HZ=N` (default 99) | byte-identical behaviour to an unflagged binary |

They may be combined. "Instrumented binary" = the capability is linked;
*activation* is a runtime decision, so one binary serves both clean runs and
measured runs. The exit-0-iff-output rule and the stderr-only diagnostics
rule are unchanged. The dormancy guarantee is a **gated invariant** (§6.1),
not an assertion.

## 4. `--trace`: Perfetto (Chrome-JSON) trace

### Event model

One Perfetto track per OS thread (`pid`/`tid` fields); timestamps are
`CLOCK_MONOTONIC` ns.

| Source (monitoring event) | Emits | Payload |
|---|---|---|
| `PY_START` | `B` slice `qualname` | `args {file, line}` (firstlineno); `cat: "pyc.func"` |
| `PY_RETURN`, `PY_UNWIND` | `E` | closes the slice — exception paths close too |
| `PY_RESUME`, `PY_YIELD` | `B`/`E` pairs | generator/coroutine bodies — see gap (1) below |
| `CALL` + `C_RETURN`/`C_RAISE` | `X` slice `C:<name>` | `cat: "pyc.ccall"` — time spent *outside* generated code (libpython / stdlib C). The STATUS speed analysis says the cycles live here; this is where you see it. |
| `LINE` (only with `PYC_TRACE_LINES=1`) | `i` instant, thread-scoped, `file:line` | |
| init | `M` metadata: `process_name`, `thread_name` | names shown by the UI |

Example (one JSON object per line — the Chrome trace-event format):

```json
{"name":"process_name","ph":"M","pid":4242,"tid":0,"args":{"name":"nbody (pyc -O0)"}}
{"name":"thread_name","ph":"M","pid":4242,"tid":4242,"args":{"name":"main"}}
{"name":"nbody","ph":"B","pid":4242,"tid":4242,"ts":151.234,"cat":"pyc.func","args":{"file":"/x/nbody.py","line":12}}
{"name":"C:sum","ph":"X","pid":4242,"tid":4242,"ts":152.010,"dur":3.4,"cat":"pyc.ccall"}
{"name":"","ph":"E","pid":4242,"tid":4242,"ts":155.881}
```

Open at <https://ui.perfetto.dev>.

### Mechanism

New translation unit `rt/trace.cpp` (linked only when `--trace`):

- An **in-process C extension module** (built with `PyModule_Create`, no
  .so on disk) exposing small C callbacks — `on_start(code, off)`,
  `on_return(code, off, rv)`, `on_call(code, off, callable, arg0)`,
  `on_line(code, line)`, … — matching the documented callback signatures,
  plus `begin(path, lines)` / `end()`.
- A one-time bootstrap (a small literal via `PyRun_String`, or the
  equivalent C monitoring API — decided in the implementation pass; the
  Python level is the documented surface) does
  `use_tool_id(PROFILER_ID, "pyc")`, `set_events(...)` for
  `PY_START|PY_RETURN|PY_UNWIND|CALL` (plus `LINE` and `PY_RESUME|PY_YIELD`
  per the env matrix), and `register_callback` per event with the C
  functions. Per-event cost is then a **C call** (no Python frame) plus one
  buffered JSON append.
- Callbacks run with the GIL held (the monitoring delivery contract), so
  `code` / `co_qualname` / `co_filename` reads are safe. The writer is a
  4 KiB-buffered `FILE*`, flushed at the end of `pyc_rt_main` with an
  `atexit` backstop.
- Hook point: `pyc_rt_main` (`rt/entry.cpp`) — env check after the
  interpreter is ready and before `body()`; `end()` after `body()`.

### Known gaps (bounded; resolved in the implementation pass)

1. **Generator/coroutine resumption.** CPython fires `PY_RESUME` per
   `next()`; a pyc generator's first execution *is* a `next()`, and the
   send path calls `impl(frame)` directly — so generator bodies may produce
   **no** monitoring events today. First task of the implementation pass:
   trace a generator program, inspect, and if missing add the fire in the
   resume path (a few lines).
2. **C slice names** use the callable's `__name__`; lambdas / descriptors
   give `?`. Acceptable, documented.
3. **Stdlib Python functions are not generated code**: they run in
   CPython's eval loop. A call to one from generated code shows as a
   `C:<name>` slice (the boundary event) and its body is invisible — the
   correct scope for a *generated-code* profiler, documented.

### Cost model (measured in §6.4)

- Dormant (no env var): expected ≡ unflagged binary (gated invariant).
- Active: one C callback + buffered append per event. Function-dense
  programs run much slower while tracing — inherent to event-based tracing;
  the low-overhead path is `--flamegraph` (§5).

## 5. `--flamegraph`: sampling profiler → SVG

New translation unit `rt/flame.cpp` (linked only when `--flamegraph`):

- `pyc_rt_flame_init(path, hz)` (same `pyc_rt_main` hook) spawns a
  **detached profiler thread** if and only if the env var is set; without it
  the dormancy cost is exactly one `getenv` at startup.
- Loop: `clock_nanosleep(1/hz)` → `PyGILState_Ensure()` → walk →
  `PyGILState_Release()`. The GIL-protected walk is the same discipline
  CPython's own `sys._current_frames` / faulthandler uses; a signal-based
  (async-signal-safe, no-GIL) walker was considered and rejected for v1 —
  it can read frames mid-teardown.
- Walk (bounded at 256 frames): `PyEval_GetFrame()`, then
  `PyFrame_GetBack` — public C API throughout; records `co_qualname` per
  frame (v1 is function-level; line-level is a later add).
- Aggregation: `std::map<folded-stack, uint64>` — only the profiler thread
  writes it, so no lock.
- On exit (after `body()`, `atexit` backstop): join, then write the SVG.
- **SVG writer** (same TU, no dependencies, ~200 lines): tree from the
  folded stacks; children sorted **by name** (deterministic layout — stable
  across runs; recorded decision); x/width from cumulative sample fractions;
  one `<rect>` + `<title>` (name; samples; percent) per node; colour =
  deterministic hash → the Gregg HSL palette; a header comment carries the
  sample count, unique-stack count, and hz.
- **What a sample captures while the main thread is inside C code that
  released the GIL** (numpy, …): the walk shows the Python frame that made
  the C call — i.e. C time is attributed to its caller, as in any
  Python-level sampler. For the C interior use `--trace` (`C:` slices) or
  `perf`. Documented, not a gap.
- Default rate **99 Hz** (avoids lockstep with 100 Hz timers; the env var
  allows 999 for short runs). Expected overhead ≲ 2% (measured in §6.4).

## 6. Verification plan (I9: no claim without a measurement)

1. **Dormancy (gate, verify.yml static step).** For the smoke program,
   compile plain / `--trace` / `--flamegraph` / both; run all without the env
   vars; stdout/stderr/exit must be **byte-identical** to the plain binary.
2. **Well-formed output (gate, same step).** Smoke under
   `PYC_TRACE=/tmp/t.json`: file exists, parses as a JSON object stream,
   contains ≥1 `M` metadata event and a `B`/`E` pair for the top-level
   function. Under `PYC_FLAMEGRAPH=/tmp/f.svg`: starts with `<svg`, contains
   the top-level function name, sample count > 0.
3. **Correctness gates untouched.** 887/887 and I6 keep running against the
   default (unflagged) build; instrumented builds are a *build variant* and
   are not in the correctness denominator (recorded decision).
4. **Overhead (measured, published in STATUS).** For
   `corpus/programs/{nbody,regex,mandelbrot}.py` at `-O2`: (a) unflagged,
   (b) `--trace` dormant, (c) `--trace` + `PYC_TRACE`, (d) `--flamegraph`
   dormant, (e) `--flamegraph` active @99 Hz. Report ratios; (b) and (d)
   must be within noise of (a).
5. **Visual cross-check.** Import a sample trace into ui.perfetto.dev
   (manual; recorded in the STATUS entry) and compare its per-function
   totals against the flamegraph's sample fractions.
6. **Generator-resume gap (§4 gap 1).** A small generator program; inspect
   the events; fix if missing; re-run the §4 evidence.

## 7. Files

| File | Change |
|---|---|
| `compiler/src/rt/trace.cpp` | new — the module, C callbacks, JSON writer |
| `compiler/src/rt/flame.cpp` | new — profiler thread, SVG writer |
| `compiler/include/pyc/rt/{trace,flame}.hpp` | new — `*_init` / `*_end` C API |
| `compiler/src/rt/entry.cpp` | env dispatch around `body()`, `#ifdef`-guarded |
| `compiler/Makefile` | `WITH_TRACE` / `WITH_FLAME` add the TUs + defines |
| `compiler/src/driver.cpp` | parse the flags; append TUs + defines to the link line |
| `.github/workflows/verify.yml` | §6.1–6.2 static step |
| `rebuild/INTERFACES.md` | v1.5 |

## 8. Open questions (A0)

- **Q1.** Default sample rate 99 Hz — OK?
- **Q2.** `C:` boundary slices on by default under `--trace` (recommend:
  yes — the speed analysis says the cycles live there)?
- **Q3.** Env names `PYC_TRACE` / `PYC_TRACE_LINES` / `PYC_FLAMEGRAPH` /
  `PYC_FLAMEGRAPH_HZ` — OK?

After sign-off: one commit per option, each gated by §6.1–6.2 plus the
existing gates, with STATUS entries carrying the measured numbers.
