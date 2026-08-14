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
- Status: fixed
- Severity: limitation
- Evidence: W5.7 CASE (`tests/runner.py` banner `# W5.7 / I-012`):
  `def f(): """hello"""; return 3` → `__name__` `f`, `__doc__` `hello`, `__call__()` `3`.
  `lambda: 1` → `__name__` `<lambda>`, `__doc__` `None`.
  Parent: `Pyc_GetAttr` had no type-11 arms; `noteType(f, "str")` sent `f.__call__()` through the string-token `Pyc_Apply` path.
- Files: `src/runtime/Runtime.cpp` (`Pyc_GetAttr`, `pyc_make_func`, `pyc_call_builtin_method`), `src/Compiler.cpp` (`funcDocstrings`, `noteType(..., "function")`), `src/codegen/Codegen.cpp`, `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 5 W5.7. `__name__` is the last dotted component of the display name. Docstring is the first body `Expr` Constant str. `__call__` is `Pyc_Apply` without prepending self. Method `__name__` on a bound method object is still I-011-class.

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
- Status: fixed
- Severity: limitation
- Evidence: W5.7 CASE (`tests/runner.py` banner `# W5.7 / I-018`):
  `sorted([3, 1, 2], key=cmp_to_key(cmp), reverse=True)` → `[3, 2, 1]`.
  Parent: `PyBuiltin_SortedWithCmp` took only `(lst, cmp)` and the compile-time arm dropped `reverseName`.
- Files: `src/runtime/Runtime.cpp`, `src/Compiler.cpp`, `src/codegen/Codegen.cpp`, `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 5 W5.7. Third arg is reverse; applied after the cmp sort, same as `PyBuiltin_Sorted`.

### I-044  Optional `-g` on `runtime.bc`
- Status: fixed
- Severity: limitation
- Evidence: CMake `option(PYC_RUNTIME_BC_DEBUG "Compile runtime.bc with -g" OFF)`. Default stays off so every `-O2` user binary does not inherit Runtime.cpp line tables.
- Files: `CMakeLists.txt`
- Blocks merge: no
- Notes: Wave 5 W5.7. `-DPYC_RUNTIME_BC_DEBUG=ON` adds `-g` to the bitcode compile. Does not retype user-local DI (I-043).

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
- Status: fixed
- Severity: latent
- Evidence: Guard comment above `g_pyc_bm_tables` in `Runtime.cpp`; AGENTS.md gotcha. The W4.2 heap-table fix is unchanged.
- Files: `src/runtime/Runtime.cpp`, `AGENTS.md`
- Blocks merge: no
- Notes: Wave 5 W5.7. Documentation / guard only. The landmine is still real for any new function-local C++ object.

### I-020  Keyword args dropped by method-call fallback
- Status: fixed
- Severity: limitation
- Evidence: W5.5 CASE (`tests/runner.py` banner `# W5.5 / I-020`):
  `def fs(s): return s.split(maxsplit=1); fs("a b c")` → `['a', 'b c']`.
  `def ff(s): return s.format(x=3); ff("{x}")` → `3`.
  `def fsn(s): return s.split(None, 1); fsn("a  b  c")` → `['a', 'b  c']`.
  Parent: `hasKeywordArgs` kept the str.split/format arms for *any* receiver,
  which stole `C().format(a=1)` (I-030). Dropping that gate without a kwargs
  fallback would have dropped `maxsplit=` / `x=` on boxed strings.
- Files: `src/Compiler.cpp` (`Pyc_CallMethodOrBuiltinKw` emit), `src/runtime/Runtime.cpp` (`Pyc_CallMethodOrBuiltinKw`, `PyString_SplitWhitespace2`, `pyc_bm_str_split`), `src/codegen/Codegen.cpp`, `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 5 W5.5. Proven str still uses the fast path. Boxed kwargs go through `Pyc_CallMethodOrBuiltinKw` (format **kwargs, split/rsplit sep=/maxsplit=). Boxed positional `split(None, n)` uses `PyString_SplitWhitespace2` (left-to-right); it used to ignore `n` and dump every token. User `C().format(a=1)` is mapped onto `C__format` via `instanceClassOf` when the class is known.

### I-021  NUL in str literals is truncated at parse/codegen
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.3 CASE (`tests/runner.py` banner `# W5.3 / I-021`):
  `print(len('a\\x00b'))` / `print(['a\\x00b'])` / `len(chr(0))` / `repr(chr(0))` / `ord(chr(0))` → `3\n['a\\x00b']\n1\n'\\x00'\n0\n`.
  Parser Constant unicode (`PythonParser.cpp` ~69–75): `PyUnicode_AsUTF8AndSize` + `assign(ptr,len)` (same pattern as the bytes branch).
  Codegen `const` handler (`Codegen.cpp` ~2334–2348): `CreateGlobalStringPtr` + `PyUnicode_FromStringAndSize(ptr, s.size())`; LLVM extern `{i8*, i64}` next to `FromString` (~222–228). Same shape as `bytesconst`.
  `PyBuiltin_Chr` (`Runtime.cpp` ~2407–2412): `FromStringAndSize(buf, 1)` so `chr(0)` is length 1, not empty.
  `include/pyc/runtime.h:26` declares it inside `extern "C"`.
  Parent: `AsUTF8` + `std::string(const char*)`; const used `FromString`; `chr(0)` was `''`.
- Files: `src/frontend/PythonParser.cpp`, `src/codegen/Codegen.cpp`, `src/runtime/Runtime.cpp`, `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 5 W5.3. SWR: no merge blockers on this CASE. `print(['a\\x00b'])` / `repr(chr(0))` go through `pyc_format_str_repr` (walks by length, emits `\\x00`), so they are not the `%s` sink. Bare `print('a\\x00b')` and Concat/Repeat rebuilds still strlen-truncate — I-063. FEATURES.md still says literals are truncated; Coordinator should flip that on merge. Empty `''`, boxed `def f(n): return chr(n); f(0)`, and `-O2` use the same three helpers (no native-local gate on this path).

### I-022  Leftover unescaped string quoting after I-001
- Status: fixed
- Severity: wrong-answer
- Evidence: `str(KeyError('a\\nb'))` — `pyc_exc_message` (`src/runtime/Runtime.cpp:12541-12542`) does `"'" + cell_content->str + "'"`; CPython `KeyError.__str__` is `repr(args[0])` → `"'a\\nb'"`. `print([Path('a\\nb')])` — PrintElement type 16 (`src/runtime/Runtime.cpp:1693`) `PosixPath('%s')` interpolates the raw path; CPython is `PosixPath({!r})` → `[PosixPath('a\\nb')]`.
- Files: `src/runtime/Runtime.cpp` (`pyc_exc_message` KeyError arm; PosixPath PrintElement)
- Blocks merge: no
- Notes: Wave 5 W5.1. KeyError arm (`12740–12741`) and Path PrintElement (`1696`) both use `pyc_format_str_repr` (quote-switch + `\\n`). `repr(Path)` still `<object>` (I-017). Decimal `'%s'` unchanged.

### I-023  Dynamic `*args` still binds None for missing required
- Status: fixed
- Severity: wrong-answer
- Evidence: `hadRuntimeStar` skips default injection *and* the new `Pyc_CheckMissingArgs` emit (`Compiler.cpp:5225`, `5293`). `__va_*` then unpacks via `emitForwardCallFromList` → `PyList_GetItemObj` (`Compiler.cpp:4120-4126`); OOB returns `nullptr` (`Runtime.cpp:614-620`), which prints as `None`. CPython `def f(a): f(*mk())` with `mk` returning `[]` is `TypeError: f() missing 1 required positional argument: 'a'`.
- Files: `src/Compiler.cpp` (`ensureVaWrapper` / `emitForwardCallFromList`), `src/runtime/Runtime.cpp` (`PyList_GetItemObj`)
- Blocks merge: no
- Notes: Wave 5 W5.4. `emitForwardCallFromList` now compares `PyList_SizeBoxed` to `nrequired` (`fixed - ndef`) and emits `Pyc_CheckMissingArgs` with `callDisplayName`. Ticket CASE `f(*mk())` / `mk()==[]` is TypeError (name only). Same SizeBoxed+icmp pattern as the existing `*args` tail loop. Leftover: omitted defaults still GetItem OOB → None (I-065). Unexpected kwargs / too-many stay out of scope.

### I-024  Indirect `g(**{})` treats the empty kwargs dict as a positional
- Status: fixed
- Severity: wrong-answer
- Evidence: Indirect lowering appends the merged kwargs dict as the last apply-list element (`Compiler.cpp:5188-5202`). Adapter missing-arg check uses `userLen = list_len - ncells` (`Codegen.cpp:1051-1064`) and does not exclude that trailing dict unless the *target* has `**kwargs` (`hasKwVar` peel is later, `1165-1202`). `def f(a): g=f; g(**{})` → `userLen==1`, `firstDef==1`, no raise; slot 0 is bound to `{}`. Direct `f(**{})` is fine (I-002 dict-spread check). CPython: `TypeError: f() missing 1 required positional argument: 'a'`.
- Files: `src/codegen/Codegen.cpp` (adapter `userLen` / `need.miss`), `src/Compiler.cpp` (indirect kwargs append)
- Blocks merge: no
- Notes: Wave 5 W5.4. First try peeled empty trailing dicts in the adapter and made `g({})` look like `g(**{})`. Reverted: peel only when `hasKwVar`. Follow-up aliases `g=f` via `lambdaAliases` (Assign of a `userDefFunctions` / `callableTokenToSynthetic` RHS) so `g(**{})` is the known-shape I-002 path. CASE now includes `g({})` then `g({1:2})`. First-class / boxed `fn(**{})` still appends the merged dict (I-064). `accumulate` / positional-dict first-class calls are not peeled.

### I-025  Missing-arg TypeError uses IR / bare name, not CPython `__qualname__`
- Status: fixed
- Severity: wrong-answer
- Evidence: Adapter always passes `f.name` (`Codegen.cpp:1091`). Compiler `callDisplayName` is `funcDisplayNames` (the def id) or the IR name (`Compiler.cpp:4339-4342`). Lambdas never register a display name (`lowerLambda` ~6429-6439). Direct nested `inner()` → `inner()`; CPython is `outer.<locals>.inner()`. Indirect nested (`g=inner; g()`) → `__nesteddef_N()`. `f=lambda a: a; f()` is a *direct* call via `lambdaAliases` → `__lambda_N()`; CPython is `<lambda>()`. `emitFuncValue` already builds the qualname for `repr` (`Compiler.cpp:3348-3365`) and does not share it with this path.
- Files: `src/codegen/Codegen.cpp` (adapter `miss.fn`), `src/Compiler.cpp` (`callDisplayName`, `funcDisplayNames`, `lowerLambda`)
- Blocks merge: no
- Notes: Wave 5 W5.4. `IRFunction.displayName` + `funcDisplayNames` store FunctionDef qualname (`outer.<locals>.inner`) and `"<lambda>"`. Adapter `miss.fn` and `emitMissingArgsCheck` use that. Ticket CASE matches CPython. Methods never set `displayName` (still `C__foo`); a def nested in a method does not push the class/method onto `funcQualNameStack` (I-066). I-037 is a different name (`__name__` vs qualname).

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
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.5 CASE (`tests/runner.py` banner `# W5.5 / I-030`):
  user `C` with `is_file` / `isoformat` / `group` / `is_integer` / `most_common` / `format(a=1)` → `user-file` / `user-iso` / `user-group` / `user-int` / `user-mc` / `user-fmt:1`.
  Parent: pathlib/datetime/`group`/`is_integer` arms accepted `typeOf=="boxed"`; `format`/`sort` accepted `hasKeywordArgs` on any receiver. User instances are `"boxed"`.
- Files: `src/Compiler.cpp` (`lowerMethodCall` pathlib/datetime/group/is_integer/format/sort/Counter), `src/runtime/Runtime.cpp` (tag-16/14/15/9/4 table rows + Counter most_common/elements/subtract)
- Blocks merge: no
- Notes: Wave 5 W5.5. Compile-time arms are proven type only. Boxed Path/date/timedelta/Match/float/Counter methods go through `Pyc_CallBuiltinMethod` by tag. `sort(key=)` on a user instance no longer takes the list arm (`isProvenListLike` only). Leftover boxed module `.get` is I-067.

### I-031  `fromkeys` / `os.path` AST gates miss aliases and from-imports
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.5 CASE (`tests/runner.py` banner `# W5.5 / I-031`):
  `D = dict; D.fromkeys([3])` → `{3: None}`. `from os import path; path.exists(".")` → `True`.
  Parent: `dict` was the token `PyBuiltin_DictFactory` (type str); `from os import path` was a dict mapping, not `os.path`.
- Files: `src/Compiler.cpp` (`dict_type` on the factory token; `osPathAliases`; `isOsPathReceiver`; `fromkeys` arm)
- Blocks merge: no
- Notes: Wave 5 W5.5. `noteType(tokenVal, "dict_type")` for `PyBuiltin_DictFactory`; assignment copies it via `typeOf`. `from os import path [as X]` and `q = os.path` populate `osPathAliases`. `exists`/`isfile`/`isdir` fire on `isOsPathReceiver`.

### I-032  `.get` still stolen / silent on dict-typed non-user-dicts
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.5 CASE (`tests/runner.py` banner `# W5.5 / I-032`):
  `m = os; m.get("path")` / `sys.get("x")` / `os.path.get("exists")` / `from os import path; path.get("exists")` / `q = os.path; q.get("exists")` → `AttributeError`.
  `C.get(c, "x")` with user `def get` → `user-get:x`.
  Parent: `get` fired for any `typeOf=="dict"` whose AST base was not a module Name. Class dicts, `m = os`, and the os.path mapping all missed that gate.
- Files: `src/Compiler.cpp` (`isRealDictReceiver`, `propagateReceiverAliases`, dict-namespace raise arm)
- Blocks merge: no
- Notes: Wave 5 W5.5. `isRealDictReceiver` excludes imported modules, `osPathAliases`, and `knownClasses`. The raise arm covers those receivers for every `isDictNamespaceMethod`. Leftover: boxed `def f(m): return m.get("path"); f(os)` and `getattr(os, "get")` — I-067.

### I-033  Adapter default probe uses param index first
- Status: fixed
- Severity: wrong-answer
- Evidence: `__apply__` tries `__default_<fn>_<paramIndex>` before `__default_<fn>_<i-firstDef>` (`Codegen.cpp:1119-1121`). Slots are numbered by default-child index 0..ndef-1 (`Compiler.cpp:9816-9817`, same as FunctionDef / `__init__`). When `firstDef > 0` and `ndef > 1`, the first defaulted param’s index equals a later slot: `class C: def foo(self, a=1, b=2): return (a, b); print(C().foo())` — CPython `(1, 2)`; pyc `(2, 2)` at -O0 and -O2. Pre-existing on the same adapter: `def f(x, a=1, b=2): return (a, b); g=f; print(g(0))` → `(2, 2)`; `T=A; T()` for `def __init__(self, a=1, b=2)` → `2 2`. Ticket shape `get(self, k, default=None)` and `get(self, k, default=None, extra=5)` hit `i-firstDef` after a miss and are correct. Two classes with one default each (`A` n=1 / `B` n=2) do not clobber (per-class `methodFuncName` slots).
- Files: `src/codegen/Codegen.cpp` (adapter miss candidates), `src/Compiler.cpp` (method / FunctionDef / `__init__` default slot names)
- Blocks merge: no
- Notes: Wave 5 W5.4. Adapter now pushes `i-firstDef` before param index (`Codegen.cpp` ~1174). `C().foo()` → `(1, 2)`. `C().get("x")` still works: `ndef==1` never had a `__default_*_2` hit, so the old second candidate was already slot 0. CASE `g=f; g(0)` is now a *direct* call via the I-024 alias (defaults injected at the call site); the adapter path is still the `C().foo()` half. `T=A; T()` stays a class-name miss (no `lambdaAliases` for `knownClasses`).

### I-034  Remaining dict methods on module namespaces
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.5 CASE (`tests/runner.py` banner `# W5.5 / I-034`):
  `os.keys()` / `os.pop("path")` → `AttributeError`.
  Parent: `keys`/`items`/`values`/`pop`/`update` fired on `typeOf=="dict"` with no module exclusion. `os.environ.get("PATH")` is still dict.get (environ is a mapping).
- Files: `src/Compiler.cpp` (`isRealDictReceiver` on table Dict rows + values/update/pop; dict-namespace raise arm)
- Blocks merge: no
- Notes: Wave 5 W5.5. Same gate as I-032. `check_dispatch_chain.py` NARROWING now includes `isRealDictReceiver` so the dict `pop` arm does not look like a catch-all in front of list `pop`.

### I-035  Class-method defaults lowered in the wrong scope
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.6 CASE (`tests/runner.py` banner `# W5.6 / I-035`):
  nested `default=42` / `default=0.5` / `__init__(self, n=42)` and
  `class CAttr: x = 7; def get(self, k, default=x)` → `42\n0.5\n42\n7`.
  Parent: defaults were lowered in `currentFunc` then assigned in `__module__`;
  FunctionDefs ran before Assign children.
- Files: `src/Compiler.cpp` (`lowerClassMethodDefaults`, `classBodyNames`, Assign-before-methods)
- Blocks merge: no
- Notes: Wave 5 W5.6. Defaults switch to `__module__` like FunctionDef's outer `saved`. Class attrs are lowered first and bound in `classBodyNames` only while defaults run (method bodies still treat `x` as a global). CASE uses distinct class names — same-name nested classes still share `Class__method` IR (pre-existing).

### I-036  Traceback snapshot overwritten on reraise / nomatch
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.6 `tests/check_traceback.py` `nomatch_keeps_callee` / `bare_reraise_keeps_callee`:
  `try: f()` / `except TypeError: pass` and `except ValueError: raise` keep frames `<module>` + `f`.
  Parent: `pyc_raise` always copied `g_tb_stack` after the try-frame unwind had dropped `f`.
- Files: `src/runtime/Runtime.cpp` (`pyc_raise`)
- Blocks merge: no
- Notes: Wave 5 W5.6. Snapshot only when `g_last_exception != exc` or the snapshot is empty. A new exception still takes a fresh copy. Leftover: two in-flight exceptions (inner raise overwrites the global snapshot of an outer one that is later re-raised) — CPython attaches the traceback to the exception object. Not this CASE.

### I-037  Traceback frame names are IR names, not Python names
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.6 `tests/check_traceback.py` `method_frame_is_co_name` / `nested_frame_is_co_name`:
  `C().foo()` → `in foo`; `outer/inner` → `in inner`.
  Parent: methods had empty `displayName` (`C__foo`); nested defs used the I-025 qualname.
- Files: `src/Compiler.cpp` (`lowerClass` `displayName` = `C.foo`), `src/codegen/Codegen.cpp` (`Pyc_PushFrame` last dotted component)
- Blocks merge: no
- Notes: Wave 5 W5.6. TypeError still uses the qualname (`C.foo()`, `outer.<locals>.inner()`). PushFrame takes the last `.` component so frames are `co_name`. Also closes I-066 (`C().foo()` missing-arg is `C.foo()` not `C__foo()`). Methods push `C.foo` onto `funcQualNameStack` so a def nested in a method gets `C.foo.<locals>.inner`.

### I-038  SuperMethod builtin fallback is Exception.__init__ only
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.7 CASE (`tests/runner.py` banner `# W5.7 / I-038`):
  `class E(Exception): def __str__(self): return 'wrap:' + super().__str__()` / `print(E('boom'))` → `wrap:boom`.
  Parent: only `__init__` was implemented; `__str__` returned None.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_SuperMethod`)
- Blocks merge: no
- Notes: Wave 5 W5.7. `__str__` uses `pyc_exc_message(self)` when a leftover MRO name is a builtin exception. list/dict `super().__init__` / `super().append` is instance-layout (I-013 class), not this ID.

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
- Status: fixed
- Severity: limitation
- Evidence: W5.7 `tests/check_gdb.py`: `createStructType` / `createMemberType` in Codegen; printer uses `cell_content` for tag 7. Codegen DI is a 4-field composite (refcount/type/value/dvalue). Printer casts to `gdb.lookup_type("PyObject")` when the full C++ type is present.
- Files: `src/codegen/Codegen.cpp`, `tools/pyc_gdb.py`, `tests/check_gdb.py`
- Blocks merge: no
- Notes: Wave 5 W5.7. User-local `-g -O0` can field-access the four scalar fields. list/str/cell_content still need runtime.bc `-g` (I-044 option). Tag 7 + non-null `cell_content` is super; empty-str is not.

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

### I-063  Bare print / C-string rebuild still truncates NUL strs
- Status: open
- Severity: wrong-answer
- Evidence: After I-021, a NUL can live in `obj->str`. These sinks still use strlen:
  - `PyObject_PrintBase` type 3 (`Runtime.cpp:1886`): `fprintf(fp, "%s\n", obj->str.c_str())`.
  - `pyc_print` (`3712–3745`): tmpfile + `out += buf` (`operator+=(const char*)` is strlen) then `fwrite(out.data(), 1, out.size())`.
  - `PyStr_FromAny` (`2040`): same tmpfile read then `PyUnicode_FromString(buf)`.
  Repro: `print('a\\x00b')` — CPython writes 4 bytes (`a`, NUL, `b`, newline); pyc writes `a\n`.
  Same class on rebuild: `PyString_Concat` (`2311`) / `PyString_Repeat` (`2318`) / `PyString_Lower` (`3049`) / `PyString_Split*` (`3082`) use `FromString(...c_str())`. `len('a\\x00'+'b')` — CPython `3`; pyc `1`. `lowerJoinedStr` (`Compiler.cpp` ~10162) joins f-string parts via Concat, so `f'a\\x00{x}'` loses the suffix.
- Files: `src/runtime/Runtime.cpp` (`PyObject_PrintBase`, `pyc_print`, `PyStr_FromAny`, `PyString_Concat` / `Repeat` / `Lower` / `Split*`)
- Blocks merge: no
- Notes: Found reviewing W5.3 / I-021. SWE flagged print. Not the ticket CASE (`len` / list-repr / `chr` / `repr` / `ord`). Do not demand in W5.3. Fix is `fwrite(data, 1, size)` on print and `FromStringAndSize` on rebuilds; `pyc_print` must `out.append(buf, n)`, not `+= buf`.

### I-064  First-class / boxed `fn(**{})` still binds `{}` as positional
- Status: open
- Severity: wrong-answer
- Evidence: I-024 only aliases a *Name* assigned from a known def (`lambdaAliases` on Assign, `Compiler.cpp` ~7650–7672). A subscript / parameter / call-result callee is still `buildingIndirectArgs`: merged kwargs dict is appended to the apply list (`Compiler.cpp` ~5344–5358). Adapter peels a trailing dict only when the *target* has `**kwargs` (`Codegen.cpp` ~1204–1210). For `def f(a)`:
  - `hs = [f]; print(repr(hs[0](**{})))` — CPython `TypeError: f() missing 1 required positional argument: 'a'`; pyc prints `{}`.
  - `def apply(fn): return fn(**{})` / `apply(f)` — same bind-as-positional.
  `hs[0]({})` (positional) is fine: no kwargs append, `userLen==1` fills slot 0. Direct `g=f; g(**{})` is the I-024 CASE.
- Files: `src/Compiler.cpp` (indirect kwargs append), `src/codegen/Codegen.cpp` (adapter `hasKwVar` peel / `userLen`)
- Blocks merge: no
- Notes: Found reviewing W5.4 / I-024. Same class as the ticket; not the ticket CASE (`g=f` is now a known-shape call). Do not demand in W5.4. Re-peeling empty dicts for `!hasKwVar` is what broke `g({})` — do not revive that.

### I-065  Dynamic `*args` still skips default injection
- Status: open
- Severity: wrong-answer
- Evidence: `emitForwardCallFromList` (`Compiler.cpp` ~4251–4283) now TypeErrors when `len < nrequired`, then still `PyList_GetItemObj` for every *fixed* param. OOB is `nullptr` → None (`Runtime.cpp` ~616–622). `hadRuntimeStar` still skips call-site default injection (`Compiler.cpp` ~5381).
  - `def f(a, b=2): return (a, b)` / `def mk(): return [1]` / `print(f(*mk()))` — CPython `(1, 2)`; pyc `(1, None)`.
  - `def f(a=1): return a` / `mk()==[]` / `print(f(*mk()))` — CPython `1`; pyc `None` (`nrequired==0`, no check, GetItem 0 OOB).
- Files: `src/Compiler.cpp` (`emitForwardCallFromList`, `hadRuntimeStar` default skip)
- Blocks merge: no
- Notes: Found reviewing W5.4 / I-023. Ticket CASE is required-only `def f(a)` + empty star. Do not demand in W5.4. Fix is to fill `defaults[i]` when `k >= nrequired` and GetItem is OOB (or pass the default slot names into the `__va_` wrapper).

### I-066  Method missing-arg TypeError still uses IR name
- Status: fixed
- Severity: wrong-answer
- Evidence: W5.6 CASE (`tests/runner.py` banner `# W5.6 / I-037 / I-066`):
  `C().foo()` with `def foo(self, a)` → `C.foo() missing 1 required positional argument: 'a'`.
  Parent: methods never set `displayName`; adapter used `C__foo`.
- Files: `src/Compiler.cpp` (`lowerClass` `displayName` / `funcQualNameStack`)
- Blocks merge: no
- Notes: Wave 5 W5.6. Same displayName write as I-037. Nested-in-method FunctionDefs now see `C.foo` on the qualname stack.

### I-067  Boxed module `.get` / `getattr(os, "get")` still dict
- Status: open
- Severity: wrong-answer
- Evidence: I-032 closed the compile-time `Name` / alias / `os.path` gates. A parameter or `getattr` still sees a tag-2 dict:
  - `def f(m): return m.get("path"); f(os)` — CPython `AttributeError`; pyc returns the os.path mapping (`Pyc_CallBuiltinMethod` tables[2]["get"]).
  - `getattr(os, "get")` — CPython `AttributeError`; pyc `None` (`Pyc_GetAttr` → dict miss).
- Files: `src/runtime/Runtime.cpp` (`Pyc_CallBuiltinMethod` tag 2, `Pyc_GetAttr`)
- Blocks merge: no
- Notes: Found finishing W5.5 / I-032. Modules are type-2 dicts at runtime; distinguishing them from user dicts is a layout change (I-013 class). Do not demand in W5.5.

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
- Notes: Wave 1 W1.2. Direct `f()`, `f(**{})`, `g=f; g()`. SWR: no merge blockers. Leftovers I-023 / I-024 / I-025 closed in W5.4 (new leftovers I-064 / I-065 / I-066).

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
