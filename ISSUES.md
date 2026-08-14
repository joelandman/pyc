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
- Notes: Do not add a new type without reading this. Splitting shared tags is a dedicated design, not a drive-by. W4.2 table keeps both collisions on purpose (bool `bit_length` on tag 5; tuple `count`/`index` on tag 7). Concrete leftovers: I-048 (boxed super proxy methods) / I-054 (len/in/subscript/`list()`, W5.1b) / I-057 (`GetSlice` / `tuple()` / map/filter). Type-5 fake-None `bit_length` is the same class; real `None` is a null pointer (I-047), not this tag.

### I-014  A6 speculative unbox / multi-dispatch
- Status: open
- Severity: limitation
- Evidence: IMPLEMENTATION.md Planned + W4.3 design. Prototype generates per-sig variants (`__specialized_add_ii` / `_ff`). Codegen dispatch (`Codegen.cpp` ~3485–3531) only routes when **every** arg is already LLVM i64/double. A loop `s = add(s, i)` boxes the first result, so later iterations never hit the variant. Recursive `fib` works because `n` is a native param inside the variant.
- Files: `src/codegen/Codegen.cpp` (call-site dispatch), `src/Compiler.cpp` `generateSpecializedVariants`
- Blocks merge: no
- Notes: Wave 4 W4.3 **design accepted, not implemented**. See IMPLEMENTATION.md “W4.3 Design Decision”. Do not start as a quick win. Next SWE needs a dedicated ticket: runtime tag-check + unbox, native result slots, no global native stores. I-016 is a later follow-on, not a substitute.

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

### I-044  Optional `-g` on `runtime.bc`
- Status: open
- Severity: limitation
- Evidence: DEBUGGING_PLAN leftover. User programs compiled `-g -O0` have no LTO of `runtime.bc`; stepping into runtime helpers has no line tables unless the bitcode itself is built with debug info.
- Files: CMake `runtime.bc` target, `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Split from I-019 after W3.4. Does not retype user-local DI (I-043).

### I-045  `str.find` drops `end` (proven and boxed)
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.2 CASE (`tests/runner.py` banner `# W5.2 / I-045`):
  `"banana".find("a", 2, 3)` / `"banana".find("a", 2, 6)` / `def f(s): return s.find("a", 2, 3); f("banana")` / `"banana".rfind("n", 0, 3)` → `-1\n3\n-1\n2\n`.
  Proven: `Compiler.cpp` find arm `args.size() >= 3` → `PyString_Find4` (no longer `>= 2` → Find3 only).
  Boxed: `pyc_bm_str_find` reads `pyc_arg_at(argsList, 2)` and calls Find4 — same a2 probe as `rfind`.
  Codegen: 4-arg extern `PyString_Find4` next to `PyString_RFind4`; header is `extern "C"`.
  Ticket inputs through Find4: st=2 en=3, `find("a", 2)` is 3, `3+1 > 3` → `-1`; st=2 en=6 → `3`. Boxed arity-3 goes through `Pyc_CallMethodOrBuiltin` + argsList, not `CallBuiltinMethod2`.
- Files: `src/Compiler.cpp` (`find` arm), `src/runtime/Runtime.cpp` (`PyString_Find4`, `pyc_bm_str_find`), `src/codegen/Codegen.cpp`, `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 5 W5.2. SWR: no merge blockers on this CASE. Empty-sub + start==end is I-061. `count`/`index`/`startswith`/`endswith` still drop start/end (I-062).

### I-046  Boxed `split(None)` / `rsplit(None)` is space-split, not whitespace-run
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.1 CASE `def fs(s): return s.split(None); fs("a  b")` already matched CPython on the parent (`['a', 'b']`). `None` is `nconst`/nullptr, so `pyc_bm_str_split` takes `SplitWhitespace`. SWE did not touch the handlers.
- Files: `src/runtime/Runtime.cpp` (`pyc_bm_str_split` / `pyc_bm_str_rsplit`; `PyString_Split2`)
- Blocks merge: no
- Notes: Wave 5 W5.1. Already matched; W5.1 CASE is a guard. Type-5 None as sep still hits `delim=" "` if any path boxes it (I-052 is non-str). Not claiming a SWE code change.

### I-047  Null `None` receiver in builtin method fallback is silent None
- Status: fixed
- Severity: wrong-answer
- Evidence: CPython `None` is pyc’s null `PyObject*` (`nconst` / Codegen.cpp). `pyc_call_builtin_method` returns nullptr on `!receiver` before the miss path. `pyc_builtin_type_name(nullptr)` already returns `"NoneType"` but is never reached.
  - `def f(x): return x.bit_length(); f(None)` — CPython `AttributeError: 'NoneType' object has no attribute 'bit_length'`; pyc prints `None`.
  - Same for `x.nope()` (W4.2 CASE only covers `bad(1)`).
- Files: `src/runtime/Runtime.cpp` (`pyc_call_builtin_method` early return)
- Blocks merge: no
- Notes: Wave 5 W5.1. `pyc_call_builtin_method`: `!receiver` + str name → `AttributeError: 'NoneType' object has no attribute '…'` via `pyc_builtin_type_name(nullptr)`. Type-5 fake-None `bit_length` is still I-013. `len(None)` / `None[0]` stay None-as-nullptr ops.

### I-048  Boxed super proxy `.count`/`.index` is tuple
- Status: fixed
- Severity: wrong-answer
- Evidence: Tag 7 is tuple **and** super proxy (I-013). Table (and the old switch) registers `(7, count)` / `(7, index)` → `PyList_Count`/`Index`. Syntactic `super().count(...)` does **not** reach this function (`PyBuiltin_SuperMethod`, I-038). A stored/boxed proxy does:
  ```
  class C:
      def f(self):
          s = super()
          return s.count(1)
  C().f()
  ```
  CPython: `AttributeError: 'super' object has no attribute 'count'`. pyc: `0` (proxy `list` is empty). `Pyc_GetItem` on tag 7 only accepts an int key, so `__class__` lookup is null and the fallback fires.
- Files: `src/runtime/Runtime.cpp` (`tables[7]` count/index)
- Blocks merge: no
- Notes: Wave 5 W5.1. `PyBuiltin_Super` sets `cell_content = super` (self-pointer). `pyc_call_builtin_method` skips the tag-7 table when `type==7 && cell_content`; AttributeError uses `"super"`. Compiler never wrote this field (`Compiler.cpp` has no `cell_content`). Tuples: `new PyObject()` zero-inits it; `PyTuple_New` never sets it. Type-7 free does **not** `DECREF` `cell_content` (only types 10/11 do) — cycle is safe today; do not add type 7 to that branch without dropping the self-pointer. W5.1b closed I-054 (`len`/`in`/subscript/`list()`). Leftover GetSlice/`tuple()`/map/filter is I-057. Not I-038.

### I-049  Function-local C++ objects in Runtime.cpp are unconstructed under runtime.bc LTO
- Status: open
- Severity: latent
- Evidence: W4.2 first used `static std::unordered_map tables[21]` + `std::call_once` inside `pyc_call_builtin_method`. `-O0` (libpycrt.a) was fine. `-O2` (runtime.bc LTO) SEGV’d on every boxed method: the compiler-emitted ctor for that array does not run on the bitcode path, so `find` hit a zeroed object. Shipped fix: `new[]` behind a BSS pointer. Any future function-local / static C++ object with a ctor in Runtime.cpp has the same `-O2`-only landmine.
- Files: `src/runtime/Runtime.cpp`, CMake `runtime.bc`
- Blocks merge: no
- Notes: Found implementing W4.2. Do not treat the heap table as a general fix.

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
- Status: fixed
- Severity: wrong-answer
- Evidence: `str(KeyError('a\\nb'))` — `pyc_exc_message` (`src/runtime/Runtime.cpp:12541-12542`) does `"'" + cell_content->str + "'"`; CPython `KeyError.__str__` is `repr(args[0])` → `"'a\\nb'"`. `print([Path('a\\nb')])` — PrintElement type 16 (`src/runtime/Runtime.cpp:1693`) `PosixPath('%s')` interpolates the raw path; CPython is `PosixPath({!r})` → `[PosixPath('a\\nb')]`.
- Files: `src/runtime/Runtime.cpp` (`pyc_exc_message` KeyError arm; PosixPath PrintElement)
- Blocks merge: no
- Notes: Wave 5 W5.1. KeyError arm (`12740–12741`) and Path PrintElement (`1696`) both use `pyc_format_str_repr` (quote-switch + `\\n`). `repr(Path)` still `<object>` (I-017). Decimal `'%s'` unchanged.

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
- Status: fixed
- Severity: wrong-answer
- Evidence: `Pyc_DelSlice` (`src/runtime/Runtime.cpp:6056`) returns immediately unless `obj->type == 1`. CPython `del (5,1,8,3)[1:3]` is `TypeError: 'tuple' object doesn't support item deletion`; same for `str` / `dict`. pyc leaves the object unchanged. Pre-fix `Pyc_DelItem` was also a no-op on a slice key, so this is leftover of W1.3 scope, not a regression.
- Files: `src/runtime/Runtime.cpp` (`Pyc_DelSlice`)
- Blocks merge: no
- Notes: Wave 5 W5.1. `Pyc_DelSlice` raises `TypeError: '…' object does not support item deletion` via `pyc_builtin_type_name` when `type != 1`. Dict omitted from the CASE (3.14 hashes slices). W5.1b closed I-051 / I-056 (`!obj` and leftover types TypeError; bytearray mutates). SetSlice/SetItem on None still silent (I-058).

### I-027  `Pyc_DelSlice` reverse-step start underflow; step 0 silent
- Status: fixed
- Severity: wrong-answer
- Evidence: `h=[0,1,2,3,4]; del h[-10::-1]`. CPython `PySlice_AdjustIndices` with step<0 clamps an under-range start to `-1` → slicelength 0 → list unchanged. pyc (`Runtime.cpp:6065-6068`, `6089-6095`) does `s += n` then `if (ss < 0) ss = 0`, so the loop `i > e` (`e` is the omitted-stop sentinel `-1`) visits index 0 and yields `[1, 2, 3, 4]`. Same clamp as pre-existing `Pyc_GetSlice`/`Pyc_SetSlice`. `del h[::0]` is `if (stp == 0) return` (`6063`) vs CPython `ValueError: slice step cannot be zero`. Common reverse cases `del h[::-1]` / `[::-2]` / `[3::-1]` / `[3:0:-1]` compute the right positions.
- Files: `src/runtime/Runtime.cpp` (`Pyc_DelSlice`)
- Blocks merge: no
- Notes: Wave 5 W5.1. `stp == 0` → `ValueError: slice step cannot be zero`. After `s += n`, `s < 0` clamps to `-1` (step<0) or `0` (step>0); reverse loop uses `ss = -1` not `0`. W5.1b closed I-050 (same clamp on GetSlice/SetSlice; extended assign exact-length). Non-int step still ignored (`stp` stays 1).

### I-028  Dead OOM checks after `allocObject` switched to `new`
- Status: fixed
- Severity: latent
- Evidence: `allocObject` is `new PyObject()` (`Runtime.cpp:6705-6709`), which throws `std::bad_alloc` (or terminates under `-fno-exceptions`) and never returns null. Four calloc-era arms are now dead: `runRegexAll` `if (!m)` (`6782`), `PyBuiltin_ReSearch` (`6913`), `PyBuiltin_ReMatch` (`6954`), `PyBuiltin_ReCompile` (`6969`). On OOM the `pcre2_*` handle allocated just above leaks and a C++ exception can escape `extern "C"` — same as every other `new PyObject()` in this file.
- Files: `src/runtime/Runtime.cpp` (`allocObject` callers)
- Blocks merge: no
- Notes: Wave 5 W5.1. Dropped the four `allocObject` null tests (`runRegexAll` / `ReSearch` / `ReMatch` / `ReCompile`). No new function-local static C++ objects (I-049).

### I-030  Remaining boxed-accepting `lowerMethodCall` arms steal user methods
- Status: open
- Severity: wrong-answer
- Evidence: `class C` with `is_file`/`is_dir`/`mkdir`/`joinpath`/`isoformat`/`weekday`/`isoweekday`/`total_seconds`/`group`/`is_integer`/`most_common`/`elements`/`subtract`; also `format(a=1)` / `sort(key=len)`. CPython returns the user methods. pyc: `False`/`False`/`None`/`x`, empty isoformat, `0`/`1`/`0.0`, `None`, `True`, `[('__class__', 0)]`, `[]`, `None`; kwargs `format`/`sort` also stolen. Same result as a direct `C().is_file()` — user instances are `typeOf` `"boxed"`, not only through a parameter.
- Files: `src/Compiler.cpp` (`lowerMethodCall` pathlib block ~9043–9070; datetime ~9078–9096; `group` ~9102; boxed `is_integer` ~9145; `format`/`sort` `hasKeywordArgs` ~9220 / ~9403; Counter ~9477–9516)
- Blocks merge: no
- Notes: Found reviewing W2.1 / I-006. SWE flagged pathlib `is_file`/`is_dir`/`mkdir`/`joinpath`. Same class as the ticket, **not** this ticket’s cases (`exists`/`call`/`bit_length`/`fromkeys`/`unlink`/`isfile`/`isdir`/`check_output` all pass). `.get()` was I-007 (now fixed); leftovers I-032 / I-034. Do not demand in W2.1. Fix is the exists pattern: proven type only at compile time; runtime tag in `Pyc_CallBuiltinMethod`.

### I-031  `fromkeys` / `os.path` AST gates miss aliases and from-imports
- Status: open
- Severity: wrong-answer
- Evidence: `D = dict; D.fromkeys([3])` — CPython `{3: None}`; pyc `AttributeError: 'str' object has no attribute 'fromkeys'` (`dict` is the token `PyBuiltin_DictFactory`). `from os import path; path.exists(".")` — CPython `True`; pyc `AttributeError: 'dict' object has no attribute 'exists'`. `import os as ox; ox.path.exists(".")` and `q = os.path; q.exists(".")` work.
- Files: `src/Compiler.cpp` (`fromkeys` Name==`"dict"` ~9311; `exists`/`isfile`/`isdir` AST `os.path` ~9324)
- Blocks merge: no
- Notes: Found reviewing W2.1 / I-006. The fromkeys miss is caused by this slice (parent name-only `fromkeys` served `D.fromkeys` by accident). `from os import path` was already wrong on the parent (boxed pathlib `exists` → `False`). Ticket CASES use `dict.fromkeys` and `import os` / `import os as`. Not a merge blocker. W2.2 leftover of the same alias/from-import class: I-032.

### I-032  `.get` still stolen / silent on dict-typed non-user-dicts
- Status: open
- Severity: wrong-answer
- Evidence: Gate is AST `Name` + `isImportedModuleName` only (`Compiler.cpp` ~9302–9321). Rebind / boxed / class / submodule / unlisted module all miss it:
  - `m = os; m.get("path")` and `def f(m): return m.get("path"); f(os)` — CPython `AttributeError`; pyc dict.get → the os.path mapping. Runtime tag 2 (`Pyc_CallBuiltinMethod` ~13481) also serves boxed modules as dicts.
  - `C.get(c, "x")` with user `def get(self, k, default=None)` — CPython `user-get:x`; pyc `x` (`typeOf(C)=="dict"`, so the proven-dict arm fires; lookup of the instance as a key misses and the default `"x"` is returned).
  - `os.path.get("exists")` / `from os import path; path.get("exists")` — CPython `AttributeError`; pyc `PyBuiltin_OsPathExists` (path mapping is a dict).
  - `import sys; sys.get("x")`, `getattr(os, "get")`, compiled user module with no `get` — CPython `AttributeError`; pyc `None` (`sys` / user modules are not in `syntheticModuleExports()`, so the raise arm does not fire; dict `Pyc_GetItem`+`Pyc_Apply` yields None). `import math; math.get("pi")` and `import os; os.get("path")` raise, as intended.
- Files: `src/Compiler.cpp` (`get` arms, `isImportedModuleName`, `moduleKnownMissingExport`, `syntheticModuleExports`), `src/runtime/Runtime.cpp` (`Pyc_CallBuiltinMethod` case 2)
- Blocks merge: no
- Notes: Found reviewing W2.2 / I-007. SWE flagged `m = os` (I-031 class) and user-module silent None. Ticket CASES are `C().get` / boxed `f(C())` / `os.get` / `import os as ox` / shadowed `time = {…}; time.get` — those pass. Same hole as I-031, different name. Do not demand in W2.2.

### I-033  Adapter default probe uses param index first
- Status: open
- Severity: wrong-answer
- Evidence: `__apply__` tries `__default_<fn>_<paramIndex>` before `__default_<fn>_<i-firstDef>` (`Codegen.cpp:1119-1121`). Slots are numbered by default-child index 0..ndef-1 (`Compiler.cpp:9816-9817`, same as FunctionDef / `__init__`). When `firstDef > 0` and `ndef > 1`, the first defaulted param’s index equals a later slot: `class C: def foo(self, a=1, b=2): return (a, b); print(C().foo())` — CPython `(1, 2)`; pyc `(2, 2)` at -O0 and -O2. Pre-existing on the same adapter: `def f(x, a=1, b=2): return (a, b); g=f; print(g(0))` → `(2, 2)`; `T=A; T()` for `def __init__(self, a=1, b=2)` → `2 2`. Ticket shape `get(self, k, default=None)` and `get(self, k, default=None, extra=5)` hit `i-firstDef` after a miss and are correct. Two classes with one default each (`A` n=1 / `B` n=2) do not clobber (per-class `methodFuncName` slots).
- Files: `src/codegen/Codegen.cpp` (adapter miss candidates), `src/Compiler.cpp` (method / FunctionDef / `__init__` default slot names)
- Blocks merge: no
- Notes: Found reviewing W2.2 / I-007. Exposed for every instance method because methods now always go through `Pyc_Apply` with newly registered `defaultGlobals`. Not a merge blocker: the ticket CASE has one trailing default. Fix is to probe default-slot index first (or stop probing param index). SWE’s empty-`paramNames`+`CreateUnreachable` trap is gone for regular methods (this slice sets `paramNames`); `__init__` still leaves `paramNames` empty.

### I-034  Remaining dict methods on module namespaces
- Status: open
- Severity: wrong-answer
- Evidence: `import os; os.keys()` / `os.items()` / `os.values()` / `os.pop("path")` — CPython `AttributeError`; pyc `list` / `list` / `list` / the os.path mapping. Arms are `typeOf=="dict"` with no `isImportedModuleName` exclusion (`Compiler.cpp` ~9336 values, ~9350 update, ~9353 pop). `os.environ.get("PATH")` is correctly dict.get (environ is a mapping).
- Files: `src/Compiler.cpp` (`lowerMethodCall` dict arms)
- Blocks merge: no
- Notes: Found reviewing W2.2 / I-007. Pre-existing; this slice only special-cased `.get`. Same class, not this ticket’s cases. Do not demand in W2.2.

### I-035  Class-method defaults lowered in the wrong scope
- Status: open
- Severity: wrong-answer
- Evidence: Method / `__init__` defaults are `lowerExpr`’d in `currentFunc` then `assign`’d to `__module__` (`Compiler.cpp:9751-9754`, `9815-9819`). FunctionDef switches to the outer `saved` scope for both (`Compiler.cpp:352-359`). Nested class: `def outer(): class C: def get(self, k, default=42): return default; return C().get("x")` — CPython `42`; pyc `None`. Same for `default=0.5` and for nested `__init__(self, n=42)` (`C().n` is None). Class body: `class C: x = 7; def get(self, k, default=x): return default` — CPython `7`; pyc `None` (FunctionDefs are lowered before Assign children, so `x` is not the class attr).
- Files: `src/Compiler.cpp` (`lowerClass` method / `__init__` default block)
- Blocks merge: no
- Notes: Found reviewing W2.2 / I-007. `__init__` already had this pattern; the slice copied it for regular methods so `C().get("x")` can omit `default=None`. Top-level `default=None` / `default=1` work. Historic shared-slot bug is not back: slots are `__default_<Class>__<method>_<i>`. Do not demand in W2.2.

### I-036  Traceback snapshot overwritten on reraise / nomatch
- Status: open
- Severity: wrong-answer
- Evidence: `pyc_raise` always does `g_tb_snapshot = g_tb_stack` then, if a try is active, `pyc_tb_unwind(try.tb_depth)` before `longjmp`. A later `pyc_raise` of the same exception (nomatch `pyc_raise(exc)` in `Compiler.cpp` ~8508, bare `pyc_reraise`, or `pyc_materialize_iterator_protocol` propagate) re-snapshots the already-unwound live stack and drops callee frames.
  - `def f(): raise ValueError('inner')` / `try: f()` / `except TypeError: pass` — CPython frames `<module>` + `f`; pyc prints only `<module>`.
  - `try: f()` / `except ValueError: raise` — CPython keeps `f`; pyc bare-reraise snapshots only the catching frame.
- Files: `src/runtime/Runtime.cpp` (`pyc_raise`, `pyc_reraise`, `pyc_tb_snapshot`)
- Blocks merge: no
- Notes: Found reviewing W3.1 / I-009. Caused by this slice; **not** the ticket cases (no `try`). New raise after a successful catch is fine (`tb_depth` unwind prevents stale `f`). Fix is snapshot-once / attach the first snapshot to the exception (CPython) and skip overwrite on reraise. Do not demand in W3.1.

### I-037  Traceback frame names are IR names, not Python names
- Status: open
- Severity: wrong-answer
- Evidence: Codegen `Pyc_PushFrame` uses `f.name` with only `__module__` → `<module>` and `__specialized_*` stripping (`Codegen.cpp` ~1911–1919). Methods are `Class__method` (`Compiler.cpp` ~9797). Nested defs are `__nesteddef_N`. Lambdas are `__lambda_N`.
  - `class C: def foo(self): raise ValueError('m')` / `C().foo()` — CPython `in foo`; pyc `in C__foo`.
  - `def outer():` / `def inner(): raise ValueError('n')` / `inner()` / `outer()` — CPython `in inner`; pyc `in __nesteddef_0`.
- Files: `src/codegen/Codegen.cpp` (I-009 displayName), `src/Compiler.cpp` (`methodFuncName`, nested/lambda IR names)
- Blocks merge: no
- Notes: Found reviewing W3.1 / I-009. Ticket cases are top-level `f` and `<module>` (IR name == Python name). Same class as I-025 (missing-arg TypeError uses IR name). Do not demand in W3.1.

### I-038  SuperMethod builtin fallback is Exception.__init__ only
- Status: open
- Severity: wrong-answer
- Evidence: `PyBuiltin_SuperMethod` (`src/runtime/Runtime.cpp` ~13326–13358) special-cases only `__init__` plus a leftover MRO name in `pyc_str_is_builtin_exc_name`. Anything else still `return nullptr`.
  - `class E(Exception): def __init__(self, m): super().__init__(m)` / `def __str__(self): return 'wrap:' + super().__str__()` / `print(E('boom'))` — CPython `wrap:boom`. pyc: `super().__str__` is not `__init__`, returns None; `'wrap:' + None` is None; `PyObject_Print` then falls through to `pyc_exc_message` → `boom` (or `print(E('boom').__str__())` → `None`).
  - `class L(list): def __init__(self, xs): super().__init__(xs)` / `print(list(L([1,2])))` — CPython `[1, 2]`. pyc: `"list"` is not a builtin-exc name, no-op; instance stays a type-2 dict.
  - `class L(list): def append(self, x): super().append(x)` / `L().append(1)` — CPython mutates the list. pyc: `super().append` is not `__init__`, silent None. Same for `dict` / `super().update`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_SuperMethod`)
- Blocks merge: no
- Notes: Found reviewing W3.2 / I-008. Same class as the ticket (super() into a builtin with no classRegistry entry), **not** this ticket’s cases (`Exception`/`ValueError` `__init__` only). User classes are dict-backed, so list/dict inheritance is also an instance-layout limitation; `__str__` is the clean leftover now that I-008 stores `args`. Do not demand in W3.2.

### I-039  SuperMethod builtin `__init__` returns a type-5 False, not None
- Status: fixed
- Severity: wrong-answer
- Evidence: Fallback arm (`Runtime.cpp` ~13351–13355) does `new PyObject(); type=5; str="None"`. Tag 5 is bool **and** None (I-013); `PyObject_PrintBase` (`~1702`) prints type 5 as `value ? True : False`. Default `value` is 0 → `False`. Repro: `class E(Exception): def __init__(self, m): print(super().__init__(m))` / `E('x')` — CPython `None`; pyc `False`. The `object.__init__` / method-miss path still returns `nullptr`, which *does* print as `None`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_SuperMethod` fallback return)
- Blocks merge: no
- Notes: Wave 5 W5.1. Builtin-exc `__init__` arm now `return nullptr`. `print(super().__init__(m))` is `None`.

### I-040  partition/rpartition non-str sep is ValueError, not TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: `PyString_Partition` / `PyString_RPartition` (`Runtime.cpp` ~3272–3296): any non-str `sep` (including `None`) becomes `delim=""` then `pyc_raise_msg("ValueError", "empty separator")`. CPython ` 'abc'.partition(None) ` / ` .rpartition(1) ` is `TypeError: must be str, not NoneType` / `not int`. Empty *str* sep is correct (`ValueError: empty separator`).
- Files: `src/runtime/Runtime.cpp` (`PyString_Partition`, `PyString_RPartition`)
- Blocks merge: no
- Notes: Wave 5 W5.1. `!sep || type != 3` → `TypeError: must be str, not …` (`pyc_builtin_type_name`). Empty str sep still `ValueError`. W5.1b closed I-052 (split/find/count/replace/startswith/index/join). `rindex` leftover is I-060.

### I-041  str.format nested lookup misses print None
- Status: fixed
- Severity: wrong-answer
- Evidence: `pyc_format_resolve_field` (`Runtime.cpp` ~3351, ~3373) uses non-raising `Pyc_GetAttr` / `Pyc_GetItem`. Miss → `val=nullptr` → `Pyc_FormatValue` → `PyStr_FromAny(nullptr)` → `"None"`. CPython: `'{0[999]}'.format([1,2])` is `IndexError: list index out of range`; `'{0[k]}'.format({})` is `KeyError`; `'{0.missing}'.format(C())` is `AttributeError`. Quoted `'{0[\'k\']}'` is the same miss class (CPython key is the literal `\'k\'`, including quotes — SWE matches that; only the raise is missing).
- Files: `src/runtime/Runtime.cpp` (`pyc_format_resolve_field`)
- Blocks merge: no
- Notes: Wave 5 W5.1. `.attr` miss → AttributeError; `[…]` uses `Pyc_Subscript` (IndexError/KeyError); leftover after `]` → ValueError. W5.1b closed I-053 (base-field miss) and I-055 (present-and-None `.attr`). Plain-dict present-None `.attr` is I-059. `{0[k]}` with a None value still prints `None` (`Pyc_Subscript` returns nullptr without raising; FormatValue stringifies it).

### I-042  str.format still splits on `:` / `!` inside `[…]`
- Status: fixed
- Severity: wrong-answer
- Evidence: `PyBuiltin_StrFormat` still does `inner.find(':')` then `fieldPart.find('!')` (`Runtime.cpp` ~3410–3420) before `pyc_format_resolve_field`. CPython field_name grammar treats `:` / `!` inside `[…]` as index characters: `'{0[a:b]}'.format({'a:b': 1})` → `1`; `'{0[a!b]}'.format({'a!b': 1})` → `1`. pyc takes format_spec `b` / conversion `b` and looks up key `a`. Same parser: `{0[1]foo}` after a closed `]` is silently ignored (CPython `ValueError: Only '.' or '[' may follow ']'`).
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_StrFormat`)
- Blocks merge: no
- Notes: Wave 5 W5.1. Colon/bang scan uses bracket depth; `{0[a:b]}` / `{0[a!b]}` / `{0[1]foo}` match the CASE. `{0[a:b]:d}` and `{0[a!b]!s}` also work (spec/conv at depth 0). Nested `{0:{1}}` is still a different production.

### I-043  GDB printer cannot field-access user locals; tag 7 heuristic inverted
- Status: open
- Severity: limitation
- Evidence:
  - Codegen DI for boxed locals is a 64-bit `DIBasicType` named `"PyObject"`, not a `DICompositeType` (`Codegen.cpp:1371-1375`; no `createStructType` / `createMemberType` anywhere). `tools/pyc_gdb.py` then does `v.dereference()` + `v["type"]` / `v["value"]` / `v["str"]` on that type. GDB will not grow struct fields on a base type. The printer never `gdb.lookup_type("PyObject").pointer()` / `cast`s to the C++ layout from `include/pyc/object_struct.h`. Null `PyObject*` → `"None"` still works (no fields). Type 0/4/5 and containers do not, unless gdb happens to be stopped in a `Runtime.cpp` frame compiled with `-g`.
  - Tag 7: printer treats a **non-empty** `str` as super (`pyc_gdb.py:132-137`). `PyBuiltin_Super` sets `type=7` and `str=""`; `PyTuple_New` also leaves `str` empty. Super therefore prints as `<tuple len=0>`. Distinguisher that matches the runtime (W5.1): tag 7 + non-null `cell_content` is the proxy (`PyBuiltin_Super` sets a self-pointer; compiler never filled this field); tuples leave `cell_content` null.
- Files: `tools/pyc_gdb.py`, `src/codegen/Codegen.cpp` (DI types), `src/runtime/Runtime.cpp` (`PyBuiltin_Super` / `PyTuple_New`)
- Blocks merge: no
- Notes: Found reviewing W3.4 / I-019. `tests/check_gdb.py` only compiles the printer, greps `FlagArtificial`, and (if gdb exists) checks `_lookup` registered — it never `print`s a live local, so this is invisible to the ticket check. FEATURES.md `print x` → `42` is optimistic. `-g` on `runtime.bc` (I-019 leftover) would expose the C++ struct under LTO but would still not retype user-local DI; the printer needs a cast or Codegen needs a 4-field composite (refcount/type/value/dvalue) for scalars. Do not demand in W3.4.

### I-050  `Pyc_GetSlice` / `Pyc_SetSlice` share I-027's reverse underflow and step-0 silent
- Status: fixed
- Severity: wrong-answer
- Evidence: Same clamp as `Pyc_DelSlice` (`Runtime.cpp` GetSlice 5920–5946, SetSlice 6032–6090). `s < 0` then `if (s < 0) s = 0` on a negative step visits index 0; `stp == 0` returns empty / no-op.
  - `[0,1,2,3,4][-10::-1]` — CPython `[]`; pyc `[0]`. `"abcde"[-10::-1]` — CPython `''`; pyc `'a'`.
  - `[0,1,2,3,4][::0]` — CPython `ValueError: slice step cannot be zero`; pyc `[]`.
  - `h[-10::-1] = [9]` — CPython `ValueError` (extended slice length 0 vs 1); pyc writes index 0 → `[9,1,2,3,4]`.
  - `h[::0] = []` — CPython `ValueError`; pyc `if (stp == 0) return`, list unchanged.
- Files: `src/runtime/Runtime.cpp` (`Pyc_GetSlice`, `Pyc_SetSlice`)
- Blocks merge: no
- Notes: Wave 5 W5.1b. `stp == 0` → `ValueError`. After `s += n`, `s < 0` clamps to `-1` (step<0) or `0`. Reverse loop starts at `-1`, not `0`. Extended `SetSlice` requires `repl.size() == positions.size()`. Existing `b[1:4]=[…]` / `c[4:1:-1]=[…]` / `d[1:4:2]=[…]` CASES stay exact-length. Super slice is I-057. Non-int step still stays 1.

### I-051  `Pyc_DelItem` / `Pyc_SetSlice` silent on immutables; bytearray del is a no-op
- Status: fixed
- Severity: wrong-answer
- Evidence: Compiler always emits `Pyc_DelItem` / `Pyc_DelSlice` / `Pyc_SetSlice` (`Compiler.cpp` 7453–7459, 7879–7890). Runtime then:
  - `Pyc_DelItem` (`Runtime.cpp:5154–5169`): only type 2 (dict) and type 1+int (list). Else `return PyBool_New(0)`. `del (5,1,8,3)[1]` / `del "abcd"[1]` / `del b"abcd"[1]` — CPython `TypeError: '…' object doesn't support item deletion`; pyc leaves the object unchanged.
  - `Pyc_DelSlice` (`6115`): `type != 1` returns. Same TypeError miss for tuple/str/bytes slice delete (I-026). `del bytearray(b"abcd")[1:3]` — CPython `bytearray(b'ad')`; pyc unchanged. `del bytearray(b"abcd")[1]` same via DelItem.
  - `Pyc_SetSlice` (`6004`): `type != 1` returns. `t[1:3] = (9,)` / `s[1:3] = "x"` — CPython TypeError; pyc no-op. `ba[1:3] = b"x"` — CPython mutates; pyc no-op.
  - `Pyc_SetItem` (`5100–5116`): list/dict/bytearray-index only. `t[1] = 9` / `s[1] = "x"` / `b"x"[0] = 1` — CPython TypeError; pyc no-op. `ba[i] = n` already works.
- Files: `src/runtime/Runtime.cpp` (`Pyc_DelItem`, `Pyc_DelSlice`, `Pyc_SetSlice`, `Pyc_SetItem`)
- Blocks merge: no
- Notes: Wave 5 W5.1b. `Pyc_DelItem` leftover types TypeError (`pyc_builtin_type_name`); type 18 erases one byte. `Pyc_SetItem` TypeError on type 3/7/17. `Pyc_SetSlice` TypeError unless type 1 or 18; type 18 mutates `obj->str` (basic replace / extended exact-length). `Pyc_DelSlice` now accepts type 18. Wording is `does not` (CPython `doesn't`) — CASE only checks the type name. `None[s:e] = …` / leftover `SetItem` still silent (I-058). Dict slice assign omitted (3.14 hashes slices; I-026).

### I-052  Non-str args to split/find/count/replace/startswith/index/join are silent
- Status: fixed
- Severity: wrong-answer
- Evidence: I-040 is partition treating non-str as empty → `ValueError`. Same sentinel pattern elsewhere, both proven (`PyString_*`) and boxed (`pyc_bm_str_*`):
  - `PyString_Split2` / `RSplit` (`3069`, `3108`): `delim = (sep && sep->type == 3) ? sep->str : " "`. `'a b'.split(1)` / `.rsplit(1)` — CPython `TypeError: must be str or None, not int`; pyc `['a', 'b']`.
  - `PyString_Find` / `RFind` / `Count` / `Index` (`5828–5877`, `4420–4435`): non-str sub → `-1` / `0`. CPython TypeError. `'abc'.index(1)` is `-1` here (comment admits it should be ValueError even for a missing *str*).
  - `PyString_Replace` (`5887`): non-str old/new returns the original string. CPython TypeError.
  - `PyString_StartsWith` / `EndsWith` (`4284`, `4305`): non-str (and not a tuple) → `False`. CPython TypeError.
  - `PyString_Join` (`3450–3456`): non-str items are skipped. `','.join([1, 2])` — CPython TypeError; pyc `','`.
- Files: `src/runtime/Runtime.cpp` (`PyString_Split2`, `PyString_RSplit`, `PyString_Find*`, `PyString_Count`, `PyString_Index`, `PyString_Replace`, `PyString_StartsWith`, `PyString_Join`)
- Blocks merge: no
- Notes: Wave 5 W5.1b. Non-str → `TypeError` via `pyc_builtin_type_name`. `split`/`rsplit` still accept None (`must be str or None`). startswith/endswith keep the tuple-of-str form; a list prefix is now TypeError (matches CPython; existing CASES are tuples). Boxed `pyc_bm_str_*` wrap the same helpers. `rindex` still returns `-1` (I-060).

### I-053  `str.format` base-field miss prints None
- Status: fixed
- Severity: wrong-answer
- Evidence: `pyc_format_resolve_field` (`Runtime.cpp:3323–3343`) leaves `val=nullptr` on OOB auto/index or missing kwargs key. `Pyc_FormatValue` → `PyStr_FromAny(nullptr)` → `"None"`. I-041 is the nested `.attr` / `[…]` walk (non-raising `Pyc_GetAttr`/`Pyc_GetItem`). Base field is the same miss class:
  - `'{1}'.format('a')` — CPython `IndexError`; pyc `'None'`.
  - `'{x}'.format()` — CPython `KeyError`; pyc `'None'`.
  - `'{} {}'.format(1)` — CPython `IndexError`; pyc `'1None'`.
- Files: `src/runtime/Runtime.cpp` (`pyc_format_resolve_field`, `PyBuiltin_StrFormat`)
- Blocks merge: no
- Notes: Wave 5 W5.1b. OOB auto/index → `IndexError: Replacement index out of range for positional args tuple`. Missing kw → `pyc_raise_msg("KeyError", base)` (raw field name; `pyc_exc_message` still repr-quotes string KeyErrors). CASE only prints `type(e).__name__`. FEATURES.md still says base-field misses print None — doc-drift, not this file.

### I-054  Tag-7 super proxy accepts tuple `len` / `in` / subscript / `list()`
- Status: fixed
- Severity: wrong-answer
- Evidence: Tag 7 is tuple **and** super (`I-013`). I-048 is the method table `(7, count)`/`(7, index)`. Operators use the same tag:
  - `PyBuiltin_Len` (`4793`) type 7 → `PyTuple_Size` (empty proxy list) → `0`. CPython `TypeError: object of type 'super' has no len()`.
  - `Pyc_Contains` (`5216–5233`) type 7 scans the empty list → `False`. CPython `TypeError: argument of type 'super' is not iterable`.
  - `Pyc_Subscript` (`5094`) type 7 OOB → `IndexError: tuple index out of range`. CPython `TypeError: 'super' object is not subscriptable`.
  - `PyBuiltin_List` (`6326–6333`) type 7 copies `list` → `[]`. CPython `TypeError: 'super' object is not iterable`.
  Distinguisher matches I-043 / I-048: tag 7 + non-null `cell_content` is the proxy.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Len`, `Pyc_Contains`, `Pyc_Subscript`, `PyBuiltin_List`)
- Blocks merge: no
- Notes: Wave 5 W5.1b. Marker is `cell_content` self-pointer (I-048). Hardcoded `'super'` messages (not `pyc_builtin_type_name`, which still says `"tuple"`). `Pyc_Iter` goes through `PyBuiltin_List`, so `for x in super()` TypeErrors as a bonus. Leftover `GetSlice` / `tuple()` / map/filter is I-057. Not I-038.

### I-055  `str.format` `{0.attr}` raises when the attribute exists and is None
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.1 I-041 does `next = Pyc_GetAttr(val, name); if (!next) AttributeError`. `Pyc_GetItem` on a type-2 instance/class dict returns `pair.second` even when that pointer is nullptr (present None). Class `class C: x = None` stores `nconst` via `Pyc_SetItem` → `PyDict_SetItem` keeps the key with a null value.
  - `'{0.x}'.format(C())` and `c.x = None; '{0.x}'.format(c)` — CPython `'None'`; pyc `AttributeError: 'dict' object has no attribute 'x'`.
  - W5.1 CASE is a **missing** attr (`C` with no `x`) and is correct.
  - `{0[k]}` with `{'k': None}` is fine: `Pyc_Subscript` returns nullptr without raising; FormatValue prints `None`.
- Files: `src/runtime/Runtime.cpp` (`pyc_format_resolve_field` `.attr` arm)
- Blocks merge: no
- Notes: Wave 5 W5.1b. `pyc_format_attr_present` walks a type-2 dict then its `__class__` dict and treats a hit with a null value as present-None. Ticket CASE (class attr + instance attr) matches. Probe is type-2 only and does not require `__class__` — plain dict leftover is I-059.

### I-056  `del None[s:e]` is still a silent no-op
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.1 `Pyc_DelSlice` (`Runtime.cpp:6157–6162`): `if (!obj) return;` then `type != 1` raises TypeError. `None` is nullptr, so `del None[1:3]` returns. CPython `TypeError: 'NoneType' object does not support item deletion`.
- Files: `src/runtime/Runtime.cpp` (`Pyc_DelSlice`)
- Blocks merge: no
- Notes: Wave 5 W5.1b. `if (!obj || (type != 1 && type != 18))` TypeError via `pyc_builtin_type_name(nullptr)` → `"NoneType"`. SetSlice still `if (!obj) return` (I-058).

### I-057  Tag-7 super still a tuple for `GetSlice` / `tuple()` / map/filter
- Status: open
- Severity: wrong-answer
- Evidence: W5.1b I-054 gated `Len` / `Contains` / `Subscript` / `List` on `type==7 && cell_content`. Same marker is missing on:
  - `Pyc_GetSlice` (`Runtime.cpp` type-7 branch): `n = PyTuple_Size` (0) → empty tuple. `s = super(); s[1:3]` / `s[::-1]` — CPython `TypeError: 'super' object is not subscriptable`; pyc `()`.
  - `PyBuiltin_Tuple` (`888`): `if (type == 7) INCREF; return obj` **before** `PyBuiltin_List`. `tuple(super())` — CPython TypeError; pyc the proxy (prints `()`).
  - `PyBuiltin_Map` / `Filter` type-7 arms: scan the empty `list` → `[]`.
- Files: `src/runtime/Runtime.cpp` (`Pyc_GetSlice`, `PyBuiltin_Tuple`, `PyBuiltin_Map`, `PyBuiltin_Filter`)
- Blocks merge: no
- Notes: Found reviewing W5.1b / I-054. SWE flagged GetSlice. Not the ticket CASE (`s[0]` / `list(s)`). `pyc_builtin_type_name` type 7 is still `"tuple"` (DelItem/SetSlice on a proxy would say `'tuple'`). Do not demand in W5.1b.

### I-058  `Pyc_SetSlice` / leftover `Pyc_SetItem` on None still silent
- Status: open
- Severity: wrong-answer
- Evidence: I-056 taught `Pyc_DelSlice` to TypeError on `!obj`. Assignment helpers still return:
  - `Pyc_SetSlice` (`6203`): `if (!obj) return;` then type 1/18 only. `None[1:3] = [1]` — CPython `TypeError: 'NoneType' object does not support item assignment`; pyc no-op.
  - `Pyc_SetItem` (`5246`): `if (!obj || !key) return nullptr;` TypeError only for type 3/7/17. `None[0] = 1` / `(1)[0] = 2` — CPython TypeError; pyc no-op.
- Files: `src/runtime/Runtime.cpp` (`Pyc_SetSlice`, `Pyc_SetItem`)
- Blocks merge: no
- Notes: Found reviewing W5.1b / I-051 / I-056. DelItem leftover types now TypeError (that suspicion matches CPython; not a bug). GetSlice on None still returns `[]` (pre-existing). Do not demand in W5.1b.

### I-059  `pyc_format_attr_present` treats a plain dict as an instance
- Status: open
- Severity: wrong-answer
- Evidence: I-055 probe (`Runtime.cpp:3337`) is `obj->type == 2` then any key hit, including a plain dict (no `__class__`). `Pyc_GetAttr` is `Pyc_GetItem`, so `{0.x}` on a mapping already finds keys.
  - `'{0.x}'.format({'x': None})` — CPython `AttributeError: 'dict' object has no attribute 'x'`. After I-041 this AttributeError'd (`!next`). After I-055 the probe says present → prints `None`.
  - Ticket CASE is a class instance (`__class__` + class-dict `x = None`) and is correct.
- Files: `src/runtime/Runtime.cpp` (`pyc_format_attr_present`)
- Blocks merge: no
- Notes: Found reviewing W5.1b / I-055. Caused by this slice; not a W5.1b CASE. `{0.x}.format({'x': 1})` already printed `1` (GetAttr=GetItem; pre-existing). Probe does not walk `__mro__`; neither does `Pyc_GetItem`'s class fallback. Do not demand in W5.1b.

### I-060  `str.rindex` non-str sub still returns -1
- Status: open
- Severity: wrong-answer
- Evidence: I-052 raised TypeError on `PyString_Index` / `Find*` / `Count`. `PyString_RIndex` (`4561`) is still `if (!sub || type != 3) return -1`. `'abc'.rindex(1)` — CPython `TypeError: must be str, not int`; pyc `-1`.
- Files: `src/runtime/Runtime.cpp` (`PyString_RIndex`)
- Blocks merge: no
- Notes: Found reviewing W5.1b / I-052. Same sentinel class; not in the ticket CASE (`index`/`find` only). Do not demand in W5.1b.

### I-061  `str.find`/`rfind` empty-sub + start==end returns -1
- Status: open
- Severity: wrong-answer
- Evidence: CPython empty string is found at every index in `[start, end]` including `start == end` when `0 <= start <= len`:
  - `"banana".find("", 2, 2)` → `2`; `"banana".find("", 6, 6)` → `6`; `"banana".rfind("", 2, 2)` → `2`.
  `PyString_Find4` (`Runtime.cpp` ~6030) and `PyString_RFind4` (~6074) both `if (en <= st) return -1` before the search. Non-empty `"banana".find("a", 3, 3)` is `-1` on both (not a bug). Empty-sub with `start < end` already matches (`find("", 2, 3)` → `2`).
- Files: `src/runtime/Runtime.cpp` (`PyString_Find4`, `PyString_RFind4`)
- Blocks merge: no
- Notes: Found reviewing W5.2 / I-045. Same clamp also treats a negative end as an empty range (`"banana".find("a", 0, -1)` — CPython `1`; pyc `-1`) and does not do `start += len`. Pre-existing on RFind4; new on Find4. Not the ticket CASE (`"a", 2, 3`). Do not demand in W5.2.

### I-062  `str.count`/`index`/`startswith`/`endswith` drop start/end
- Status: open
- Severity: wrong-answer
- Evidence: Same class as I-045 before the fix. Table rows are arity-1 (`Compiler.cpp` `builtinMethodRows`: startswith/endswith/count/index → `PyString_Count` / `PyString_Index` / `PyString_StartsWith` / `PyString_EndsWith`). Boxed handlers are `PYC_WRAP1` (a1 and argsList ignored).
  - `"banana".count("a", 2, 3)` — CPython `0`; pyc `3`.
  - `"banana".index("a", 2, 3)` — CPython `ValueError`; pyc `1` (first `'a'`).
  - `"banana".startswith("n", 2, 3)` — CPython `True`; pyc `False`.
  - `"banana".endswith("n", 0, 3)` — CPython `True`; pyc `False`.
- Files: `src/Compiler.cpp` (`builtinMethodRows`), `src/runtime/Runtime.cpp` (`pyc_bm_str_count` / `_index` / `_startswith` / `_endswith`)
- Blocks merge: no
- Notes: Found reviewing W5.2 / I-045. `find`/`rfind` now keep end; these four still do not. Not this ticket. Do not demand in W5.2.

---

## Closed (already fixed; listed so agents do not re-open them)

### I-015  Boxed-receiver method fallback ~7× slower
- Status: fixed
- Severity: limitation
- Evidence: Runner 684/684. W4.1: `Pyc_CallMethodOrBuiltin0/1/2` skips the compiler-side args list. W4.2: `(tag, name)` hash lookup, heap tables (function-local static maps SEGV under runtime.bc LTO — I-049). o2_smoke 2/2. SWE measured boxed `.count` ~3.5× proven (was ~7×).
- Files: `src/Compiler.cpp`, `src/runtime/Runtime.cpp`, `src/codegen/Codegen.cpp`, `include/pyc/runtime.h`
- Notes: Wave 4 W4.1 + W4.2. SWR: no merge blockers. Leftovers I-045–I-049.

### I-019  Debug-info leftovers
- Status: fixed
- Severity: limitation
- Evidence: Runner 682/682. `tests/check_gdb.py` 3/3 (printer syntax, FlagArtificial in Codegen, gdb loads printer). `tools/pyc_gdb.py` registered; A6 `__specialized_*` DISubprograms are `FlagArtificial`; README `-g` documents the printer.
- Files: `src/codegen/Codegen.cpp`, `tools/pyc_gdb.py`, `tests/check_gdb.py`
- Notes: Wave 3 W3.4. SWR: no merge blockers. Leftovers I-043 (printer cannot field-access user locals; tag 7 heuristic) / I-044 (`-g` on `runtime.bc`).

### I-010  str.format nested fields; partition("") is lenient
- Status: fixed
- Severity: limitation
- Evidence: Runner 681/681, `file_case_failures=0`. W3.3 CASES match CPython at -O0; o2_smoke 2/2. Parent: empty partition returned 3-tuples; nested format printed the container.
- Files: `src/runtime/Runtime.cpp` (`PyString_Partition`, `PyString_RPartition`, `pyc_format_resolve_field`, `PyBuiltin_StrFormat`)
- Notes: Wave 3 W3.3. SWR: no merge blockers. Leftovers I-040 / I-041 / I-042.

### I-008  `super().__init__` into a builtin base
- Status: fixed
- Severity: limitation
- Evidence: Runner 674/674, `file_case_failures=0`. W3.2 CASES match CPython at -O0; o2_smoke 2/2. Parent: `MyError:` / ` 7` / `E` / `None None`. `PyBuiltin_SuperMethod` now implements BaseException.__init__ when remaining MRO names include a builtin exception.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_SuperMethod`)
- Notes: Wave 3 W3.2. SWR: no merge blockers. Leftovers I-038 (other builtin supers) / I-039 (return prints False).

### I-009  Uncaught tracebacks lack File/line
- Status: fixed
- Severity: limitation
- Evidence: Runner 668/668, `file_case_failures=0`. `tests/check_traceback.py` 3/3 (`module_raise`, `func_raise`, `index_error`) at -O0; o2_smoke 2/2. Parent printed only the header + `Type: msg`. Frames are now `File "…", line N, in <name>` via a thread-local C stack (`Pyc_PushFrame` / `Pyc_PopFrame` / `Pyc_SetLineno`), snapshotted in `pyc_raise`, printed oldest-first in `pyc_fatal_exception`.
- Files: `src/runtime/Runtime.cpp`, `include/pyc/runtime.h`, `src/codegen/Codegen.cpp`, `tests/check_traceback.py`
- Notes: Wave 3 W3.1. SWR: no merge blockers. Leftovers I-036 (reraise/nomatch overwrite snapshot) / I-037 (IR frame names).

### I-007  `module.get()` dispatched as dict.get
- Status: fixed
- Severity: wrong-answer
- Evidence: Runner 667/667, `file_case_failures=0`. W2.2 CASES (banner `# W2.2 / I-007` in `tests/runner.py`) match CPython at -O0 and -O2: user `C().get("x")` / boxed `f(C())` → `user-get:x`; `d.get` / boxed `g(d)` keep `1 None 9`; `os.get("path")` → `AttributeError`. Coordinator extras `import os as ox; ox.get("path")` and `time = {"a":1}; time.get("a")` also match. Parent (post I-006, `10cc798`) accepted `"boxed"` on the `get` arm: `C().get("x")` → `None`, `os.get("path")` → the os.path mapping. `check_dispatch_chain.py`: 73 arms, 1 exemption (`compile`).
- Files: `src/Compiler.cpp` (`lowerMethodCall` `get` arms, `isImportedModuleName`, `moduleKnownMissingExport`, class-method default/`paramNames` registration)
- Notes: Wave 2 W2.2. Dropped `"boxed"` from the dict.get arm; imported synthetic modules whose export list lacks `get` raise AttributeError instead of generic GetItem+Apply None. Regular methods now register per-class `defaultGlobals` + `paramNames` (same unique `Class__method` keying as `__init__` after the shared-slot fix) so `Pyc_Apply` can omit `default=None`. SWR: no merge blockers. Leftovers I-032 / I-033 / I-034 / I-035. I-006 stays closed; I-030 stays open.

### I-006  Remaining name-only `lowerMethodCall` arms
- Status: fixed
- Severity: latent
- Evidence: Runner 665/665, `file_case_failures=0`. W2.1 CASES (banner in `tests/runner.py`) match CPython at -O0 and -O2: user `C().call/exists/bit_length/fromkeys/unlink/isfile/isdir/check_output`, boxed `f(C())` for those names, boxed `(7).bit_length` / `dict.fromkeys` / `{}.fromkeys`, `os.path.exists/isdir/isfile(".")`, `import os as ox; ox.path.exists`, `Path.exists()` direct and through a parameter, `True.bit_length()`. `check_dispatch_chain.py`: 72 arms after I-006; `exists` exemption retired in W2.3 (`compile` remains). Coordinator added the CASES first; they failed on the parent (`C().call(1)` → `-1`, `C().exists()` → `False`, `C().bit_length()` → `0`, `C().fromkeys([1])` → `{1: None}`).
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
