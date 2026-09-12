# pyc — current state and MVP

**Date:** 2026-09-12. Numbers from `compiler/baseline-libtest.json` and
`compiler/baseline-language.json` unless marked unknown. CHARTER remains
binding; this file is the dashboard, not an amendment.

Roles that produced this: Architect (`agents/architect.md`), PM
(`agents/pm.md`), SWE-compiler (`agents/swe-compiler.md`), SWE-runtime
(`agents/swe-runtime.md`).

## Verdict

The rebuild is on the CHARTER architecture: generated AST, `PyObject*` via
libpython, C-API protocols, I1 refusals, I5 differential harness, published
I6. Language + gaps + concurrency is **788/788** impactful. `Lib/test` is
**272/389 = 69.92%** (`-O0`, sysroot 3.14.7, `--stdlib`). C1 (frames) and
C2 (periodic GIL) are closed. The remaining product gap is not a new
runtime: it is well-formed IR on every accepted program, named refusals
instead of LLVM crashes, unexplained `EXIT_DIFFERS`, and a developer
toolchain that still requires a purpose-built 3.14.7 sysroot.

## Measured now

| Check | Result |
|---|---|
| language + gaps + concurrency | 788/788 impactful |
| `Lib/test` (I6) | 272/389 (69.92%) |
| I6 composition | 254 clean + 18 `STDERR_DIFFERS`-only = 272 pass |
| `DID_NOT_COMPILE` | 0 |
| `EXIT_DIFFERS` | 98 |
| `STDOUT_DIFFERS` | 10 |
| timeout | 11 |
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
   `exec(f.__code__, closure=...)` runs the native body via a stub bytecode
    helper that reads the eval frame's function closure. LoadGlobal uses the
    current frame's globals (and mapping `__getitem__`), so FORWARDREF
    annotate reconstruction works. Nested `__annotate__` code objects from
    CPython's compile sit in the outer `co_consts`. Unexpected keywords
    offer a "Did you mean" suggestion.
7. **I8 CLI (S1).** `pycc` finds the sysroot interpreter (manifest or
   `bin/python3`), `--python-sysroot` aliases `--sysroot`, `--python`/`-std`/
   `--python-abi`/`--list-python-targets` work. `-std` is `--feature-version`.
   Two runtimes from one binary is still S5.
8. **Lock-file scale.** `lower.cpp` ~6k lines. Header still says nothing
   unboxes; unboxing has landed (`UNBOXING.md` 1–16).

### P2 — product / process

9. **I6 republished 2026-09-12.** `compiler/baseline-libtest.json` is
    272/389 vs sysroot 3.14.7, `jobs=8`, `-O0`. `DID_NOT_COMPILE` is 0.
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
| M1 | `make -C verify verify` | 788/788 impactful; no new silent-wrong |
| M2 | `make -C verify fast` | `--fail-on-silent-wrong` |
| M3 | CI `ldd` smoke | no `libpython` `DT_NEEDED` |
| M4 | CI wheel step | NumPy import+run in that binary |
| M5 | I6 vs README | **246/389 (63.24%)**, no regress |
| M6 | remaining compile fails | construct + line + reason — **not** invalid LLVM IR |

**Not MVP gates:** unittest metric; `--python=3.13`; ELF `-static`; unboxing
vs C.

## Next steps

**Now**

- Keep M1–M5 green.
- Dump stderr/IR for P0 (malformed IR, `test_unpack`, exit-5) and the 102
  `EXIT_DIFFERS` before adding syntax.
- Re-run `make -C verify metric` so baseline matches HEAD (t-strings /
  `except*` / type aliases).
- This file is the dashboard. CHARTER frame-deferred text is stale; amend
  only with user sign-off.

**Next (MVP completeness)**

1. Turn LLVM verifier failures into fixes or I1 diagnostics (M6).
2. I1a: star-as-annotation is Unpack; format>2 is NotImplementedError.
   Async comprehensions remain.
3. Function-object protocol: real `PyFunction` + vectorcall trampoline.
   `exec(f.__code__, closure=)` runs the native body.
4. Module/class `__annotate__` exists; function param/return `__annotate__`
   landed (format 1), including nested capture of enclosing locals via cells.
5. Workstream S phases 0–2 (sysroot as data + prebuilt artifact).

**Later (v1 / CHARTER §4)**

I8 two targets from one binary; more Lib/test; unittest-driven metric;
native generators; traceback carets; Tier 2; unboxing remainder.

Suggested compiler order (SWE-compiler): I1a probes → function protocol →
`__annotate__` → comprehension `for` targets → leave async-in-native alone
→ grow Lib/test by fixing loud failures → unboxing/carets last.

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
| S0 now | Document (a)≠(b). Fix stale “frontend links libpython” CI comment. | Honest onboarding |
| S1 done | `pycc` reads `pyc-sysroot.json`; `--python-sysroot`; no hardcoded `python3.14` | I8 locally |
| S2 next | Publish a prebuilt sysroot tarball (CI already caches the tree). `build-python-sysroot.sh` remains the *producer*, not the onboarding step. | Developers do not compile CPython |
| S3 later | Relocatable toolchain (`pycc` finds sysroot next to itself). Output `PyConfig.home` landed (PYTHONHOME, beside-exe, baked sysroot). | Download ≠ `$HOME/opt/...` |
| S4 later | Casual `pycc file.py` uses bundled/downloaded sysroot. Missing target → compile error, not a wrong binary. Verify still uses that interpreter as oracle. PATH `python3` only for parse-only / `--emit-llvm` when `version_info[:2]` matches the PTD. | No local CPython install to compile a program |
| S5 v1 | Release layout: `bin/pycc`, `lib/pyc/`, `sysroot/`. `--python=X.Y` is a second artifact, not a flag on one libpython. | VERSION_TARGETING as shipped |

S2 is the first phase that removes “build this specific Python on your
machine.” S4 is when the *compiler user* no longer needs a local copy.
Contributors changing A2 still rebuild the sysroot when headers change.

## Doc debt (do not confuse with compiler bugs)

Stale vs executable: CHARTER still says frames are deferred; INTERFACES §5
CLI is unimplemented; VERSION_TARGETING / CHARTER §6 still mention
`/home/joe/local`; `compiler/README.md` still lists some refusals that have
landed; GENERATORS/GIL/UNBOXING status lines lag the code. Trust `pycc` and
`verify/` over those sentences. CHARTER edits need explicit sign-off.
