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
