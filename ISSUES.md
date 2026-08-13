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

### I-001  Container repr does not escape special characters
- Status: open
- Severity: wrong-answer
- Evidence: `print(["a\nb", "c"])` embeds a real newline. CPython prints `['a\\nb', 'c']`. IMPLEMENTATION.md (Known Limitations / Runtime).
- Files: `src/runtime/Runtime.cpp` (`PyObject_PrintElement` / `PyBuiltin_Repr`)
- Blocks merge: no
- Notes: Wave 1 default first ticket (W1.1). Runtime-only. New CASES should not assert on `print()` of newline-containing strings inside a container until this is fixed.

### I-002  Missing required arguments become None, not TypeError
- Status: open
- Severity: wrong-answer
- Evidence: IMPLEMENTATION.md `**kwargs` section: omitted required params fall back to boxed `None`. `def f(a): return a` / `f()` should raise `TypeError`.
- Files: `src/Compiler.cpp` (`lowerCall`, dict-spread fallbacks), possibly Runtime
- Blocks merge: no
- Notes: Wave 1 W1.2. Locks Compiler.

### I-003  Incomplete `pyc_ensure_boxed_list` audit
- Status: open
- Severity: latent
- Evidence: IMPLEMENTATION.md Runtime notes. Homogeneous int/float list literals live in `ilist`/`flist`. Readers of `lst->list` without the helper silently no-op (`.sort()`/`.index()` class of bug, already fixed for the eight methods that were audited).
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Wave 1 W1.3. Audit remaining `lst->list` readers; do not “fix” by disabling A4.

### I-004  `allocObject()` calloc on a C++ PyObject
- Status: open
- Severity: latent
- Evidence: IMPLEMENTATION.md Runtime. Types 8/9 (compiled regex / match) allocate via `calloc`. `dict` is a non-trivial `std::unordered_map`/`vector` member — constructor never runs. Safe today only because those types do not touch `.dict`.
- Files: `src/runtime/Runtime.cpp` (`allocObject`)
- Blocks merge: no
- Notes: Wave 1 W1.4. Switch to `new PyObject()` like datetime/list/dict.

### I-005  Nested-comp subscript on the iteration variable
- Status: open
- Severity: wrong-answer
- Evidence: KnownGapsPlan.md “Related remaining gaps.” Float-promoted / tuple-collapsed values. Distinct from list-comp unpack (I-102, fixed).
- Files: `src/Compiler.cpp` (comprehension lowering / type tracking)
- Blocks merge: no
- Notes: Wave 1 W1.5.

### I-006  Remaining name-only `lowerMethodCall` arms
- Status: open
- Severity: latent
- Evidence: IMPLEMENTATION.md method-dispatch write-up. ~35 name-only arms remain after the table migration. A catch-all still shadows later same-name arms (`Counter.update` shipped dead). `tests/check_dispatch_chain.py` is a syntactic smoke detector, not a reachability analysis. Exemptions: `exists`, `compile`.
- Files: `src/Compiler.cpp` (`lowerMethodCall`, `builtinMethodRows`)
- Blocks merge: no
- Notes: Wave 2 W2.1. Compiler locked for the whole wave.

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

---

## Closed (already fixed; listed so agents do not re-open them)

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
- Evidence: `builtinMethodRows()`, `Pyc_CallBuiltinMethod`, `tests/check_dispatch_chain.py`. Remaining arms: I-006.

### I-110  `time.perf_counter` missing
- Status: fixed
- Evidence: Synthetic `time.perf_counter` exists. `mbs.py` is excluded for the 5s runner timeout, not a missing builtin. PERFORMANCE_BASELINE.md note was stale (corrected Wave 0).
