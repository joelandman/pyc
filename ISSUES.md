# pyc Issue Register

Owned by **SWR**. Coordinator seeds and updates status on merge. SWE does not edit this file.

Schema:

```
## I-NNN  short title
- Status: open | accepted | in-progress | fixed | wontfix
- Severity: crash | wrong-answer | latent | limitation | doc-drift
- Evidence: file:line or repro + CPython vs pyc
- Files:
- Blocks merge: yes/no
- Notes:
```

When this file disagrees with the `pyc` binary or `tests/runner.py`, trust the executable and fix the register.

---

## Open

### I-007  `module.get()` dispatched as dict.get
- Status: open
- Severity: wrong-answer
- Evidence: IMPLEMENTATION.md Known Limitations. Any synthetic module function named `get` becomes dict `.get()` and returns `None`.
- Files: `src/Compiler.cpp` (`lowerMethodCall`)
- Blocks merge: no
- Notes: Wave 2 W2.2. Same lock as I-006.

### I-008  `super().__init__` into a builtin base
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md user-defined exception subclasses. `class MyError(Exception): def __init__(self, m): super().__init__(m)` is unsupported. `class MyError(Exception): pass` works.
- Files: `src/Compiler.cpp` (class / super), Runtime exception construction
- Blocks merge: no
- Notes: Wave 3 W3.2.

### I-009  Uncaught tracebacks lack File/line
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md exceptions. IR already carries `lineno` for `-g`.
- Files: Runtime exception printer; possibly Codegen
- Blocks merge: no
- Notes: Wave 3 W3.1.

### I-010  str.format nested fields; partition("") is lenient
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md / FEATURES.md. `"{0.attr}"` / `"{0[1]}"` unsupported. Empty separator does not raise `ValueError`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_StrFormat`, partition)
- Blocks merge: no
- Notes: Wave 3 W3.3.

### I-011  `type()` is a display string, not a type object
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md. `type(x).__name__` is parsed out of `"<class '…'>"`. User classes omit `__main__.`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Type`, `Pyc_GetAttr`)
- Blocks merge: no
- Notes: Architectural. Not scheduled before Wave 3.

### I-012  Functions lack `__name__` / `__doc__` / `__call__` attributes
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md “Full First-Class Function Objects — Partial.”
- Files: Runtime function objects, `Pyc_GetAttr`
- Blocks merge: no

### I-013  Type-tag collisions
- Status: open
- Severity: latent
- Evidence: `include/pyc/object_struct.h`. Tag 5 = bool **and** None. Tag 7 = tuple **and** super proxy. Tags 8–9 unused by “real” Python types (used by regex/match).
- Files: `include/pyc/object_struct.h`, Runtime type switches, Codegen field GEPs
- Blocks merge: no
- Notes: Do not add a new type without reading this. Splitting shared tags is a dedicated design, not a drive-by.

### I-014  A6 speculative unbox / multi-dispatch
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md Planned. Prototype generated per-sig variants; non-recursive sites stay boxed because dispatch checks LLVM IR nativeness, not inferred types.
- Files: `src/codegen/Codegen.cpp`, `src/Compiler.cpp` A6
- Blocks merge: no
- Notes: Wave 4 W4.3. **Design review required** before SWE writes it.

### I-015  Boxed-receiver method fallback ~7× slower
- Status: open
- Severity: limitation
- Evidence: PERFORMANCE_BASELINE.md “The real cost: unproven receivers.” 8M `.count(1)`: proven 0.11s vs boxed 0.93s. Two unused opts: arity-specific `Pyc_CallBuiltinMethodN`; `(tag, name)` lookup.
- Files: `src/runtime/Runtime.cpp`, `src/Compiler.cpp` fallback emit
- Blocks merge: no
- Notes: Wave 4 W4.1 / W4.2.

### I-016  Arena allocator / escape analysis / float-return A6
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md Planned; PERFORMANCE_BASELINE / PROFILE_NBODY leftovers.
- Files: Runtime allocator, Codegen
- Blocks merge: no
- Notes: Wave 4, after W4.1/W4.2.

### I-017  Datetime / pathlib / bytes / hashlib / struct / decimal subsets
- Status: open
- Severity: limitation
- Evidence: FEATURES.md synthetic-module table. No µs/tz; Path single-arg; no `open(..., "rb")`; hashlib no `.update()`; decimal no `getcontext`.
- Files: Runtime synthetic modules
- Blocks merge: no
- Notes: New modules are deferred. Expand an existing subset only with an explicit ticket.

### I-018  `sorted(..., reverse=)` + `cmp_to_key` combination
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md sorted/key/reverse write-up.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Sorted` / cmp_to_key path)
- Blocks merge: no

### I-019  Debug-info leftovers
- Status: open
- Severity: limitation
- Evidence: DEBUGGING_PLAN.md Known Limitations. `-g` works. Remaining: GDB `PyObject*` pretty-printer; mark A6 variants `FlagArtificial`; optional `-g` on `runtime.bc`; README CLI table (fixed in Wave 0).
- Files: `src/codegen/Codegen.cpp` DI; docs / `.gdbinit`
- Blocks merge: no
- Notes: Wave 3 W3.4.

### I-020  Keyword args dropped by method-call fallback
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md dispatch steps 2–3. Terminal fallback builds positional-only lists. Arms that accept keywords (`split(maxsplit=)`, `format(**kwargs)`) must keep their fast path when `hasKeywordArgs`.
- Files: `src/Compiler.cpp` `lowerMethodCall`
- Blocks merge: no
- Notes: Do not whitelist those arms for unproven receivers without carrying kwargs.

### I-021  NUL in str literals is truncated at parse/codegen
- Status: open
- Severity: wrong-answer
- Evidence: `print(['a\\x00b'])` → `['a']` (SWE W1.1 note; Coordinator swapped the I-001 CASE to `\\x01` for this reason). CPython keeps the embedded NUL (`['a\\x00b']`, `len('a\\x00b')==3`). Parser `src/frontend/PythonParser.cpp:69-70`: `PyUnicode_AsUTF8` + `std::string(const char*)` (strlen). Codegen `const` handler `src/codegen/Codegen.cpp:2190-2198`: `CreateGlobalStringPtr` + `PyUnicode_FromString`. Bytes already use the length-explicit `bytesconst` / `PyBytes_FromStringAndSize` path; the comment there calls out that `const` cannot be reused for this reason. Related sink: `PyBuiltin_Chr` (`src/runtime/Runtime.cpp:2404-2409`) does `char buf[2]={(char)(v&0xFF),0}` then `PyUnicode_FromString`, so `chr(0)` is the empty string, not `\\x00`.
- Files: `src/frontend/PythonParser.cpp` (Constant unicode), `src/codegen/Codegen.cpp` (`const` handler), `src/runtime/Runtime.cpp` (`PyUnicode_FromString` / `PyBuiltin_Chr`)
- Blocks merge: no
- Notes: Found reviewing W1.1 / I-001. Not this ticket — `pyc_format_str_repr` would emit `\\x00` if a NUL ever reached the runtime (`std::string` walks by length). Fix is `PyUnicode_AsUTF8AndSize` + `PyUnicode_FromStringAndSize` (bytes already have the analogue).

### I-022  Leftover unescaped string quoting after I-001
- Status: open
- Severity: wrong-answer
- Evidence: `str(KeyError('a\\nb'))` — `pyc_exc_message` (`src/runtime/Runtime.cpp:12541-12542`) does `"'" + cell_content->str + "'"`; CPython `KeyError.__str__` is `repr(args[0])` → `"'a\\nb'"`. `print([Path('a\\nb')])` — PrintElement type 16 (`src/runtime/Runtime.cpp:1693`) `PosixPath('%s')` interpolates the raw path; CPython is `PosixPath({!r})` → `[PosixPath('a\\nb')]`.
- Files: `src/runtime/Runtime.cpp` (`pyc_exc_message` KeyError arm; PosixPath PrintElement)
- Blocks merge: no
- Notes: Same class as I-001, out of W1.1 scope (container/repr of str). Decimal `Decimal('%s')` is fine — `mpd_to_sci` has no quotes/controls. `PyStr_FromAny` list/dict `"'"+str+"'"` wraps (~2098–2119) are only the tmpfile-fail fallback; live `str(['a\\nb'])` is Print (already fixed by I-001). `repr(Path(...))` still falls through PyBuiltin_Repr to `<object>` (I-017 pathlib subset), distinct from the PrintElement wrap. Do not treat as an I-001 miss.

### I-023  Dynamic `*args` still binds None for missing required
- Status: open
- Severity: wrong-answer
- Evidence: `hadRuntimeStar` skips default injection *and* the new `Pyc_CheckMissingArgs` emit (`Compiler.cpp:5225`, `5293`). `__va_*` then unpacks via `emitForwardCallFromList` → `PyList_GetItemObj` (`Compiler.cpp:4120-4126`); OOB returns `nullptr` (`Runtime.cpp:614-620`), which prints as `None`. CPython `def f(a): f(*mk())` with `mk` returning `[]` is `TypeError: f() missing 1 required positional argument: 'a'`.
- Files: `src/Compiler.cpp` (`ensureVaWrapper` / `emitForwardCallFromList`), `src/runtime/Runtime.cpp` (`PyList_GetItemObj`)
- Blocks merge: no
- Notes: Found reviewing W1.2 / I-002. SWE listed this as out of slice. Literal `f(*[])` / tracked `xs=[]; f(*xs)` statically expand (`Compiler.cpp:4847-4857`, `7307-7312`) so they *should* hit the new check — do not treat those as this ID. Repro is a non-literal star (`def mk(): return []` / `xs=list()`). No LLVM verify fail (wrapper still passes `fixed` operands). Unexpected kwargs / too-many positionals stay out of scope.

### I-024  Indirect `g(**{})` treats the empty kwargs dict as a positional
- Status: open
- Severity: wrong-answer
- Evidence: Indirect lowering appends the merged kwargs dict as the last apply-list element (`Compiler.cpp:5188-5202`). Adapter missing-arg check uses `userLen = list_len - ncells` (`Codegen.cpp:1051-1064`) and does not exclude that trailing dict unless the *target* has `**kwargs` (`hasKwVar` peel is later, `1165-1202`). `def f(a): g=f; g(**{})` → `userLen==1`, `firstDef==1`, no raise; slot 0 is bound to `{}`. Direct `f(**{})` is fine (I-002 dict-spread check). CPython: `TypeError: f() missing 1 required positional argument: 'a'`.
- Files: `src/codegen/Codegen.cpp` (adapter `userLen` / `need.miss`), `src/Compiler.cpp` (indirect kwargs append)
- Blocks merge: no
- Notes: Found reviewing W1.2 / I-002. Combination of two ticket surfaces (`f(**{})` and `g=f; g()`), not in the runner CASES. Same class as I-002; do not demand it in W1.2.

### I-025  Missing-arg TypeError uses IR / bare name, not CPython `__qualname__`
- Status: open
- Severity: wrong-answer
- Evidence: Adapter always passes `f.name` (`Codegen.cpp:1091`). Compiler `callDisplayName` is `funcDisplayNames` (the def id) or the IR name (`Compiler.cpp:4339-4342`). Lambdas never register a display name (`lowerLambda` ~6429-6439). Direct nested `inner()` → `inner()`; CPython is `outer.<locals>.inner()`. Indirect nested (`g=inner; g()`) → `__nesteddef_N()`. `f=lambda a: a; f()` is a *direct* call via `lambdaAliases` → `__lambda_N()`; CPython is `<lambda>()`. `emitFuncValue` already builds the qualname for `repr` (`Compiler.cpp:3348-3365`) and does not share it with this path.
- Files: `src/codegen/Codegen.cpp` (adapter `miss.fn`), `src/Compiler.cpp` (`callDisplayName`, `funcDisplayNames`, `lowerLambda`)
- Blocks merge: no
- Notes: Found reviewing W1.2 / I-002. Current CASES are top-level `f` (IR name == Python name), so they would not see this. File so a later nested/lambda CASE against live python3 does not look like an I-002 miss.

### I-026  `del t[1:3]` on tuple/str/dict is a silent no-op
- Status: open
- Severity: wrong-answer
- Evidence: `Pyc_DelSlice` (`src/runtime/Runtime.cpp:6056`) returns immediately unless `obj->type == 1`. CPython `del (5,1,8,3)[1:3]` is `TypeError: 'tuple' object doesn't support item deletion`; same for `str` / `dict`. pyc leaves the object unchanged. Pre-fix `Pyc_DelItem` was also a no-op on a slice key, so this is leftover of W1.3 scope, not a regression.
- Files: `src/runtime/Runtime.cpp` (`Pyc_DelSlice`)
- Blocks merge: no
- Notes: Found reviewing W1.3 / I-003. Ticket asked to classify: **out of slice** (I-003 is A4 `lst->list` readers; the confirmed hole was list slice delete). Do not demand a TypeError in W1.3.

### I-027  `Pyc_DelSlice` reverse-step start underflow; step 0 silent
- Status: open
- Severity: wrong-answer
- Evidence: `h=[0,1,2,3,4]; del h[-10::-1]`. CPython `PySlice_AdjustIndices` with step<0 clamps an under-range start to `-1` → slicelength 0 → list unchanged. pyc (`Runtime.cpp:6065-6068`, `6089-6095`) does `s += n` then `if (ss < 0) ss = 0`, so the loop `i > e` (`e` is the omitted-stop sentinel `-1`) visits index 0 and yields `[1, 2, 3, 4]`. Same clamp as pre-existing `Pyc_GetSlice`/`Pyc_SetSlice`. `del h[::0]` is `if (stp == 0) return` (`6063`) vs CPython `ValueError: slice step cannot be zero`. Common reverse cases `del h[::-1]` / `[::-2]` / `[3::-1]` / `[3:0:-1]` compute the right positions.
- Files: `src/runtime/Runtime.cpp` (`Pyc_DelSlice`)
- Blocks merge: no
- Notes: Found reviewing W1.3 / I-003. Off-by-one only when start < `-len` with a negative step (the ticket's `::2` / basic slice / omitted-start reverse are fine). Non-int step is ignored and `stp` stays 1, so `del h[::2.0]` becomes a full basic delete rather than TypeError — same class, same helper. Not a merge blocker.

### I-028  Dead OOM checks after `allocObject` switched to `new`
- Status: open
- Severity: latent
- Evidence: `allocObject` is `new PyObject()` (`Runtime.cpp:6705-6709`), which throws `std::bad_alloc` (or terminates under `-fno-exceptions`) and never returns null. Four calloc-era arms are now dead: `runRegexAll` `if (!m)` (`6782`), `PyBuiltin_ReSearch` (`6913`), `PyBuiltin_ReMatch` (`6954`), `PyBuiltin_ReCompile` (`6969`). On OOM the `pcre2_*` handle allocated just above leaks and a C++ exception can escape `extern "C"` — same as every other `new PyObject()` in this file.
- Files: `src/runtime/Runtime.cpp` (`allocObject` callers)
- Blocks merge: no
- Notes: Found reviewing W1.4 / I-004. Not this slice. Drop the checks, or use `nothrow` if anyone wants the old cleanup path. Datetime/list/dict already omit the null test.

### I-030  Remaining boxed-accepting `lowerMethodCall` arms steal user methods
- Status: open
- Severity: wrong-answer
- Evidence: `class C` with `is_file`/`is_dir`/`mkdir`/`joinpath`/`isoformat`/`weekday`/`isoweekday`/`total_seconds`/`group`/`is_integer`/`most_common`/`elements`/`subtract`; also `format(a=1)` / `sort(key=len)`. CPython returns the user methods. pyc: `False`/`False`/`None`/`x`, empty isoformat, `0`/`1`/`0.0`, `None`, `True`, `[('__class__', 0)]`, `[]`, `None`; kwargs `format`/`sort` also stolen. Same result as a direct `C().is_file()` — user instances are `typeOf` `"boxed"`, not only through a parameter.
- Files: `src/Compiler.cpp` (`lowerMethodCall` pathlib block ~9043–9070; datetime ~9078–9096; `group` ~9102; boxed `is_integer` ~9145; `format`/`sort` `hasKeywordArgs` ~9220 / ~9403; Counter ~9477–9516)
- Blocks merge: no
- Notes: Found reviewing W2.1 / I-006. SWE flagged pathlib `is_file`/`is_dir`/`mkdir`/`joinpath`. Same class as the ticket, **not** this ticket’s cases (`exists`/`call`/`bit_length`/`fromkeys`/`unlink`/`isfile`/`isdir`/`check_output` all pass). `.get()` is I-007. Do not demand in W2.1. Fix is the exists pattern: proven type only at compile time; runtime tag in `Pyc_CallBuiltinMethod`.

### I-031  `fromkeys` / `os.path` AST gates miss aliases and from-imports
- Status: open
- Severity: wrong-answer
- Evidence: `D = dict; D.fromkeys([3])` — CPython `{3: None}`; pyc `AttributeError: 'str' object has no attribute 'fromkeys'` (`dict` is the token `PyBuiltin_DictFactory`). `from os import path; path.exists(".")` — CPython `True`; pyc `AttributeError: 'dict' object has no attribute 'exists'`. `import os as ox; ox.path.exists(".")` and `q = os.path; q.exists(".")` work.
- Files: `src/Compiler.cpp` (`fromkeys` Name==`"dict"` ~9311; `exists`/`isfile`/`isdir` AST `os.path` ~9324)
- Blocks merge: no
- Notes: Found reviewing W2.1 / I-006. The fromkeys miss is caused by this slice (parent name-only `fromkeys` served `D.fromkeys` by accident). `from os import path` was already wrong on the parent (boxed pathlib `exists` → `False`). Ticket CASES use `dict.fromkeys` and `import os` / `import os as`. Not a merge blocker.

---

## Closed (already fixed; listed so agents do not re-open them)

### I-006  Remaining name-only `lowerMethodCall` arms
- Status: fixed
- Severity: latent
- Evidence: Runner 665/665, `file_case_failures=0`. W2.1 CASES (banner in `tests/runner.py`) match CPython at -O0 and -O2: user `C().call/exists/bit_length/fromkeys/unlink/isfile/isdir/check_output`, boxed `f(C())` for those names, boxed `(7).bit_length` / `dict.fromkeys` / `{}.fromkeys`, `os.path.exists/isdir/isfile(".")`, `import os as ox; ox.path.exists`, `Path.exists()` direct and through a parameter, `True.bit_length()`. `check_dispatch_chain.py`: 72 arms, 1 documented exemption (`compile` only; `exists` exemption no longer fires). Coordinator added the CASES first; they failed on the parent (`C().call(1)` → `-1`, `C().exists()` → `False`, `C().bit_length()` → `0`, `C().fromkeys([1])` → `{1: None}`).
- Files: `src/Compiler.cpp` (`lowerMethodCall`, `builtinMethodRows`, `RecvKind::Int`), `src/runtime/Runtime.cpp` (`Pyc_CallBuiltinMethod` tags 0/5/2/16)
- Notes: Wave 2 W2.1. Gated: `bit_length` (proven int/bool table row, not `"boxed"`); `fromkeys` (proven dict or AST Name `"dict"`); `exists`/`isfile`/`isdir` (AST `os.path`, including `import os as`); `unlink` (AST Name os/alias); `call`/`check_output` (AST Name subprocess/alias). Name-only `bit_length` deleted. Pathlib `exists` inner arm is now `typeOf=="path"` only — parent `C().exists()` was `PyPathlib_Exists` (the boxed pathlib arm), not `Pyc_OsPathExists`. Leftovers I-030 / I-031. SWR: no merge blockers.

### I-005  Nested-comp subscript on the iteration variable
- Status: fixed
- Severity: wrong-answer
- Evidence: `detectCompElementType` no longer guesses float for Name[Constant]. I-029 ListComp arm always boxed. Runner 662/662. Focused cases + nbody match CPython at -O0/-O2.
- Files: `src/Compiler.cpp`
- Notes: Wave 1 W1.5. SWR found I-029 in the first pass; Coordinator applied `return "boxed"` for nested ListComp. Both closed in this slice.

### I-029  Nested ListComp inherit treats a list as a scalar int/float
- Status: fixed
- Severity: wrong-answer
- Evidence: `[[1 for _ in [0]] for __ in [0]]` → `[[1]]`. CASES under I-029.
- Files: `src/Compiler.cpp` (`detectCompElementType` ListComp arm)
- Notes: Introduced and fixed in W1.5.

### I-004  `allocObject()` calloc on a C++ PyObject
- Status: fixed
- Severity: latent
- Evidence: `allocObject` is `new PyObject()`. Runner 650/650. Valgrind memcheck 0 errors on a create/drop regex program. Focused cases match CPython at -O0 and -O2.
- Files: `src/runtime/Runtime.cpp`
- Notes: Wave 1 W1.4. Dead `freeObject` removed. SWR: no merge blockers. Leftover I-028 (dead OOM null-checks).

### I-003  Incomplete `pyc_ensure_boxed_list` audit
- Status: fixed
- Severity: latent / wrong-answer
- Evidence: `Pyc_DelSlice` + `PyDict_FromKeys` ensure. CASES under `W1.3 / I-003`. Runner 649/649. Focused 14/14 at -O0 and -O2.
- Files: `src/Compiler.cpp`, `src/runtime/Runtime.cpp`, `include/pyc/runtime.h`, `src/codegen/Codegen.cpp`
- Notes: Wave 1 W1.3. Most A4 consumers were already safe. Real hole: `del lst[s:e]` silent no-op. SWR: no merge blockers. Leftovers I-026 / I-027.

### I-002  Missing required arguments raise TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: `Pyc_RaiseMissingArgs` / `Pyc_CheckMissingArgs`. CASES under `W1.2 / I-002`. Runner 644/644. Focused cases match CPython at -O0 and -O2 (14/14).
- Files: `src/Compiler.cpp`, `src/runtime/Runtime.cpp`, `include/pyc/runtime.h`, `src/codegen/Codegen.cpp`
- Notes: Wave 1 W1.2. Direct `f()`, `f(**{})`, `g=f; g()`. SWR: no merge blockers. Leftovers I-023 / I-024 / I-025.

### I-001  Container repr does not escape special characters
- Status: fixed
- Severity: wrong-answer
- Evidence: `pyc_format_str_repr` in Runtime.cpp. CASES under `W1.1 / I-001`. Runner 638/638. Focused repro matches CPython at -O0 and -O2.
- Files: `src/runtime/Runtime.cpp`
- Notes: Wave 1 W1.1. Quote-switch + ASCII escapes match CPython `unicode_repr`. Bare `print("a\nb")` still str. Leftovers filed as I-021 / I-022. SWR: no merge blockers.

### I-100  `**dict` unmatched keys not routed to `**kwargs`
- Status: fixed
- Severity: wrong-answer
- Evidence: `Pyc_RouteSpreadKwargs` in Runtime.cpp / Compiler.cpp / Codegen.cpp. KnownGapsPlan Gap 1.
- Notes: IMPLEMENTATION.md “Still not fixed” paragraph was stale (corrected Wave 0).

### I-101  List/set comprehension multi-variable unpack
- Status: fixed
- Commit: `86e9fd5`
- Evidence: KnownGapsPlan Gap 2. `[k for k, g in pairs]` works. `tests/unpack_comp.py` FILE_CASE.
- Notes: IMPLEMENTATION.md “Newly Discovered… [None, None]” section was stale (corrected Wave 0).

### I-102  `re.match` not anchored
- Status: fixed
- Evidence: KnownGapsPlan Gap 3. `PyBuiltin_ReMatch` / `PCRE2_ANCHORED`.

### I-103  User names `t0`/`c0` collided with compiler temps
- Status: fixed
- Commit: `7e4e0b8`
- Evidence: KnownGapsPlan Gap 4. Temps are `$tN` / `$cN`.

### I-104  No distinct tuple type
- Status: fixed
- Commits: `4899369`, `df9c6e9`, `69a5fa2`
- Evidence: Type tag 7. itertools/os.path/operator/struct/enumerate/zip/dict.items/partition/groupby/most_common return tuples.

### I-105  Float `20.0` printed as scientific notation
- Status: fixed
- Commit: `93e222a`
- Evidence: `tests/runner.py` case `print(20.0)` → `20.0\n`. IMPLEMENTATION.md Known Limitations paragraph was stale (corrected Wave 0).

### I-106  Dicts not insertion-ordered
- Status: fixed
- Commit: `02a9574`
- Evidence: `std::vector<std::pair<…>>` payload. KnownGapsPlan “related remaining” and IMPLEMENTATION Planned list were stale (corrected Wave 0).

### I-107  `modifiers.py` infinite loop on `continue` at `-O0`
- Status: fixed
- Evidence: `tests/runner.py` FILE_CASES includes `modifiers.py`. For-loop continue now points after the increment. AGENTS.md exclusion note was stale (corrected Wave 0).

### I-108  `-g` debug info
- Status: fixed
- Commit: `73694a7`
- Evidence: `src/main.cpp` `-g`; DIBuilder in Codegen; IR `lineno`. Leftovers tracked as I-019.

### I-109  Method-dispatch table migration (steps 1–6)
- Status: fixed
- Commits: `a8ffcf0` … `4457356`
- Evidence: `builtinMethodRows()`, `Pyc_CallBuiltinMethod`, `tests/check_dispatch_chain.py`. Name-only leftovers closed in I-006; remaining boxed-accepting steals are I-030.

### I-110  `time.perf_counter` missing
- Status: fixed
- Evidence: Synthetic `time.perf_counter` exists. `mbs.py` is excluded for the 5s runner timeout, not a missing builtin. PERFORMANCE_BASELINE.md note was stale (corrected Wave 0).
