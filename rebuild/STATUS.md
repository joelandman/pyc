# pyc — current state and MVP

**Date:** 2026-11-07. Language numbers in the table below are the recorded
`compiler/baseline-language.json` gate (887/887). The `Lib/test` row is the
2026-11-07 I6 re-record (`-O0`, sysroot 3.14.7, `--stdlib`), taken after the
`test_signal` longrunning move (see 2026-11-07 entry below).
CHARTER remains binding. §4 product rules were not changed.

Roles that produced this: Architect (`agents/architect.md`), PM
(`agents/pm.md`), SWE-compiler (`agents/swe-compiler.md`), SWE-runtime
(`agents/swe-runtime.md`).

## Verdict

The rebuild is on the CHARTER architecture: generated AST, `PyObject*` via
libpython, C-API protocols, I1 refusals, I5 differential harness, published
I6. Language + gaps + concurrency is **887/887** impactful. `Lib/test` is
**376/389 = 96.66%** (`-O0`, sysroot 3.14.7, `--stdlib`), last measured
2026-11-07. C1 (frames) and C2 (periodic GIL) are closed. A proved
heap-free loop drops the GIL only when another thread requests it
([GIL.md](GIL.md)). All remaining `EXIT_DIFFERS` are root-caused and
decided (static-libpython inspection deltas, 2026-11-07), refusals are
named and the contract is gated in CI (M6), and the nightly is green
(2026-11-07). The residual non-passes are 9 oracle-unstable coin-flips,
1 oracle-side timeout, and 1 measurement-value line — none a compiler
defect (see the 2026-11-07 entry).
Ordinary functions carry CPython's code object when the local layout
matches (`df32461`); the native body still runs. The eval stub remains
for the module frame, class bodies, comprehensions, and any function
whose locals do not match.

## Measured now

| Check | Result |
|---|---|
| language + gaps + concurrency | 887/887 impactful |
| `Lib/test` (I6) | 376/389 (96.66%), measured 2026-11-07 |
| I6 composition | 375 clean + 1 `STDERR_DIFFERS`-only = 376 pass |
| `DID_NOT_COMPILE` | 0 |
| `EXIT_DIFFERS` | 2 (both decided: `test_sys`, `test_external_inspection`) |
| `STDOUT_DIFFERS` | 9 (8 `ORACLE_UNSTABLE` coin-flips + `test_signal` measurement line) |
| timeout | 1 (`test_runpy`, oracle side) |
| `ORACLE_UNSTABLE` | 9 |
| A1 round-trip | 3.14.7 and 3.13.15 TOTAL (`compiler/README.md`) |
| Tier-1 / NumPy | CI wheel smoke + local wheel smoke via the numpy sysroot copy (2026-11-07); `ldd` has no libpython `DT_NEEDED` |
| C1b / C2 | closed; probes in `language/` |
| `--trace` / `--flamegraph` | gated in verify.yml (dormancy byte-identical, well-formedness); overhead ≈0% dormant and ≈0% active flamegraph (99 Hz); active trace ≈10× function-dense — TRACING.md §6, 2026-11-08 |
| real-world perf (smoke, `-O2`) | baseline 2026-11-09: numeric **1.31–1.35×**, pure-python **1.10–1.12×**, c-bound **0.98×** vs CPython 3.14.7 — "Real-world performance" section below |
| sysroot relocatability | launcher RUNPATH `[&lt;build-prefix&gt;/lib : $ORIGIN/../lib]`; moved-tree bare run reports the tree's own 3.14.7 on a box carrying two OTHER libpythons (2026-11-10, hal9k) |

I6 is file-as-script vs sysroot 3.14.7 with `--stdlib` (unittest can
import `test.support`). Do not treat unittest MATCH on a single file as
the I6 row. The 2026-08-27 snapshot (246/389, 24 DNC) is replaced:
`--stdlib` was missing locally, so many files imported and exited 0
without running tests. Net +26 (67 newly passing, 41 now fail).

2026-11-01: `test_tracemalloc` now passes (51/51) after the closure-cell
fix (`pyc_rt_cell_new`, cell allocated while the frame reads as incomplete
so tracemalloc attributes it to the caller).

2026-11-01: I6 re-recorded to **367/389 (94.34%)**. The previous record
(369) was no longer reproducible in this environment: `test_xpickle`'s
backwards-compat worker now resolves a CPython 3.11 that is on PATH
(uv-managed, plain CPython), which lacks the pyc-only `__pyc_eval__`
builtin, so the 12 `test_builtin_functions` subtests fail deterministically.
That delta is pre-existing, not a regression from recent work: the
compiler at the previous record's own commit fails the same way here, and
CI (no such worker on PATH) still measures 369. `test_logging` was
`ORACLE_UNSTABLE` in the recording run (CPython disagreed with itself; not
a pyc fault) -- on a run where it is stable the row is 368/389 in this
environment. `test_fork1` (chronically flaky) was stable in this run.

2026-11-02: I6 re-recorded to **375/389 (96.40%)** (`f3ff9e6`); the header
row above predates it. test_bdb, test_call, test_dis, test_opcache,
test_peepholer, test_sys_settrace, test_type_cache, test_xpickle now pass.

2026-11-03: CI reliability restored. Every push since 2026-10-11 and every
scheduled run since 2026-10-30 had been red for reasons stacked on each
other, so red no longer signalled anything:
1. `lower.cpp:2780` narrowed an `int` slot into `Instr`'s `uint32_t`
   (added 10-30 in `e1ee480`) -- a hard error for clang++-22 (CI), and
   every workflow died in "Set up pyc" before verifying anything.
2. `compiler/Makefile`'s `CXX ?= clang++` was dead: make's builtin
   `CXX = g++` is already defined, so local builds silently used g++,
   which demotes the same construct to a warning. Local green / CI red,
   unforecastable. The Makefile now assigns CXX for real (clang++-22 when
   present). A push must be built with the CI toolchain locally before it
   leaves the machine.
3. `check_ir_wellformed.py` had drifted from the IR format: the new
   `; loc_err -> bbN` tail on `decref`/`ret` made normal-path releases
   invisible (false "leaks on the normal path" on nearly every
   module-level class), and a phi's hand-off was not recorded as a
   release site (the and/or join read as a leak). The Oct-11/29
   `boolop_once` finding was a real leak in the OLD class machinery,
   superseded by the 11-02 class-body rework; the corpus is now
   1035/1035 well-formed. The checker also runs the parse oracle with
   `LD_LIBRARY_PATH` stripped (a 3.14.5 libpython on the path had
   silently swapped the 3.14.7 parse oracle; classdict_comp.py parsed
   on CI and "failed to parse" locally).
4. The apt.llvm.org fetch is retried: one network blip (11-03 14:34 UTC)
   red both scheduled workflows with the code green.
 Measured, not assumed: after the fixes the full verify pipeline was
 reproduced locally (clang++-22 build, static checks, smoke, 887-case
 differential + gate, all green), and the pushed run is green on GitHub:
 static `well-formed 1035 malformed 0`, differential **887/887**, verdict
 "GATE RAN — no regression" (`206606b`).

## Real-world performance

CHARTER §4's bar — "measurable speedup over CPython on a typed numeric
benchmark" — measured the way this project measures everything else: real
programs, pinned deterministic inputs, the sysroot CPython as oracle
(I5), a published record. This section is the published home for it (A0
decision, 2026-11-08: no separate PERF.md).

**Harness** — `verify/perf_run.py` (record schema `pyc-perf/1`), a sibling
of the differential harness, which deliberately discards timing. Per
workload: one pyc build at the pinned flags (default `-O2`), 1 warmup +
7 timed runs per side, alternated, median with p25–p75, wall clock
around the whole process (startup included, symmetric on both sides).
Both sides run under the same pinned `child_env`; the oracle is the
sysroot CPython the binary embeds. The harness cross-checks pyc against
the oracle per workload (exit + stdout): a workload that disagrees is
flagged MISMATCH, because its timing is a coin flip (I9). Every workload
also ships one `--trace` and one `--flamegraph` artifact (TRACING.md),
from a separate instrumented build that is not in the timed set.

**Workloads** — `verify/perf.py` (registry) plus `verify/perf/workloads/`
and three corpus programs (nbody, mandelbrot). Three classes:
`numeric` (type-stable float/int — where unboxing is expected to pay),
`pure-python` (call/combinatorial density — where native dispatch should
win), `c-bound` (C work dominates — the honest floor). Every input is a
pinned constant or an in-memory construction from fixed patterns. The pin
covers name, path, argv, and the workload FILE's content, so an edit to a
pinned constant (N=500 → 600) changes the pin and the comparison is
refused until the baseline is re-recorded with a reason. Measured on this box
(32 cores): smoke set 2–3 min including the artifact builds; the full
set adds ~1 min of work. The 2-core CI runner runs both about twice as
slow.

**Comparability** — two-level key (`verify/perf_compare.py`). Record
level: oracle version and pyc flags must agree. Workload level: every
name in the intersection must be pin-identical (path, argv, file
content) — so the nightly full set compares against the smoke baseline,
and the intersection is the comparison. Anything else is refused (exit
2) rather than compared, because the comparison would measure two
different things. The host is deliberately not in the key: a speedup
measured side-by-side on one machine is the machine-invariant quantity,
and the two recorded runs agree on it within ±5% while their absolute
seconds diverge up to 49% (below). The baseline is
`verify/perf/baseline.json`; it is re-recorded only with a measured
reason in the commit, like the correctness baselines.

**Policy** — PR: the smoke set runs on every push, trend-only. Nightly:
the full set in `metric.yml` (one build class with the PR smoke: pgo off,
`-O2`). Until run-to-run variance is characterised, drift beyond ±15% is
a red warning, not a failure; the timing gate arrives with the
characterised variance, not before. Harness errors are still hard
failures in both: a workload that does not compile, mismatches the
oracle, or times out entirely, and a record that is not comparable, both
fail the job.

**First record** (baseline pin `8227920d1e85c6af`, 2026-11-09, local box
`hal9k`, sysroot 3.14.7, pyc `-O2`, 1 warmup + 7 runs; re-recorded
2026-11-09 because the pin was extended to cover workload file
content — see the 2026-11-09 entry):

| Workload | Class | CPython (s) | pyc (s) | Speedup |
|---|---|---:|---:|---:|
| nbody (500000) | numeric | 1.772 | 1.349 | **1.31×** |
| spectralnorm (N=500) | numeric | 1.516 | 1.123 | **1.35×** |
| ast_parse (400 classes) | pure-python | 0.981 | 0.883 | 1.11× |
| xml_build (200k rows) | pure-python | 0.351 | 0.319 | 1.10× |
| decimal_pi (10000 digits) | c-bound | 0.321 | 0.327 | 0.98× |
| nqueens (N=13) | pure-python | 0.806 | 0.716 | 1.12× |

The class picture is the shape the 2026-10-02 speed section predicted:
type-stable numeric is where the native body wins (1.33–1.34×),
call-dense pure Python wins smaller (1.10–1.15×), and C-bound is the
floor (0.98× — the four C-bound `Lib/test` files of 2026-10-02 are the
same class measured at per-line granularity, where the overhead a
dedicated workload amortises shows as 1.9–2.4×). The direction of CHARTER
§4 is confirmed in measurement; the attribution is not yet proven —
whether the numeric win is unboxing or simply the absent eval loop is
what the per-workload flamegraph artifacts exist to answer (plan
question A, "where do the cycles live").

**Measured variance** (two smoke runs, same machine, minutes apart,
2026-11-08): absolute medians moved up to +49% between runs (machine
load); the speedups moved within ±5% (1.33→1.31, 1.34→1.34,
1.10→1.08, 1.11→1.06, 0.98→0.97, 1.15→1.15). The speedup is the quantity
a trend line can be built on; the absolute seconds are machine-local,
which is why cross-host comparisons in `perf_compare.py` alert on
speedups only.

## Major remaining issues

Ranked by Architect and SWE-compiler. As of 2026-11-07 every remaining
`EXIT_DIFFERS` is root-caused and decided (see the 2026-11-07 entry); the
open items below are the eval-stub residuals, the honest refusals, and
process items. Dump artefacts before adding syntax — that remains the rule
for any NEW difference.

### P0 — correctness of what we already accept

**Closed 2026-09-10** (measured on HEAD vs sysroot 3.14.7; `baseline-libtest.json`
is stale, 2026-08-27).

1. **Malformed IR.** `test_super.py`, `test_tokenize.py`, `test__interpreters.py`
   all compile on HEAD (`llvm-as-22` / `opt-22 -passes=verify` clean). SSA
   fixes after the baseline (`a979c32`, `185304e`, …) closed this. Remaining
   `test_super` diffs are unittest failures, not invalid IR.
2. **`test_unpack` exit 0 vs 1.** Both sides now exit 1. The old subject-0
   was `DocTestSuite()` never running.
3. **Subject exit 5.** Not SIGTRAP. unittest's "NO TESTS RAN" is exit 5.
   Cause: `sys._getframemodulename` returned None (`f_funcobj` was
   `PyStackRef_None`; doctest does not fall back to `f_globals['__name__']`)
   and `__main__.__doc__` was unset, so module doctests were empty.
   After the frame-func + module-doc fixes, exit matches CPython on
   `test_unpack`, `test_unpack_ex`, `test_extcall`, `test_genexps`,
   `test_pep646_syntax`, `test_descrtut`, `test_metaclass`.

Probes: `verify/corpus/language/getframemodulename.py`,
`getframemodulename_rebind.py`, `doctest_suite.py`, `module_doc.py`.

### P1 — completeness without silent wrong answers

4. **`EXIT_DIFFERS`.** **Closed 2026-11-07.** Every file named below passes
   in the 376/389 baseline, and the two surviving `EXIT_DIFFERS` files
   (`test_sys`, `test_external_inspection`) are decided design deltas —
   see the 2026-11-07 entry. The paragraph is kept as the measured record
   of how the cluster closed. `test_super` **40/40** (3 skipped);
   `test_copy` **81/81**; `test_funcattrs` **35/35**; `test_genericclass`
   **22/22**; `test_bytes` **319/319** (8 skip); `test_dict` **121/121**;
    `test_complex` **37/37**; `test_format` **18/18**; `test_dynamic` **11/11**;
    `test_decorators` **16/16**; `test_builtin` **147/147** (6 skip);
    `test_functools` **325/325**; `test_scope` **41/41**; `test_binop` **12/12**;
    `test_named_expressions` **74/74**. `f(*args)` keeps the tuple identity
     for `tp_call`; `global __x` mangles like CPython. `test_call` 185/186
    (3 skip): `_testinternalcapi` loads via `--whole-archive`; `test_super_deep`
    still overflows 8MB C stack at ~17k -O0 frames. Yield/async marshal MATCH:
    `test_generators` 59/59, `test_asyncgen` 85/85, `test_yield_from` 43/43,
    `test_genexps` 1/1, `test_generator_stop` 2/2; genexp `__qualname__` repaired.
    `test_coroutines` **99/99**. Dumped EXIT_DIFFERS
    Former I6 `DID_NOT_COMPILE` (24 files) all **compile on HEAD** (t-strings,
    `except*`, type aliases, kw-only lambdas, genexp/genfunc marshal).
    leftovers at the time — all since closed: `test_super_deep` now skips
    on both sides via CPython's own `@skip_if_huge_c_stack(90_000)`
    (test_call passes in full); `test_compile` and `test_dis` closed with
    the code-object work behind `f3ff9e6` (see the 2026-11-07 entry).
    `test_raise` **37/37**; `test_gc` get_objects/heap_size;
    `test_str` nomemory vs `with`
    GetAttr. `test_unpack`/`extcall` exit 1 both sides as `__main__`.
    MATCH: `test_iter` 57; `test_with` 54; `test_listcomps` 66;
    `test_dictcomps` 10; `test_setcomps` 2; `test_tuple` 38;
    `test_exception_group` 52; `test_property` subclass `__doc__`.
 5. **Honest I1 refusals** still in `lower.cpp` (if they reach native
    lowering): `star-unpacking` (parser SyntaxError for star-as-expr;
    call/list/set unpack runs), `starred assignment` (sole `*a =` is
    CPython SyntaxError; `a, *b, c =` is implemented), `yield` / `await` /
    `async for` / `async with` (backstops: function-level yield/async marshals,
    so these arms do not fire on valid 3.14). Comprehension `for` targets now
    include Name/tuple/list and attribute/subscript. Star-as-annotation is
    `typing.Unpack`; `__annotate__(format>2)` is NotImplementedError. Async
    comprehensions in `async def` marshal; in a sync function they are a
    parse SyntaxError.
6. **Compiled callables are `PyFunction`.** `PyFunction_New` +
   `PyFunction_SetVectorcall` onto the native trampoline. `PyFunction_Check`,
   `inspect.signature`, `__closure__`, `__annotate__`, `__name__`/`__qualname__`
   all MATCH. `type(f)(f.__code__, ns)` reuses the trampoline via a function
   watcher. Empty cellvar is UnboundLocalError; empty freevar is NameError.
   `exec(f.__code__, closure=...)` runs the native body through the
   `__pyc_eval__` stub-bytecode helper installed into builtins. When the
   local layout matches, the function's own CPython code object is
   installed instead of that stub (`df32461`); `exec` of a replaced
   closure still finds the native body by the code-object tail.
   `pyc_rt_set_lasti` stores the code unit for the current source span
   so traceback carets read `tb_lasti` back through `co_positions`
   (`17270a0`, `adc0c4e`). LoadGlobal
   uses the current frame's globals (and mapping `__getitem__`), so
   FORWARDREF annotate reconstruction works. Nested `__annotate__` code objects from
   CPython's compile sit in the outer `co_consts`. Unexpected keywords
   offer a "Did you mean" suggestion.
7. **I8 CLI — done (S1 + S5).** `pyc` finds the sysroot interpreter
   (manifest or `bin/python3`), `--python-sysroot` aliases `--sysroot`,
   `--python`/`-std`/`--python-abi`/`--list-python-targets` work,
   `--fetch-sysroot` installs the published tarball, and `install-pyc.sh`
   lays out the prefix (`bin/pyc`, `lib/pyc/`, `sysroots/<abi>-<ver>-tier1/`).
   `-std` is `--feature-version`. One CPython artifact is enough; a missing
   version is an exit-2 error, not a fallback. See workstream S below.
8. **Lock-file scale.** `lower.cpp` 8155 lines (2026-11-07). Unboxing has
   landed (`UNBOXING.md`, all 16 staged steps).
 9. **Sync-comp loop var lives in the comp frame, not the enclosing one.**
    1B (`b22c9d4`) makes the comp's loop var a local of the enclosing function
    (so a comp-bearing function adopts its real inlined code object), but the
    native impl still runs the comp as its own frame — the loop var's VALUE is
    set in the comp frame, leaving the enclosing function's slot NULL. CPython's
    inlined comp sets the enclosing slot to the last value. This is invisible to
    normal programs (the loop var is name-unbound either way, so a later `i` is a
    NameError on both) and to the warmup path (a once-called function runs the
    adopted code object in ceval, which does set the slot). It only diverges in
    the debugger's unbound-locals count for a comp function called 2+ times
    (after warmup closes and the native impl is used): pyc counts the slot as
    unbound, CPython does not. Closing it requires the native impl to run the
    comp inline (full PEP 709 inlining), not merely adopt the code object.

### P2 — product / process

10. **I6 republished 2026-11-07.** `compiler/baseline-libtest.json` is
    376/389 vs sysroot 3.14.7, `-O0`, recorded under CI's interpreter set
    (see the 2026-11-07 entry). `DID_NOT_COMPILE` is 0. The 2026-09-26
    355/389 and 2026-11-01/02 records are superseded history.
11. **Developer sysroot — done (workstream S, S0–S5).** No local CPython
    *build* is required: `sysroot.yml` publishes the Tier-1 tarball,
    `pyc --fetch-sysroot` / `PYC_FETCH=1` install it beside the prefix, and
    `install-pyc.sh` lays out `bin/pyc`, `lib/pyc/`, `sysroots/<abi>-<ver>-tier1/`.
    A developer changing A2 headers still rebuilds the sysroot.
12. **Output home.** `PyConfig.home` is set from `PYTHONHOME`, then
     `lib/pythonX.Y` next to (or above) the executable, then the compile-time
     sysroot. Prefix is no longer implicit-only.
13. **A2 leak bar.** Measured nightly (2026-10-28): the `leak` job in
    `metric.yml` builds a cp314d (`Py_REF_DEBUG`) sysroot and runs
    `make -C compiler refcount-probe`; the workloads are flat per
    iteration and the negative-control `leak` workload leaks exactly
    +1/iter, which proves the harness can fail. A release sysroot exits
    2 with a diagnostic instead of passing silently.

### Closed — do not re-open

C1b frames, C2 `_Py_HandlePending`, comprehension cells, nested-class
closures, alloca hoist, I3 (no method-name chains), I4 generated AST +
generic-arm lint, I5/I5a, I7 NumPy in CI. Unboxing stays behind proofs.

## MVP

CHARTER product: native, deployable, CPython-correct, approaching real
Python. Completeness before speed. CHARTER §4 is **v1**, not MVP.

**MVP means:** a real program compiles to one Tier-1 binary that matches
CPython on what it accepts, is easy to copy (`ldd`: no libpython, `dlopen`
works), and completeness is measured and not gamed. Not: C speed, two
`--python=` targets, Tier 2, native generators, unittest-driven `Lib/test`.

| # | Check | Bar |
|---|---|---|
| M1 | `make -C verify verify` | 887/887 impactful; no new silent-wrong |
| M2 | `make -C verify fast` | `--fail-on-silent-wrong` |
| M3 | CI `ldd` smoke | no `libpython` `DT_NEEDED` |
| M4 | CI wheel step | NumPy import+run in that binary |
| M5 | I6 vs README | **376/389 (96.66%)**, no regress |
| M6 | remaining compile fails | construct + line + reason — **not** invalid LLVM IR |

**Not MVP gates:** unittest metric; `--python=3.13`; ELF `-static`; unboxing
vs C.

## Next steps

**Now**

- 2026-11-10:
  (1) **First survey pass: where the cycles live (plan question A),
  measured over the 10-workload perf set** (`verify/perf_analyze.py`,
  new; parses the trace windows + flamegraph frames side by side so a
  disagreement between the two independent measurements is visible).
  Findings, all measured today:
  a. **Generator expressions are the measured codegen gap.** matmul's
     flamegraph: `main.<locals>.<genexpr>` is **94.7% of all self
     samples**; pidigits' trace: the genexpr's self time is **74.0%** of
     the run (1016 ms of 1373 ms; the actual bigint work is the `spigot`
     steps at 20.3%). Both workloads trail their loop-only siblings
     (matmul 1.14× vs nbody/spectralnorm 1.34–1.35×) precisely because
     the inner loop is a lazy genexpr running as CPython bytecode — 3.14
     inlined list comprehensions but not genexprs. Compiling a
     genexpr-plus-known-consumer (`sum`/`join`/`for`/`any`/`all`) as one
     native loop is the top codegen target; it is also the most common
     real-world idiom.
  b. **The native-loop win is real and near the floor already**: nbody
     `advance` 100% self (1.35×), spectralnorm mul/at/mat ~90% (1.34×),
     mandelbrot main 98.8% (1.20×); decimal_pi 1.00× with 8% in
     `C:sqrt` is the c-bound floor behaving as predicted (2026-10-02
     section, confirmed).
  c. **Event traces are infeasible on generator-dense workloads**: one X
     event per generator step → matmul 3.6 GB / mandelbrot 2.8 GB /
     spectralnorm 2.3 GB / nqueens+mail_parse ~1 GB for ~1 s runs.
     Flamegraph is the observation there; event traces stay right for
     the call-dense class (ast_parse 6 MB, xml_build 51 MB). Recorded in
     TRACING.md §6.
  d. **The trace is blind to native hot loops**: nbody's 500k-iteration
     loop produced zero CALL events (11 B/E events for the whole run);
     ast_parse's `ast.parse` call from native code produced no frame, so
     its 222.9 ms lands in "main self". Whether that is a deliberate
     direct-call fast path in codegen is open — plausibly the same
     mechanism that makes the loops fast; investigate before touching
     the trace.
  e. C: attribution on the call-dense class (the target list below):
     ast_parse is **66.2% in `C:compile`** (the C compiler is the
     program), xml_build 27.3% `C:feed` (expat) + 18.5% `C:sum`,
     decimal_pi 8.0% `C:sqrt`.
  Survey of *real programs in use*: still not done — the perf set is ten
  hand-picked benchmark shapes, and the `real-app` class in `perf.py`
  is declared but empty. Proposal on the table: two pinned third-party
  workloads (`black --check` / `mypy --fast` on a pinned corpus, both
  pure-Python, installed into a sysroot copy per the numpy-copy
  precedent), run through the same harness + analyzer.
  (2) **`install.sh`: one-command installer for the compiler + sysroot
  prefix, with `--dryrun` and `--uninstall`** (repo root; composes
  `tools/install-pyc.sh` + `tools/install-sysroot.sh`, then a differential
  smoke that compiles with `$PREFIX/bin/pyc` and NO `PYC_SYSROOT` — the
  prefix's own `sysroots/` must resolve — and compares the output with the
  sysroot's own CPython). Uninstall is marker-guarded
  (`lib/pyc/pyc-prefix.json`), removes only pyc-owned paths, and never
  follows the sysroot symlink. Verified end to end locally: symlink-sysroot
  install + reinstall-over + uninstall (target left intact, empty dirs
  pruned), all dry-run plans.
  Measured while testing the download path: **the published sysroot is not
  ELF-relocatable.** Its `bin/python3.14` carries an absolute RUNPATH to
  the CI build path (`/home/runner/opt/py-sysroots/cp314-3.14.7-tier1/lib`);
  the asset is self-consistent (launcher + its own lib report 3.14.7,
  matching the manifest), but on a machine without that path the loader
  falls back to a system `libpython3.14.so.1.0` (hal9k has 3.14.4 in
  /usr/lib) — so the interpreter silently runs on a DIFFERENT CPython than
  the one the binary links (I5 exposure), and `install-sysroot.sh`'s
  identity check was fooled by the fallback (it refused a good asset:
  "manifest claims 3.14.7, interpreter is 3.14.4"). The driver's parse
  subprocess (LD_LIBRARY_PATH stripped, RUNPATH broken) has the same
  exposure on such machines.
  (3) **Sysroot relocatability fixed — the three steps, measured.**
   1. `build-python-sysroot.sh` now appends
      `LDFLAGS += -Wl,-rpath,'$$ORIGIN/../lib'` to the generated Makefile
      after configure. It cannot ride configure's `LDFLAGS` or a make
      command-line variable: the token must survive script shell → make
      input → recipe shell, and every other route measured today failed
      (env LDFLAGS → RUNPATH `/../lib`; make cmdline `$$ORIGIN` → `/../lib`
      because make re-expands command-line values once; `$$$$ORIGIN` →
      `/&lt;pid&gt;/../lib` because the recipe shell reads `$$` as its PID).
      In the Makefile, make's `$$`→`$` plus single quotes is one auditable
      layer. The launcher now carries BOTH the absolute `$PREFIX/lib` entry
      (build machine) and `$ORIGIN/../lib` (any machine).
   2. `install-sysroot.sh` verify runs the interpreter with
      `LD_LIBRARY_PATH` scrubbed AND pinned to the tree's own `lib/`
      (scrub alone was the hole), checks the manifest's `libpython_shared`
      actually sits inside the tree, and WARNS (not dies) when the bare
      launcher cannot run on this machine (pre-2026-11-10 tree).
   3. `driver.cpp` pins `LD_LIBRARY_PATH=&lt;sysroot&gt;/lib` for ALL oracle
      subprocesses (the `oracle_env` helper: xy/abi probes, parse,
      `python3-config --includes`, the two sysconfig lookups) — not just
      parse. Pinning, not scrubbing, is what keeps a hostile or merely
      loaded host environment (conda, `/home/.../local` carrying its own
      libpython3.14) from swapping the oracle; measured today, hal9k's
      ambient `LD_LIBRARY_PATH` shadows even a perfectly healthy tree.
   Measured proof on hal9k (2026-11-10): a `--no-pgo` local build → tree
   moved to a new path → bare run (host env stripped) reports **3.14.7**
   (pre-fix the same exercise reported the host's 3.14.4/3.14.5),
   `factorial(25)` correct, `ssl`/`sqlite3` dynload extensions resolve
   libpython through the executable's `$ORIGIN` RUNPATH; RUNPATH is
   `[&lt;build-prefix&gt;/lib:$ORIGIN/../lib]`. Full `install.sh` flow green
   for both sysroot modes (copy + symlink) with the smoke comparing against
   the pinned oracle; `--uninstall` re-verified for both.
   Latent bugs the new checks caught and fixed: (a) `build-python-sysroot.sh`
   `fetch_source`'s "tree already present" shortcut skipped the
   `patchlevel.h` version check (only ran after fresh extract) — a mislabeled
   tree would compile for minutes and fail only at the manifest stamp; (b)
   `install.sh`'s local `rewrite_manifest` swallowed the path separator
   (`…/prefix` + `lib/…` → `…/prefixlib/…`) and left the `sysroot` field
   stale; (c) `install.sh` named the `--sysroot DIR` destination after the
   directory's basename instead of the manifest's `archive_name`, so the
   driver's `sysroots/` auto-search (pattern `cp*-tier*`) would never have
   found it — the smoke would have tested nothing; (d) the smoke's own
   oracle was scrubbed but not pinned (subject and oracle could diverge on
   pre-fix trees).
   Outstanding: the PUBLISHED asset is still the pre-fix tree until the next
   sysroot nightly republishes it; after that, re-run `./install.sh --prefix
   &lt;tmp&gt;` (download path) to confirm the identity check passes end to end.
  `build-python-sysroot.sh` (+ verify against the tree's own lib).
- 2026-11-09:
  (1) **First nightly perf run: the comparison contract had a hole; fixed
  and the baseline re-recorded.** The 06:00 metric run measured the full
  set correctly (10/10 workloads OK, artifacts produced, 329 s) but
  `perf_compare` refused the comparison: the pin hash was per-SET, and
  the baseline is a smoke record while the nightly runs full — so the
  cross-set comparison could never match (the local tests only ever
  compared smoke against smoke). The hole was worse than that: the pin
  covered name+path+argv, not the workload file, so editing a pinned
  constant (N=500 → 600) would have kept the key and silently compared
  different amounts of work. Fixed: the pin now includes the file
  content per workload (`content_sha`), and comparability is two-level —
  record level (oracle version, pyc flags) plus per-workload pin identity
  over the intersection, which is what makes full-vs-smoke-baseline
  legal (verified locally: exit 0, the 4 full-only workloads listed as
  "no baseline"). Baseline re-recorded under the new key (pin
  `8227920d1e85c6af`; reason in the commit, per the re-record rule).
  Second defect in the same run: `upload-artifact@v4` only takes
  workspace-relative paths, so the absolute `$RUNNER_TEMP` artifact dir
  was dropped with a warning and the shipped artifact held the record but
  not a single trace — the artifact dirs now live in the workspace.
  Runner numbers for the record (4-core, first full-set run): numeric
  1.26–1.35×, pure-python 1.08–1.18×, c-bound 1.00× — within ±5% of the
  local box on every shared workload, which is the speedup-stability
  claim holding across machines for the first time.
  (2) **I6 at 368/389 (94.60%) in the same nightly — not a regression.**
  Case-level against the 376 baseline: 10 cases newly non-pass, ALL
  `ORACLE_UNSTABLE` (CPython disagreed with itself that run: 5
  `test_codecmaps_*`, `test_hashlib`, `test_logging`, `test_ucn`,
  `test_unicodedata`, `test_zipfile64` — coin flips, no ground truth, the
  documented jitter of 2026-11-01), and 2 cases fixed (`test_signal`'s
  measurement line now clean, `test_fork1`'s flake gone). The gate ran
  and passed: no reproducible difference appeared. The published
  baseline stays at 376; re-recording to a coin-flip number is exactly
  what the re-record rule forbids.
- 2026-11-08:
  (1) **`coverage.py` retired.** The local tool that measured "what fraction
  of the corpus lowers" was removed (`git rm compiler/tools/coverage.py`).
  Its one output had become a constant: its own parse+lower logic reports
  **887/887 (100.0%), zero blockers** on the gated corpus set (language +
  known-gaps + concurrency), and no CI step, Makefile target, or doc
  referenced it. Its self-check is structurally incompatible with the
  project's goal — it proves it can detect a gap by finding one, so a gap
  must exist (its hardcoded probe construct had gone stale seven times
  before; at 100% coverage it fails permanently, which is the guard working,
  not a bug). The measurement discipline it embodied ("prove you can produce
  both answers before reporting") survives in `selfcheck.py`, which the
  CI-gated `check_ir_wellformed.py` uses on every push. The `pyc --lower`
  door it used is kept: `check_ir_wellformed.py` still drives it
  (INTERFACES v1.4, no contract change).
  (2) **`--trace` / `--flamegraph` implemented (A0 request, 2026-11-08).**
  Full design + measurements in `rebuild/TRACING.md`; INTERFACES v1.5 adds
  the two flags. The load-bearing facts were measured before the first line
  was written: (a) a `sys.monitoring` tool under `PROFILER_ID` receives a
  **byte-identical event stream under pyc vs the sysroot oracle** for
  PY_START/PY_RETURN/CALL on compiled functions (42 events, same qualnames
  and lines) — the runtime's existing `fire_monitor` sites are the tracer's
  event source, so **no codegen or lowering changes**; (b) generator bodies
  are real CPython bytecode (`lower.cpp`), so their events fire natively —
  a 13-event probe is byte-identical to the oracle, closing the design's
  one open gap; (c) the flamegraph walker needs only public C API
  (`_PyThread_CurrentFrames` / `PyFrame_GetBack` / `f_code`).
  Implementation findings, all measured: five 3.14 API divergences from the
  C header / 3.13 docs (event numbering, tool-as-name, one-event
  callbacks, non-importable `sys.monitoring`, opaque GILState handle);
  two concurrency bugs found on the reproducer and fixed — accumulated
  sleep deadlines (→ GIL ping-pong, >100× slowdown) and join-with-GIL
  deadlock (~1 in 3 runs hung; 10/10 after the fix). Result: dormancy is
  byte-identical and now a verify.yml gate; overhead measured at ≈0% for
  both flags dormant and for the active flamegraph (99 Hz), ≈10× for the
  active trace on a function-dense workload (≈0% on arithmetic-dense —
  event-based, per TRACING.md §6). A0 signed off 99 Hz, default `C:`
  slices, and the env names.
  (3) **Real-world performance harness (Phase 0 of the measurement plan;
  A0 decisions: workload set as proposed; PR timing smoke ON for trend
  tracking; the published home is this file — no separate PERF.md).**
  `verify/perf.py` (registry: 6 smoke + 4 nightly workloads, three classes,
  pin hash) + `verify/perf/workloads/` (7 new deterministic programs) +
  `verify/perf_run.py` (1 warmup + 7 timed runs per side, median/p25–p75,
  wall clock around the whole process, same pinned env both sides, I5
  oracle, per-workload trace + flamegraph artifacts, MISMATCH cross-check)
  + `verify/perf_compare.py` (comparability keyed on pin hash + oracle
  version + flags; drift beyond ±15% is a warning, not a gate, until
  variance is characterised). First record committed as
  `verify/perf/baseline.json` (2026-11-08, local box, sysroot 3.14.7,
  `-O2`): numeric 1.33–1.34×, pure-python 1.10–1.15×, c-bound 0.98× — the
  class shape the 2026-10-02 speed section predicted. Two-run variance
  measured the same day: absolute seconds up to +49%, speedups within
  ±5% — the speedup is the trend quantity. CI: verify.yml runs the smoke
  set on every push after the gate (hard-fails only on harness errors:
  workload failure or incomparable records); metric.yml gets a nightly
  `perf` job (full set, same build class: pgo off, `-O2`). One bug found
  and fixed on the first `make perf` run: an argparse `append` list
  default silently doubled `-O2`, which the comparability key correctly
  refused — the harness working as designed.
- 2026-11-07: pre-MVP blocker pass (B1–B6), all measured:
  (1) **B1 decided — `test_external_inspection.py` accepted as a design
  delta.** Root cause: its `test_self_trace` runs `RemoteUnwinder(os.getpid())`,
  which scans `/proc/<pid>/maps` for a `PyRuntime` section; a Tier-1 binary
  statically links libpython, so the section does not exist (reproduced
  locally: `RuntimeError: Failed to find the PyRuntime section`, exit 1 vs
  0, other 8 tests pass). Same defect class as the already-accepted
  `test_sys` `remote_exec`-on-self delta (2026-10-31). No code change.
  (2) **B2 decided — `test_runpy.py` accepted and documented.** It is the
  *oracle* that does not finish within the 600s longrunning budget on the
  4-core runner (baseline record: `oracle_timed_out`, subject exit 0).
  I5a: a statement about the limit, never a pyc fault; still counted against
  the rate.
  (3) **B3 — `test_signal.py` added to the longrunning group**
  (`verify/longrunning.py`). Its ITIMER/signal tests are wall-clock bound:
  ~67s under CPython and ~66s under pyc on this 32-core box (measured
  2026-11-07, both exit 0, `OK (skipped=4)`), which straddles the 30s + 60s
  budget on the 4-core runner. Not a hang.
  (4) **B4 — the I6 ceiling is now stated.** Of the 14 non-passing files:
  2 are the accepted static-libpython inspection deltas (above, plus
  `test_sys`), 2 are wall-clock budget artifacts (`test_runpy` oracle side;
  `test_signal` fixed in (3)), and 10 are `ORACLE_UNSTABLE` programs that
  print a coin flip — pids, ports, unseeded `random`, temp paths, thread
  order (`test_fork1`, `test_imaplib`, `test_logging`, `test_pty`,
  `test_socketserver`, `test_sort`, `test_ssl`, `test_thread`,
  `test_threading`, `test_zipimport_support`). I5a counts those against the
  rate on purpose; they cannot be compiler-fixed without violating the
  contract, so the published number has a ceiling below 100%.
  (5) **B5 verified — release publishing is healthy.** `pack-pyc` and
  `metric` nightlies green four consecutive days (2026-11-04…07) and both
  assets re-uploaded 2026-11-07 (`pyc-linux-x86_64.tar.xz` 626 KB,
  `cp314-3.14.7-tier1-x86_64.tar.xz` 84 MB). The S5 "publish step failing"
  note is closed.
  (6) **B6 — the M4 wheel milestone is provable locally.** A numpy-carrying
  copy of the sysroot now lives at
  `$HOME/opt/py-sysroots/cp314-3.14.7-tier1-numpy` (numpy 2.5.4; the primary
  sysroot stays pristine; the copy's `pyc-sysroot.json` paths are rewritten
  and the copied interpreter resolves its own prefix). Measured:
  `numpy.arange(10).sum()` compiles against that sysroot, the Tier-1 binary
  prints 45 = oracle, `ldd` shows no libpython dependency. Note for future
  readers: run the sysroot's python with a scrubbed `LD_LIBRARY_PATH` —
  this shell's 3.14.5 libpython on the path outranks the sysroot's RUNPATH,
  and even the *primary* sysroot fails `import array` under it (the harness
  `child_env()` already strips it; the gates were never affected).
  (7) **M6 contract gated:** `check_diagnostics.py` now runs in
  `verify.yml` static checks (DIAGNOSED on HEAD before wiring): a refused
  compile must name file:line:col and the kind, and the checker proves it
  can fail.
  (8) **`compiler/Makefile` no longer reads a directory as "up to date":**
  `/tmp/pyc_lower` was a stale empty directory, `make` reported "Nothing to
  be done", and a gate would have run on a no-op build. A directory at
  `$(OUT)` is removed and the binary rebuilt when missing or stale.
  (9) **Local I6 re-records must match CI's interpreter set.** The first
  2026-11-07 metric run scored `test_xpickle` `EXIT_DIFFERS`: its
  backwards-compat workers resolve every versioned CPython on PATH, and
  this box carries a uv-managed 3.11 that lacks the pyc-only
  `__pyc_eval__` builtin — the same pre-existing delta recorded 2026-11-01,
  which `metric.yml` removes with its "Match the baseline's interpreter set"
  step so the nightly scores the suite the baseline saw. A baseline
  recorded under a polluted PATH would make CI report noise every morning.
  The re-record therefore ran under a shadow PATH with versioned pythons
  hidden (CI rule: keep `python3`, `python3.14`, drop the rest). Follow-up
  (not done here, A5 contract change on its own review): pin the same
  interpreter set inside `child_env()` so every local run is CI-comparable
  without a workflow step.
  (10) **`test_signal.py` re-classified: a measurement-value line, not a
  silent wrong answer.** Under the 600s budget it finishes (both sides,
  `OK (skipped=4)`), but in script mode `support.verbose` defaults to 1
  (`test/support/__init__.py:189` — regrtest sets it to 0), so the test
  always prints `detected median itimer() resolution: %.6f s.` — a runtime
  JITTER MEASUREMENT (20 samples of a 1 µs `setitimer`, median of gaps),
  not a computed answer. In the 2026-11-07 metric run the two oracle
  samples happened to agree (0.000013 s) while the subject, run at a
  different load point, printed 0.000016 s — so the per-run I5a "earned"
  rule did not fire and the mechanical I1 rule (exit 0 + `STDOUT_DIFFERS`)
  flagged it as a new silent wrong answer. Discriminating experiment
  (I9): an exact replica of `measure_itimer_resolution` run 10× per side
  sequentially and 32× per side at 8-way concurrency — CPython and pyc
  produced indistinguishable distributions on every pass (seq: 8×0.000012
  + 2×0.000013 each; 8-way: 31×0.000013 + 1×0.000014 each). No systematic
  pyc offset. The value has no stable ground truth (same family as the
  other coin-flips); whether a run reports it as `ORACLE_UNSTABLE` or
  `STDOUT_DIFFERS` depends on whether the oracle's two samples straddle a
  load shift. It is recorded in the re-baseline as a known case: the
  comparator's I1 rule exempts cases already baselined as exit 0 +
  `STDOUT_DIFFERS` ("a recorded defect, not a regression"), so future
  runs are stable under either flag shape.
  (11) **Driver renamed `pycc` → `pyc` (A0 sign-off, the project owner,
  2026-11-07).** Not a contract change: INTERFACES.md §5 has always
  specified `pyc <source.py> [-o OUT]` — the implementation had drifted to
  `pycc`. The in-tree driver is `compiler/tools/pyc`, the installed name is
  `bin/pyc`; error prefixes, tool flags (`--pyc`), and every doc/CI/script
  reference now use the contract name. INTERFACES bumped to v1.2 to record
  the sign-off (changelog: no contract changed). Measured after the rename:
  `make -C verify fast` 865/865, `make -C verify verify` 887/887 gate OK,
  `install-pyc.sh` prefix runs `bin/pyc` with no env. `find_pyc()` in
  `measure_run.py` now also resolves the driver relative to the repo root,
  so `make -C verify fast` no longer depends on the caller's cwd.
  (12) **Driver and backend consolidated into ONE C++ binary (A0-directed,
  2026-11-07).** The former bash `compiler/tools/pyc` script plus the separate
  `pyc_lower` binary are now a single `pyc` executable (`compiler/src/driver.cpp`
  links the lowering core, called in-process via `pyc::lower_envelope` — it is
  no longer a process the driver must find and launch). Only the parse (target
  interpreter, VERSION_TARGETING (b)) and the final clang link (CHARTER toolchain
  pin) remain subprocesses; the lowering was the part that was a second binary
  for no architectural reason. `make -C compiler` builds
  `compiler/tools/pyc` (where the script lived); `install-pyc.sh` now installs a
  single `bin/pyc` (no `bin/pyc_lower`); `PYC_LOWER` is gone. The binary keeps an
  internal `pyc --lower [ENVELOPE|-]` door (the old `pyc_lower` CLI) so the
  IR-level checks drive the lowering directly. §5 CLI unchanged; INTERFACES
  v1.3. Measured on this tree: `make -C verify fast` 865/865, `make -C verify
  verify` 887/887 gate OK, `check_ir_wellformed` 77 well-formed / 0 malformed,
  `install-pyc.sh` prefix runs `bin/pyc` with no env. (Separate, pre-existing:
  `coverage.py`'s self-check now finds none of its 7 hardcoded probe constructs
  refused — the compiler covers all of them — so it cannot build a probe; it is
  a local dev tool, not a CI gate. Needs a new still-refused candidate or
  retirement.)
  I6 re-recorded with (3) and (9) in effect: **376/389 (96.66%)**,
  `compiler/baseline-libtest.json` updated (see the header row).
- 2026-11-02: inline sync-comprehension loop vars into the enclosing
  function's locals (MVP item 1B). CPython 3.12+ inlines sync
  comprehensions (PEP 709): the loop target is a fast local of the
  enclosing function (it shows in co_varnames but is not name-bound, so a
  later `i` is a NameError and the slot is left NULL -- which is what makes
  the debugger's unbound-locals warning count it). pyc lowered the comp as a
  separate function, so the enclosing function's local set was missing the
  loop var; the lowered layout diverged from the parse-oracle code object's
  co_varnames, layout_ok failed, and the function fell back to the eval stub
  (a synthetic code object) instead of adopting the real one. Bind the sync
  comp's loop target in the enclosing scope during scope collection (the
  walrus walk already descends into comps; generator expressions are left
  alone -- they are their own frame). Now a comp-bearing function's layout
  matches the oracle and it adopts the real inlined code object. Measured:
  the comp functions' code objects now match the oracle byte-for-byte (list/
  dict/set comp 46/50/48 units vs the old 220/254-unit stubs); test_trace
  27/27 (was failing test_trace_list_comprehension) and JumpTestCase 99/99
  (was failing test_jump_backward_over_listcomp_v2); full test_sys_settrace
  448/448. Comp values differential-identical across list/dict/set/multi-gen/
  nested/free-var/tuple-target forms. fast 865/865, verify 887/887.
- 2026-11-02: debugger line jumps in eval-stub frames (MVP item 2). A
  function that carries location slots is a pyc stub; the 3.14 `f_lineno`
  setter validates a jump with `mark_stacks`, which does not propagate
  reachability past `RETURN_VALUE`. The old stub put `RETURN_VALUE` at unit 7,
  before the location slots (8+slot), so every slot was statically
  unreachable and every debugger line jump failed with "can't jump from
  unreachable code". Two changes:
  (1) The slot-bearing stub (`kEvalCodeSlots`) is now an 11-unit header
  (`kStubHeaderUnits`, support.hpp) ending in `LOAD_CONST <falsy>` ;
  `POP_JUMP_IF_TRUE` over the slots ; `RETURN_VALUE`. The constant is `None`
  (falsy), so at runtime the jump is never taken and ceval falls straight
  through to the return without executing the slots -- the stub fires no line
  events of its own (a precise line-counting tracer sees only the impl's).
  But `mark_stacks` follows the jump EDGE, so the slots stay reachable and the
  setter accepts the jump. The parking convention moves from 8+slot to
  `kStubHeaderUnits`+slot (frame.cpp), and the linetable splits the 11-unit
  header into two entries (8+3) to stay inside the 3-bit length field.
  (2) codegen: a block reached only through an error edge is an exception pad;
  landing a jump there raises and early-returns. Straight-line code is one
  block, so a statement's line sat mid-block and could only be "claimed" by
  its own pad, which stole the line. Exclude pads from jump-target candidacy.
  Measured: `JumpTestCase` 96/99 -> 98/99 (fixes `test_jump_forward_over_
  listcomp` 2->3 and `test_jump_backward_over_listcomp` 3->1); full
  `test_sys_settrace` 445/448 -> 447/448 (oracle 448/448). The one remaining,
  `test_jump_backward_over_listcomp_v2` (8->2), needs 1B: CPython inlines the
  sync listcomp so its loop var is an enclosing local (NULL at the jump -> the
  "assigning None to N unbound locals" warning) and the jump has real bytecode
  to redirect onto; pyc models the comp as a separate frame, so neither holds.
  fast 865/865, verify 887/887.
- 2026-11-01: `test_sys_settrace` loop/finally/break tail worked to
  completion. Unique failing cases 228 -> 118, each group root-caused and
  oracle-differenced:
  (1) Post-unwind completion and finally bookkeeping branched and built
  pending-value `ConstNull`s stamped at the `try:`/`finally:` header line,
  so the header re-fired after the guarded region completed (an extra LINE
  after an `else` body; the implicit return reporting the header line).
  Line-0 bookkeeping locations: no location update, `lasti` stays where the
  real code left it.
  (2) After a `break` out of a NESTED loop, `rejoin_outer_live` re-`IntLoad`s
  the live i64s in the loop's after-exit block, stamped at the loop header
  line -- the header re-fired after the break (CPython's break jumps past
  the condition and never re-fires it). Line-0 location; the real line stays
  on the unbound-error pad.
  (3) CPython fires the `except:` line while INSPECTING each handler (the
  clause's `LOAD_GLOBAL` + `CHECK_EXC_MATCH` sit on the except line), for
  every handler it inspects -- matched or not. pyc ran the dispatch on the
  try line and fired the except line only in a matched handler's body, so an
  inner non-matching handler's line was missing when the exception unwound
  outward. Fire it at the start of each handler's dispatch; the match logic,
  body-entry bookkeeping and the branch run on the except line.
  (4) When an exception unwinds into a `with`'s `__exit__`, CPython takes the
  exception out of the raised state and fires the `with` line before the
  `__exit__` call. pyc called `__exit__` with the exception still in flight,
  and a LINE event never fires while an exception is set (the CPython
  invariant that keeps the user trace function from running with the
  program's exception set). `pyc_rt_exit_exc` now takes the with line, takes
  the exception out, fires the line (new `pyc_rt_fire_line`: a line event
  resolved from the source coordinates alone, no compiler-assigned slot) and
  calls `__exit__`; a raising line-event callback routes through the same
  error path as a raising `__exit__`.
  The remaining 118 are all outside the tail's scope: `TraceOpcodesTestCase`
  56 (opcode events need the synthetic code object to expose real opcodes),
  `JumpTestCase` 50 (debugger line jumps across desugared constructs), and
  12 class-creation cases -- pyc lowers the class body inline in the
  enclosing function, so there is no class-body frame and no class-body
  call/line/return events (the same fundamental gap as the first two).
- 2026-10-31: `test_sys_settrace` event-stream gaps, two sub-fixes.
  (1) The legacy `'exception'` trace event: CPython fires it once per frame
  in the propagation chain (a fresh `raise`, a failing op, or the moment one
  arrives from a callee) and NOT on a bare re-raise (RERAISE has no legacy
  mapping) or the for-loop's normal StopIteration. The firing rule collapses
  onto the pad's `add_tb` flag — a pad adds a traceback entry iff an
  exception is thrown in this frame, which is exactly where the event fires.
  New `FireException` IR op at the entry of every throwing pad; runtime
  `pyc_rt_trace_exception` reports the in-flight `(type, value, tb)`.
  (2) The `try:` header LINE event: CPython emits a NOP at the header so the
  header line fires in order before the body; pyc lowered the body directly,
  so the header line fired late and out of order. New `SetLine` op (a
  standalone location update = CPython's NOP) emitted at the try header.
  Oracle-verified across nested propagation, same-frame catch, re-raise,
  `raise X from Y`, generator StopIteration, and failing subscript/attr/binary/
  name. `test_sys_settrace` 113 -> 107 unique failing, zero new; fast gate
  865/865, verify 887/887. File-level `Lib/test` neutral (the settrace row was
  already failing), so I6 stays **369/389**.
- 2026-11-01: (c) `test_tracemalloc.test_no_incomplete_frames` **fixed**
  (was a known delta). Root cause: CPython's `MAKE_CELL` sits BEFORE `RESUME`
  (frame "incomplete", `instr_ptr < firsttraceable`), so tracemalloc's
  `_PyFrame_GetFirstComplete` skips it; pyc's synthetic code object has RESUME
  at offset 0 (`_co_firsttraceable == 0`), so a pyc frame is never incomplete
  and tracemalloc recorded an extra frame (the def line). Fix: `pyc_rt_cell_new`
  allocates the cell while the current frame's `owner` is
  `FRAME_OWNED_BY_INTERPRETER` (the first branch of `_PyFrame_IsIncomplete`),
  restored immediately. `test_tracemalloc` 50/51 -> 51/51.
- 2026-10-31 known delta, root-caused and decided (not a regression):
  (d) `test_sys`
  `test_remote_exec_with_self_process` — `sys.remote_exec` on the *self*
  process needs a `*python*` section in `/proc/pid/maps`; pyc statically links
  libpython (deliberate Tier-1 self-contained binary) so there is none.
  Decision: accept the delta; linking shared libpython would break the
  self-contained design for one niche remote-debugging test. Child-process
  `remote_exec` tests pass (the child is the sysroot CPython).
- 2026-10-31: debugger line-jump redirects (a trace callback sets
  `frame.f_lineno`). A per-function jump table lands on the reported
  line's block; target selection is the exact all-or-nothing test (a use
  kills every candidate iff the reverse natural reach from its use block,
  with its def block removed, holds a target) — test_io lower 245s -> 0.9s.
  The location update gets its own `loc_err` edge (separate from
  `on_error`), and the runtime stays silent while an exception is in
  flight. `test_traceback` passes, so I6 moves 368 -> 369/389. The jump
  checks and `loc_err` pads make the generated module ~4.6x larger; `pyc`
  now skips LTO at `-O0` (nothing to inline when unoptimised), taking the
  test_io final link 108s -> 9s and clearing the metric gate's compile
  timeouts (`DID_NOT_COMPILE` back to 0).
- 2026-10-28 blocker pass (B1–B8), all measured: B1 mangled
  kwonly-default keys (`TypeError` on a defaulted kwarg fixed, probe
  `mangled_kwonly_defaults.py`); B2 build/pack/install now refuse a
  sysroot whose interpreter does not match its manifest (the published
  3.14.5-in-a-3.14.7-manifest release); B3 `LD_LIBRARY_PATH` scrubbed
  from every oracle spawn (the gate now measures the right CPython
  from a polluted shell); B4 deploy: stdlib search is verified per
  candidate and a miss names what it searched; B5 C-trace contract
  (`c_traceobj` argument, callee `co_firstlineno` on CALL, quiet line
  updates) — `test_sys_settrace` no longer segfaults; B7 the line-event
  stream for desugared loops (runtime fire rule, back-edge force-fire on
  same-line jumps, for/while re-stamping, quiet cell prologue) —
  `loop_line_events.py` is byte-identical to the oracle and `test_trace.py`
  passes; B6 the
  Tier-1 verify probe pins `PYTHONHOME` (it used to measure the host's
  stdlib on hosts that ship python3.14); B8 nightly leak bar (cp314d
  sysroot, workloads flat, negative control leaks +1/iter). Gates on
  this tree: 887/887 language, 368/389 `Lib/test`, no regressions.
- The location-slot commit is `adc0c4e`. The language-only
  fast gate on that tree was 860/860 impactful, 858 byte-identical,
  547s. The two stderr-only rows were `case_208.py` and `case_209.py`.
- M5 was held at **355/389** until `make -C verify metric`; that ran
  2026-10-29 and the row is now 368/389. Do not edit
  `compiler/baseline-libtest.json` by hand. A local 389-file run on
  intermediate commit `17270a0` printed 358/389 and was never this row:
  it predates the slot cache, and it was not adopted. Script runs
  named in [CORRECTNESS.md](CORRECTNESS.md) are not this row either.
- `pack-pyc.yml` and `metric.yml` were adjusted in `249eb81`, which is
  on `origin/devel`. Whether a scheduled run has since finished is not
  claimed here.
- `sysroot.yml` has been succeeding. `verify.yml` is the push gate.

**Next (MVP completeness)**

1. I6 re-run 2026-10-29: **368/389 (94.60%)**, adopted as
   `compiler/baseline-libtest.json`; the 2026-09-26 355/389 record is
   superseded. The unpublished `17270a0` run was never a substitute for
   this. **B2 done 2026-11-01.** The stale asset on the
   `sysroot-cp314-linux-x86_64` release was a **3.14.4** in a 3.14.7 box
   (84MB, uploaded 2026-10-31). It was deleted and replaced with a fresh
   pack of the genuine 3.14.7 sysroot (39.6MB, engine banner
   `3.14.7 (main, Oct 28 2026, 10:24:26)`), re-verified by downloading the
   published asset and running its launcher under `env -i`. Release notes
   corrected to `glibc 2.43`. Gotcha that masked the truth locally: this
   shell exports `LD_LIBRARY_PATH=/home/joe/local/lib` (a 3.14.5
   `libpython3.14.so.1.0`); the prefix launcher uses `RUNPATH`, which the
   loader ranks *below* `LD_LIBRARY_PATH`, so any direct `python3.14 -VV`
   in that shell reports 3.14.5. The gates are unaffected — `child_env()`
   strips `LD_LIBRARY_PATH` — so every measurement here has been against
   the real 3.14.7. The B2 pack guard (refuse a sysroot whose interpreter
   does not match its manifest) will not let the mismatch happen again.
2. ~~Dump the remaining `EXIT_DIFFERS` before adding syntax.~~
   **Done (2026-11-07).** The named cluster (stub `co_code` / `co_consts` /
   linetable — `test_dis`, `test_compile`, `test_peepholer`,
   `test_opcache` — plus `test_sys_settrace`) is closed: all pass in the
   375/389 baseline (`f3ff9e6`). The two remaining `EXIT_DIFFERS` files
   are root-caused and decided, not open: `test_sys` (accepted 2026-10-31)
   and `test_external_inspection` (accepted 2026-11-07, same class).
   The `test_dis` sub-item below is closed history.

    - `test_dis` dis-rendering gap — **closed** (passes in the 375/389
      baseline; see the 2026-11-07 entry above). It was 16
      failures on `be1b7f3` (sysroot 3.14.7, 2026-10-05). The co_firstline
      stub fix `3c03bff` (in `lower.cpp`) resolves the 6
      `disassemble_*` stub cases (`test_disassemble_class`/`_method`/
      `_static_method` × `DisTests`+`DisWithFileTests`) and leaves **10**.
      All 10 are `dis` *output* — runtime bytes MATCH CPython; none is a
      wrong value. Three rendering groups: (a) `tricky` in
      `test_info`/`test_code_info`/`test_show_code` — pyc emits
      `NESTED`/`0x1000000` on the `Flags` line and lists cells per-name
      where CPython groups them `[abedfxyz]`; (b) quickened/specialized
      opcodes in the ×2 `test_loop_quicken`, ×2
      `test_loop_with_conditional_at_end_is_quickened`, ×2
      `test_super_instructions` — pyc's baseline bytecode prints `RESUME`
      where CPython quickened bytecode prints `RESUME_CHECK` (and
      `FOR_ITER` vs `FOR_ITER_RANGE`, `JUMP_BACKWARD_NO_JIT`,
      `LOAD_GLOBAL` vs `LOAD_GLOBAL_MODULE`, `CALL` vs `CALL_PY_GENERAL`,
      `LOAD_CONST` vs `LOAD_CONST_MORTAL`); (c) `test_nested` differs only
      in the embedded code-object pointer (cross-process address, not an
      opcode). These are `dis`/`disasm` rendering gaps, not silent wrong
      answers (I1). Bound the count with an I6 run before/after.
3. M6: a refusal names the construct, the line, and the reason.
   **Gated 2026-11-07** — `check_diagnostics.py` runs in `verify.yml`
   static checks and fails the PR gate on a bare `exit 1` (it proved
   DIAGNOSED on HEAD before wiring).

**Speed, measured 2026-10-02** (`adc0c4e`, sysroot 3.14.7, all exit 0).
`-O2` is the generated IR only; the runtime is already `-O2`.

| Test | CPython | `-O0` | `-O2` |
|---|---:|---:|---:|
| `test_buffer.py` | 6.61s | 14.81s | 14.23s |
| `test_decimal.py` | 8.91s | 18.42s | 17.96s |
| `test_long.py` | 2.32s | 2.65s | 2.68s |
| `test_unicodedata.py` | 25.28s | 40.37s | 38.62s |

`-O2` saved a few percent. The cycles are in libpython calls the native
body makes, plus work the eval loop does not do as its own call.
`pyc_rt_set_lasti` is still the hottest symbol on `test_unicodedata.py`
(9.3%): each source line stores `instr_ptr`, and the slot array only
removed the hash. The interpreter is still 6–12% (stubs). Attribute
load is `PyObject_GetAttr`, so a method call allocates a temporary and
then vectorcalls it. On these four files the C work (Unicode, buffer,
decimal, long arithmetic) is the program. Removing the eval loop
entirely would beat CPython by a small factor. A 5–10× figure needs
different programs: pure Python, values kept in SSA between calls, and
method calls that do not allocate.

**Later (v1 / CHARTER §4)**

I8 two targets from one binary; unittest-driven metric; native
generators; Tier 2; unboxing remainder. Column carets for an adopted
code object come from `pyc_rt_set_lasti`. Star-as-annotation,
`PyFunction`, and `__annotate__` have landed (see P1 above).

## Workstream S — stop requiring a local CPython *build*

Two different things. Do not mix them.

**(a) Output binaries link static libpython (Tier 1).** Settled (CHARTER §2,
I7). Wheels need it. **Do not drop.**

**(b) Developers/CI must compile a purpose-built 3.14.7 sysroot**
(`tools/build-python-sysroot.sh`, default
`$HOME/opt/py-sysroots/cp314-3.14.7-tier1`). This is **not** implied by (a).
It is onboarding and CI friction.

Four Pythons are conflated today:

| | Role | Need Python headers in `pyc`? |
|---|---|---|
| A | Host: run `pyc_parse` (subprocess) | **No.** `pyc` is plain clang++ (links no libpython). |
| B | Target: headers + `libpython.a` to **link the user's program** | Yes, including `internal/pycore_*.h` for C1b (`frame.cpp`). |
| C | Oracle: I5 differential tests | Same interpreter as B. |
| D | Purpose-built Tier-1 vs stock distro | Distro 3.12 cannot do B/C/I7. |

Parse already uses the target interpreter (VERSION_TARGETING option (b)).
S1: `pyc` reads `pyc-sysroot.json` when present and does not hardcode
`python3.14`. `--python=X.Y` as a second artifact is S5.

**What cannot be removed:** libpython in every output; internal headers for
C1b; same CPython for parse + link + I5 when claiming correctness; wheel
ABI = sysroot ABI; stdlib next to that libpython at run time.

**Rejected:** stock distro `libpython` for linking; parse-3.12 / link-3.14;
an independent runtime.

| Phase | What | Unlocks |
|---|---|---|
| S0 done | Document (a)≠(b). | Honest onboarding |
| S1 done | `pyc` reads `pyc-sysroot.json`; `--python-sysroot`; no hardcoded `python3.14` | I8 locally |
| S2 done | `sysroot.yml` publishes `sysroot-cp314-linux-x86_64`. `build-python-sysroot.sh` remains the producer. The nightly has been succeeding. | Developers do not compile CPython |
| S3 done | `pyc` searches beside itself (`sysroot/`, `../sysroot`). `setup-machine.sh --beside`. `PyConfig.home` is set from the prefix. | Download ≠ `$HOME/opt/...` |
| S4 done | `pyc --fetch-sysroot` / `PYC_FETCH=1`. A missing sysroot is exit 2. Verify still requires `--sysroot` / `--oracle`. | No local CPython install to compile a program |
| S5 done | `install-pyc.sh` prefix: `bin/pyc`, `lib/pyc/`, `sysroots/<abi>-<ver>-tier1/`. `--python=X.Y` selects a tree; a missing version is exit 2. One 3.14 artifact is enough. `pack-pyc.sh` writes the compiler tarball. The nightly publish step is green — verified 2026-11-07: four consecutive successful runs and both release assets re-uploaded (the earlier "failing after a successful pack" note is closed). | VERSION_TARGETING as shipped |

S2 is what removes “build this specific Python on your machine,” and
the sysroot release is the one that has been publishing. S4 is when the
compiler user no longer needs a local copy. S5 is the prefix. A
contributor changing A2 still rebuilds the sysroot when headers change.

## Doc debt (do not confuse with compiler bugs)

Still historical, not a description of the current tree:
`compiler/README.md` totality figures are the 2026-08-22
round-trip and its "still refused" list is the 2026-08-23 state (both now
carry correction notes; the live numbers are in this file's "Measured
now" table). Trust `pyc` and `verify/` over those sentences. CHARTER
product rules still need explicit sign-off; a factual correction is not
a rule change.

Cleared 2026-11-07: the INTERFACES.md naming debt is resolved — the driver
was renamed `pycc` → `pyc` (A0 sign-off given 2026-11-07; INTERFACES v1.2,
no contract change: §5 has always specified `pyc`, the implementation had
drifted). VERSION_TARGETING.md already names the live Tier-1
sysroot (the `/home/joe/local` reference is dated 2026-08-22); the
STATUS "Next (MVP completeness)" EXIT_DIFFERS/test_dis items are marked
done/closed; compiler/README's refusal list is corrected; UNBOXING.md's
"Steps 1–3 landed" header now reads all sixteen; GIL/CORRECTNESS carry
the 887/887 gate figure; NOTES.md's immediate state is current.
