# pyc — current state and MVP

**Date:** 2026-10-02. Language numbers in the table below are the recorded
`compiler/baseline-language.json` gate (881/881). A language-only fast
gate on this date was 860/860 impactful (858 byte-identical); the 881
gate was not re-run. The `Lib/test` row is still the 2026-09-26 I6
record. CHARTER remains binding. §4 product rules were not changed.

Roles that produced this: Architect (`agents/architect.md`), PM
(`agents/pm.md`), SWE-compiler (`agents/swe-compiler.md`), SWE-runtime
(`agents/swe-runtime.md`).

## Verdict

The rebuild is on the CHARTER architecture: generated AST, `PyObject*` via
libpython, C-API protocols, I1 refusals, I5 differential harness, published
I6. Language + gaps + concurrency is **881/881** impactful. `Lib/test` is
**355/389 = 91.26%** (`-O0`, sysroot 3.14.7, `--stdlib`), last measured
2026-09-26. C1 (frames) and C2 (periodic GIL) are closed. A proved
heap-free loop drops the GIL only when another thread requests it
([GIL.md](GIL.md)). The remaining product gap is stub code objects,
named refusals instead of LLVM crashes, unexplained `EXIT_DIFFERS`, and
getting the nightly Lib/test job to finish so this row can move.
Ordinary functions carry CPython's code object when the local layout
matches (`df32461`); the native body still runs. The eval stub remains
for the module frame, class bodies, comprehensions, and any function
whose locals do not match.

## Measured now

| Check | Result |
|---|---|
| language + gaps + concurrency | 881/881 impactful |
| `Lib/test` (I6) | 355/389 (91.26%), measured 2026-09-26 |
| I6 composition | 327 clean + 28 `STDERR_DIFFERS`-only = 355 pass |
| `DID_NOT_COMPILE` | 0 |
| `EXIT_DIFFERS` | 21 |
| `STDOUT_DIFFERS` | 10 |
| timeout | 4 |
| `ORACLE_UNSTABLE` | 11 |
| A1 round-trip | 3.14.7 and 3.13.15 TOTAL (`compiler/README.md`) |
| Tier-1 / NumPy | CI wheel smoke; `ldd` has no libpython `DT_NEEDED` |
| C1b / C2 | closed; probes in `language/` |

I6 is file-as-script vs sysroot 3.14.7 with `--stdlib` (unittest can
import `test.support`). Do not treat unittest MATCH on a single file as
the I6 row. The 2026-08-27 snapshot (246/389, 24 DNC) is replaced:
`--stdlib` was missing locally, so many files imported and exited 0
without running tests. Net +26 (67 newly passing, 41 now fail).

## Major remaining issues

Ranked by Architect and SWE-compiler. Causes of most `EXIT_DIFFERS` are
**unknown** — dump artefacts before adding syntax.

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

4. **`EXIT_DIFFERS`.** Dump first. `test_super` **40/40** (3 skipped);
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
    leftovers: `test_super_deep` (~464 B/C-frame, 90k needs ~42MB);
    `test_compile` native `__code__` bytecode (A); `test_dis` Bound at
    `co_consts[1]`;     `test_raise` **37/37**; `test_gc` get_objects/heap_size;
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
7. **I8 CLI (S1).** `pycc` finds the sysroot interpreter (manifest or
   `bin/python3`), `--python-sysroot` aliases `--sysroot`, `--python`/`-std`/
   `--python-abi`/`--list-python-targets` work. `-std` is `--feature-version`.
   Two runtimes from one binary is still S5.
8. **Lock-file scale.** `lower.cpp` ~6k lines. Header still says nothing
   unboxes; unboxing has landed (`UNBOXING.md` 1–16).

### P2 — product / process

9. **I6 republished 2026-09-26.** `compiler/baseline-libtest.json` is
    355/389 vs sysroot 3.14.7, `jobs=16`, `-O0`. `DID_NOT_COMPILE` is 0.
10. **Developer sysroot.** LLVM 22 + hand-built 3.14.7 tree +
    `PYC_LOWER=/tmp/pyc_lower`. See workstream S below.
11. **Output home.** `PyConfig.home` is set from `PYTHONHOME`, then
     `lib/pythonX.Y` next to (or above) the executable, then the compile-time
     sysroot. Prefix is no longer implicit-only.
12. **A2 leak bar** (`Py_REF_DEBUG` slope on the corpus) not measured on the
    metric sysroot.

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
| M1 | `make -C verify verify` | 881/881 impactful; no new silent-wrong |
| M2 | `make -C verify fast` | `--fail-on-silent-wrong` |
| M3 | CI `ldd` smoke | no `libpython` `DT_NEEDED` |
| M4 | CI wheel step | NumPy import+run in that binary |
| M5 | I6 vs README | **355/389 (91.26%)**, no regress |
| M6 | remaining compile fails | construct + line + reason — **not** invalid LLVM IR |

**Not MVP gates:** unittest metric; `--python=3.13`; ELF `-static`; unboxing
vs C.

## Next steps

**Now**

- The location-slot commit is `adc0c4e`. The language-only
  fast gate on that tree was 860/860 impactful, 858 byte-identical,
  547s. The two stderr-only rows were `case_208.py` and `case_209.py`.
  The 881 gate (language + gaps + concurrency) was not re-run.
- Leave M5 at **355/389** until `make -C verify metric`. Do not edit
  `compiler/baseline-libtest.json` by hand. A local 389-file run on
  intermediate commit `17270a0` printed 358/389 and is not this row:
  it predates the slot cache, and it was not adopted. Script runs
  named in [CORRECTNESS.md](CORRECTNESS.md) are not this row either.
- `pack-pyc.yml` and `metric.yml` were adjusted in `249eb81`, which is
  on `origin/devel`. Whether a scheduled run has since finished is not
  claimed here.
- `sysroot.yml` has been succeeding. `verify.yml` is the push gate.

**Next (MVP completeness)**

1. Re-run I6 before publishing any Lib/test fraction other than 355/389.
   The unpublished `17270a0` run is not a substitute for that.
2. Dump the remaining `EXIT_DIFFERS` before adding syntax. The cluster
   that is already named: stub `co_code` / `co_consts` / linetable
   (`test_dis`, `test_compile`, `test_peepholer`, `test_opcache`) and
   `test_sys_settrace` (600s timeout).
3. M6: a refusal names the construct, the line, and the reason.

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

| | Role | Need Python headers in `pyc_lower`? |
|---|---|---|
| A | Host: run `pyc_parse` (subprocess) | **No.** `pyc_lower` is plain clang++. |
| B | Target: headers + `libpython.a` to **link the user's program** | Yes, including `internal/pycore_*.h` for C1b (`frame.cpp`). |
| C | Oracle: I5 differential tests | Same interpreter as B. |
| D | Purpose-built Tier-1 vs stock distro | Distro 3.12 cannot do B/C/I7. |

Parse already uses the target interpreter (VERSION_TARGETING option (b)).
S1: `pycc` reads `pyc-sysroot.json` when present and does not hardcode
`python3.14`. `--python=X.Y` as a second artifact is S5.

**What cannot be removed:** libpython in every output; internal headers for
C1b; same CPython for parse + link + I5 when claiming correctness; wheel
ABI = sysroot ABI; stdlib next to that libpython at run time.

**Rejected:** stock distro `libpython` for linking; parse-3.12 / link-3.14;
an independent runtime.

| Phase | What | Unlocks |
|---|---|---|
| S0 done | Document (a)≠(b). | Honest onboarding |
| S1 done | `pycc` reads `pyc-sysroot.json`; `--python-sysroot`; no hardcoded `python3.14` | I8 locally |
| S2 done | `sysroot.yml` publishes `sysroot-cp314-linux-x86_64`. `build-python-sysroot.sh` remains the producer. The nightly has been succeeding. | Developers do not compile CPython |
| S3 done | `pycc` searches beside itself (`sysroot/`, `../sysroot`, `pyc_lower`). `setup-machine.sh --beside`. `PyConfig.home` is set from the prefix. | Download ≠ `$HOME/opt/...` |
| S4 done | `pycc --fetch-sysroot` / `PYC_FETCH=1`. A missing sysroot is exit 2. Verify still requires `--sysroot` / `--oracle`. | No local CPython install to compile a program |
| S5 done | `install-pyc.sh` prefix: `bin/pycc`, `lib/pyc/`, `sysroots/<abi>-<ver>-tier1/`. `--python=X.Y` selects a tree; a missing version is exit 2. One 3.14 artifact is enough. `pack-pyc.sh` writes the compiler tarball. The nightly publish step has been failing after a successful pack (see Next steps). | VERSION_TARGETING as shipped |

S2 is what removes “build this specific Python on your machine,” and
the sysroot release is the one that has been publishing. S4 is when the
compiler user no longer needs a local copy. S5 is the prefix. A
contributor changing A2 still rebuilds the sysroot when headers change.

## Doc debt (do not confuse with compiler bugs)

Still historical, not a description of the current tree:
[INTERFACES.md](INTERFACES.md) §5 spells the driver as `pyc`; the binary
is `pycc` (that file is frozen and needs A0 sign-off to rename).
[VERSION_TARGETING.md](VERSION_TARGETING.md) still calls
`/home/joe/local` the ready stock build; the live sysroot is named in
CHARTER §6. `compiler/README.md` totality figures are the 2026-08-22
round-trip. Trust `pycc` and `verify/` over those sentences. CHARTER
product rules still need explicit sign-off; a factual correction is not
a rule change.
