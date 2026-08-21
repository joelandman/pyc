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

### I-229  Out of MVP: stdlib/NumPy, async, lazy generators, threads
- Status: wontfix
- Severity: limitation
- Evidence: FEATURES.md “MVP boundary”. 838/838 is the supported subset.
  Random GitHub Python fails on **imports / async / exec**, not missing `if`.
- Files: FEATURES.md, AGENTS.md (never compile CPython stdlib)
- Blocks merge: no
- Notes: Not tickets. (1) Real stdlib/NumPy — keep `ImportError`; synthetics
  or C shims only; no embedding CPython; NumPy needs a new tag (I-013) and
  buffer protocol (explicitly out). (3) `async`/`await` — coroutine state
  machines + synthetic asyncio; months; don’t compile as sync. (4) Lazy
  `yield` / `itertools.count` — AOT state machines; eager materialize is OK
  for MVP; don’t materialize `count()`. (5) Threads — whole-runtime atomic
  refcounts; multiprocessing needs pickle/IPC. If ever: (4) then (3) then
  (5) then NumPy. I-049 remains a Runtime-author landmine, not an end-user
  feature.

### I-011  `type()` is a display string, not a type object
- Status: fixed
- Severity: limitation
- Evidence: `w011_type.py`: `type(1)` prints `<class 'int'>`; `__name__`
  `int`; `type(1) is type(2)`; `isinstance(1, type(1))`; `type(type(1))`
  is `type`; `type(Dog()) is Dog`. Parent: display strings; `is` False.
- Files: `src/runtime/Runtime.cpp` (`pyc_intern_type`, `PyBuiltin_Type`,
  print, `Pyc_IsInstance`), `src/Compiler.cpp` (`noteType` boxed)
- Blocks merge: no
- Notes: Immortal type-2 dicts (`list_item_type==4`). No new tag (I-013).
  Not full type objects (no `__mro__` on builtins, no `type.__new__`).

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
- Status: fixed
- Severity: limitation
- Evidence: W5.8. Boxed direct calls with a matching `__specialized_*` emit
  null + `type==0/4` guards (`Codegen.cpp` `emitNullAndTag`) then unbox
  field 2/3; miss is the boxed callee. `check_speculative_unbox.py` is the
  IR lock (`pyc_user_main` calls `__specialized_add_ii` behind a type==0
  check). `w58_unbox.py` / CASE banner `# W5.8 / I-014` lock answers
  (bool, indirect `g=add`, mixed float, `global s`). All-native / fib
  self-recursion unchanged. Coordinator: runner 720/720, o2_smoke 2/2,
  nbody 100 matches CPython at `-O0` and `-O2`.
- Files: `src/codegen/Codegen.cpp` (call-site dispatch)
- Blocks merge: no
- Notes: Wave 5 W5.8.   `__apply__` (no `*`/`**`/cells) tag-checks into `__specialized_*`
  for `g=f; g(...)`. Tag 5 / closures / Apply still boxed. Native join
  leftover was I-112 (fixed).

### I-016  Arena allocator / escape analysis / float-return A6
- Status: fixed
- Severity: limitation
- Evidence: `w016_arena.py`, `w016_mutual.py`; `--escape-dump` on nbody/fib.
- Files: `src/ir/IR.cpp` (`analyzeEscapes`), `src/runtime/Runtime.cpp`
  (`Pyc_Arena*`), `src/codegen/Codegen.cpp`, `src/Compiler.cpp`
- Blocks merge: no
- Notes: Non-escaping int/float boxes use a stack-disciplined arena;
  escapers stay on malloc/freelist.   Container stores inherit the container's escape bit. Native float
  return only when the body return type is proven float (all-float
  args are not enough: `cplx(re,im)` returns a list). Mutual recursion
  gets peer `__specialized_*`. List/dict *objects* stay malloc.

### I-017  Datetime / pathlib / bytes / hashlib / struct / decimal subsets
- Status: accepted
- Severity: limitation
- Evidence: FEATURES.md synthetic-module table. Split into later feature tickets I-113–I-120.
- Files: Runtime synthetic modules
- Blocks merge: no
- Notes: Umbrella only. Do not implement as one slice. Children: I-113 µs, I-114 tz, I-115 datetime⊂date, I-116 strptime/strftime/fromisoformat, I-117 pathlib remaining, I-118 open read/readline/close/rb, I-119 hashlib.update, I-120 decimal getcontext.

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
- Status: fixed
- Severity: wrong-answer
- Evidence: W6.1 CASE / `tests/w61_runtime.py`: `s[1:3]` / `tuple(s)` / `map` / `filter` → TypeError.
  Parent: empty tuple / `()`. Gate is `type==7 && cell_content` (I-048 marker). Real tuples unchanged.
- Files: `src/runtime/Runtime.cpp` (`Pyc_GetSlice`, `PyBuiltin_Tuple`, `PyBuiltin_Map`, `PyBuiltin_Filter`)
- Blocks merge: no
- Notes: Wave 6 W6.1. Leftover consumers are I-121. `pyc_builtin_type_name` type 7 is still `"tuple"`.

### I-058  `Pyc_SetSlice` / leftover `Pyc_SetItem` on None still silent
- Status: fixed
- Severity: wrong-answer
- Evidence: W6.1 CASE: `None[1:3]=[1]` / `None[0]=1` → TypeError (parent printed setslice-ok / setitem-ok).
- Files: `src/runtime/Runtime.cpp` (`Pyc_SetSlice`, `Pyc_SetItem`)
- Blocks merge: no
- Notes: Wave 6 W6.1. Leftover SetItem types (`1[0]=2`) are I-122. GetSlice on None still `[]`.

### I-059  `pyc_format_attr_present` treats a plain dict as an instance
- Status: fixed
- Severity: wrong-answer
- Evidence: W6.1 CASE: `'{0.x}'.format({'x': None})` → AttributeError (parent printed `None`).
  Probe now requires a `__class__` entry, then walks instance dict + class dict.
- Files: `src/runtime/Runtime.cpp` (`pyc_format_attr_present`)
- Blocks merge: no
- Notes: Wave 6 W6.1. I-055 instance present-None still matches.

### I-060  `str.rindex` non-str sub still returns -1
- Status: fixed
- Severity: wrong-answer
- Evidence: W6.1 CASE: `'abc'.rindex(1)` → TypeError (parent `-1`).
- Files: `src/runtime/Runtime.cpp` (`PyString_RIndex`)
- Blocks merge: no
- Notes: Wave 6 W6.1. start/end still dropped (I-062).

### I-061  `str.find`/`rfind` empty-sub + start==end returns -1
- Status: fixed
- Severity: wrong-answer
- Evidence: W6.1 CASE: `find("",2,2)` / `find("",6,6)` / `rfind("",2,2)` / `find("a",0,-1)` → `2 6 2 1`.
- Files: `src/runtime/Runtime.cpp` (`pyc_adjust_str_indices`, `PyString_Find4`, `PyString_RFind4`)
- Blocks merge: no
- Notes: Wave 6 W6.1. Find3/RFind3 leftover is I-123.

### I-062  `str.count`/`index`/`startswith`/`endswith` drop start/end
- Status: fixed
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
- Status: fixed
- Severity: wrong-answer
- Evidence: W6.1 CASE: `print('a\\x00b')` writes a,NUL,b; `len('a\\x00'+'b')` is 3.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Wave 6 W6.1. Remaining `FromString(c_str())` rebuilds are I-124.

### I-064  First-class / boxed `fn(**{})` still binds `{}` as positional
- Status: fixed
- Severity: wrong-answer
- Evidence: I-024 only aliases a *Name* assigned from a known def (`lambdaAliases` on Assign, `Compiler.cpp` ~7650–7672). A subscript / parameter / call-result callee is still `buildingIndirectArgs`: merged kwargs dict is appended to the apply list (`Compiler.cpp` ~5344–5358). Adapter peels a trailing dict only when the *target* has `**kwargs` (`Codegen.cpp` ~1204–1210). For `def f(a)`:
  - `hs = [f]; print(repr(hs[0](**{})))` — CPython `TypeError: f() missing 1 required positional argument: 'a'`; pyc prints `{}`.
  - `def apply(fn): return fn(**{})` / `apply(f)` — same bind-as-positional.
  `hs[0]({})` (positional) is fine: no kwargs append, `userLen==1` fills slot 0. Direct `g=f; g(**{})` is the I-024 CASE.
- Files: `src/Compiler.cpp` (indirect kwargs append), `src/codegen/Codegen.cpp` (adapter `hasKwVar` peel / `userLen`)
- Blocks merge: no
- Notes: Found reviewing W5.4 / I-024. Same class as the ticket; not the ticket CASE (`g=f` is now a known-shape call). Do not demand in W5.4. Re-peeling empty dicts for `!hasKwVar` is what broke `g({})` — do not revive that.

### I-065  Dynamic `*args` still skips default injection
- Status: fixed
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
- Status: fixed
- Severity: wrong-answer
- Evidence: I-032 closed the compile-time `Name` / alias / `os.path` gates. A parameter or `getattr` still sees a tag-2 dict:
  - `def f(m): return m.get("path"); f(os)` — CPython `AttributeError`; pyc returns the os.path mapping (`Pyc_CallBuiltinMethod` tables[2]["get"]).
  - `getattr(os, "get")` — CPython `AttributeError`; pyc `None` (`Pyc_GetAttr` → dict miss).
- Files: `src/runtime/Runtime.cpp` (`Pyc_CallBuiltinMethod` tag 2, `Pyc_GetAttr`)
- Blocks merge: no
- Notes: Found finishing W5.5 / I-032. Modules are type-2 dicts at runtime; distinguishing them from user dicts is a layout change (I-013 class). Do not demand in W5.5.

### I-111  PyNumber_* unsupported operands print None
- Status: fixed
- Severity: wrong-answer
- Evidence: W6.1 CASE: `add(None,1)` / `add([1],2)` / `add(1,None)` / `1-[1]` → TypeError.
- Files: `src/runtime/Runtime.cpp` (`pyc_unsupported_binop`, `PyNumber_Add`/`Subtract`/`Multiply`/`Divide`)
- Blocks merge: no
- Notes: Wave 6 W6.1. `//` `/` `%` `**` `Negate` / bitwise leftovers are I-125.

### I-112  Speculative native join look-ahead misses i64assign; fallback unbox unguarded
- Status: fixed
- Severity: latent
- Evidence: W12 / `w112_join.py`: `s=0; s=add(s,x)` in a range loop →
  `4`/`8`. Look-ahead now sees `i64assign`/`f64assign`. Fallback boxed
  result is tag-checked (null / type≠0/4 → 0) and DECREF'd.
- Files: `src/codegen/Codegen.cpp` (`unboxJoinSteal`, assign look-ahead)
- Blocks merge: no
- Notes: Wave 12. Coordinator: runner 821/821, o2_smoke 2/2, nbody 100
  matches at `-O0`/`-O2`. Spec path still uses native `convertToJoin`.

### I-121  Remaining tag-7 super consumers after I-057
- Status: fixed
- Severity: wrong-answer
- Evidence: I-057 gated GetSlice / tuple() / map / filter. Still treating super as an
  empty tuple / non-list: `sorted`/`reversed`/`any`/`all`/`sum`/`enumerate`/`zip`/
  `min`/`max`/`MapN`/`super()+(1,)` / `super()*2` / `bool(super())`.
  CPython: TypeError not iterable (bool is True).
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.1 / I-057. Same `cell_content` marker. Do not TypeError real tuples.

### I-122  `Pyc_SetItem` leftover types still silent
- Status: fixed
- Severity: wrong-answer
- Evidence: I-058 TypeErrors `!obj` and type 3/7/17. Type 0/4/5/20 still
  `return nullptr`. `1[0] = 2` / `True[0] = 1` / `{1}[0] = 2` —
  CPython TypeError; pyc no-op.
- Files: `src/runtime/Runtime.cpp` (`Pyc_SetItem` tail)
- Blocks merge: no
- Notes: Found reviewing W6.1 / I-058.

### I-123  `str.find`/`rfind` 2-arg negative start still clamps to 0
- Status: fixed
- Severity: wrong-answer
- Evidence: I-061 fixed Find4/RFind4. Find3/RFind3 still `if (st < 0) st = 0`.
  `"banana".find("n", -3)` — CPython `4`; pyc `2`.
- Files: `src/runtime/Runtime.cpp` (`PyString_Find3`, `PyString_RFind3`)
- Blocks merge: no
- Notes: Found reviewing W6.1 / I-061. Reuse `pyc_adjust_str_indices`.

### I-124  Remaining C-string str rebuilds after I-063
- Status: fixed
- Severity: wrong-answer
- Evidence: I-063 fixed print / Concat / Repeat / FromAny / Lower / Split*.
  Upper/Strip/Title/ZFill/Center/Replace/Partition/Join/Splitlines still
  `FromString(c_str())`. `"a\\x00b".upper()` / `"".join(["a\\x00","b"])` truncate.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.1 / I-063.

### I-125  Remaining PyNumber_* unsupported operands
- Status: fixed
- Severity: wrong-answer
- Evidence: I-111 TypeErrors Add/Sub/Mul. Still silent:
  `1 // [1]` / `1 / None` / `1 % [1]` / `pow(None, 1)` / `-None` print None;
  `None << 1` / `[1] & 2` print 0.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.1 / I-111. Reuse `pyc_unsupported_binop`. Do not
  break `1//2`, `1/2`, `"%s" % x`, `1<<2`, `{1}|{2}`.

### I-126  Remaining C-string str rebuilds after I-124
- Status: fixed
- Severity: wrong-answer
- Evidence: I-124 closed the listed rebuilds. Still `FromString(c_str())`:
  Casefold/Capitalize/Swapcase/LJust/RJust. `"a\\x00b".ljust(5)` / `.casefold()`
  truncate. format/`%` rebuilds same hole.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.1b / I-124. Sweep remaining str rebuilds.

### I-127  Remaining tag-7 super consumers after I-121
- Status: fixed
- Severity: wrong-answer
- Evidence: `set(super())` → set(); `"".join(super())` → `""`; `super()==()` → True.
  CPython TypeError / False.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.1b / I-121. Same `cell_content` marker.

### I-128  `Pyc_SetItem` None key still silent
- Status: fixed
- Severity: wrong-answer
- Evidence: `if (!key) return nullptr` before the I-122 catch-all. `1[None]=2` —
  CPython TypeError; pyc no-op.
- Files: `src/runtime/Runtime.cpp` (`Pyc_SetItem`)
- Blocks merge: no
- Notes: Found reviewing W6.1b / I-122.

### I-129  print/repr/type of super still an empty tuple
- Status: fixed
- Severity: wrong-answer
- Evidence: `print(super())` / `repr(super())` / `type(super())` → `()` / `'()'` / `<class 'tuple'>`.
  CPython: `<super: <class 'C'>, <C object>>` / `<class 'super'>`.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.1c / I-127.

### I-130  Remaining C-string rebuilds after I-126 (re / textwrap / subprocess)
- Status: fixed
- Severity: wrong-answer
- Evidence: `re.findall` / `m.group` / `re.split` / `re.sub`, `textwrap`, subprocess capture
  still `FromString(c_str())`. `re.findall("a.b", "a\\x00b")` truncates.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.1c / I-126.

### I-131  `str.rindex` still drops start/end
- Status: fixed
- Severity: wrong-answer
- Evidence: `"banana".rindex("n", 0, 3)` — CPython `2`; pyc `4`.
- Files: `src/Compiler.cpp`, `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.2 / I-062. Mirror Index 3/4.

### I-132  `list`/`tuple` `.index` drop start/end
- Status: fixed
- Severity: wrong-answer
- Evidence: `[1,2,1].index(1, 1)` / `(1,2,1).index(1, 1)` — CPython `2`; pyc `0`.
  `list.count` is 1-arg only in CPython — do not add start/end to count.
- Files: `src/Compiler.cpp`, `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Found reviewing W6.2 / I-062. `deque.count` is 1-arg only.

### I-133  Boxed `fn(*[...])` literal star is dropped
- Status: fixed
- Severity: wrong-answer
- Evidence: W7.1 CASE / `w71_call.py`: `hs[0](*[1])` → `1`.
  Parent: TypeError missing `a` (literal star landed in `argRes`; Apply used empty `indirectArgListTemp`).
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 7 W7.1. Also covers `*(1,)`, `hs[0](1, *[2])`, `xs=[1]; hs[0](*xs)` (I-140). Coordinator: `-O0`/`-O2` match CPython.

### I-134  First-class named kwargs / non-empty `**dict` still bind the dict
- Status: fixed
- Severity: wrong-answer
- Evidence: W7.1b CASE / `w71b_kw.py`: `hs[0](a=1)` / `hs[0](**{"a": 1})` / `apply(f)` → `1`.
  Parent: bound the merged dict as positional.
- Files: `src/Compiler.cpp`, `src/codegen/Codegen.cpp`, `src/runtime/Runtime.cpp` (`Pyc_ApplyKw`), `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 7 W7.1b. Kwargs are a third Apply argument, not a trailing list element — `hs[0]({})` / `hs[0]({"a": 1})` stay positional. Empty `**{}` still TypeError (I-064). Coordinator: `-O0`/`-O2` match CPython. Multi-kw / extra keys closed as I-144.

### I-135  `__va_` first-star-wins: `**kwargs` is `*args`
- Status: fixed
- Severity: crash
- Evidence: W7.1: `def f135(a, *args, **kw)` / `f135(*mk135())` with `mk135()==[1]` compiles and prints `1` / `0` / `{}`.
  Parent: LLVM "Incorrect number of arguments" (`emitForwardCallFromList` `ps[j][0]=='*'`). Literal `f(*[1])` was statically expanded and was not the crash.
- Files: `src/Compiler.cpp` (`emitForwardCallFromList`)
- Blocks merge: no
- Notes: Wave 7 W7.1. One star vs two stars. `*args` is still a list (not this ticket).

### I-136  Dynamic `*` splice only walks type-1 lists
- Status: fixed
- Severity: wrong-answer
- Evidence: W7.1: `f(*mk_tup())` with `mk_tup()==(1,)` → `1`.
  Parent: TypeError (`PyList_SizeBoxed` type-1 only).
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 7 W7.1. Starred value goes through `PyBuiltin_List` first. Also closes str/set star (I-141). Super (`type==7 && cell_content`) still TypeErrors.

### I-137  Dynamic `*args` plus keywords are dropped
- Status: fixed
- Severity: wrong-answer
- Evidence: W7.1: `g(*mk(), b=3)` with `g(a, b=2)` → `(1, 3)`.
  Parent: `(1, 2)` (`funcName` rewritten to `__va_g`).
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 7 W7.1. Forward inline against the real target. `g(*mk(), **{"b": 3})` also matches. Leftover required-kw / unexpected-kw: I-142.

### I-138  Remaining C-string rebuilds after I-130 (os.path / pathlib)
- Status: fixed
- Severity: wrong-answer
- Evidence: W7.2 CASE / `w72_path.py`: `os.path.basename("a\\x00b")` → `'a\\x00b'`.
  Parent: `'a'`.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Wave 7 W7.2. basename/dirname/split/splitext/abspath and Path `.name`/`.suffix`/`.stem` use `FromStringAndSize`. Coordinator: `-O0`/`-O2` match CPython. Leftover `os.path.join`: I-143.

### I-139  `getattr(module, name, default)` still drops default
- Status: fixed
- Severity: wrong-answer
- Evidence: W7.3 CASE / `w73_getattr.py`: `getattr(os, "missing", 99)` → `99`; `getattr(C(), "x", 7)` → `7`; 2-arg module miss still AttributeError.
  Parent: 3-arg ignored; I-067 raised on modules; instance miss printed None.
- Files: `src/Compiler.cpp`, `src/runtime/Runtime.cpp` (`Pyc_GetAttrDefault`), `include/pyc/runtime.h`, `src/codegen/Codegen.cpp` (extern only)
- Blocks merge: no
- Notes: Wave 7 W7.3. Coordinator: `-O0`/`-O2` match CPython. Stored-None vs miss is I-145. Indirect `g=getattr` not this arm.

### I-113  datetime / timedelta microseconds
- Status: open
- Severity: limitation
- Evidence: FEATURES.md. Tag 14/15 store no µs. `timedelta.microseconds` is always 0. `datetime(..., microsecond=)` is dropped.
- Files: `src/runtime/Runtime.cpp` (datetime/timedelta layout)
- Blocks merge: no
- Notes: I-017 child. Feature work; not a close-out. Needs an explicit implement ticket.

### I-114  datetime timezones
- Status: open
- Severity: limitation
- Evidence: FEATURES.md. No `tzinfo` / `timezone` / aware datetimes. `.astimezone` / `utcnow` aware forms unsupported.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: I-017 child. Feature work.

### I-115  `datetime` is not a `date` subclass
- Status: open
- Severity: limitation
- Evidence: FEATURES.md. `isinstance(datetime.now(), date)` is False. Shared tag 14, no MRO.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: I-017 child. Touches I-011/I-013-class type objects. Feature work.

### I-116  datetime `strptime` / `strftime` / `fromisoformat`
- Status: fixed
- Severity: limitation
- Evidence: W10.2 / `w116_dt.py`: `strftime` `%Y-%m-%d` / `%H:%M:%S` / `%%`;
  `date.fromisoformat` / `datetime.fromisoformat` (T and space);
  `strptime` on date and datetime match CPython. Parent: AttributeError
  `'object' has no attribute 'strftime'`.
- Files: `src/runtime/Runtime.cpp`, `src/Compiler.cpp`, `src/codegen/Codegen.cpp`,
  `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 10 W10.2. Coordinator: `-O0`/`-O2` match. `%f` stays I-113;
  offset/`Z` stays I-114. Compact `YYYYMMDD` fromisoformat leftover I-226
  notes. libc `strftime` gives `%a` for free.

### I-117  pathlib PurePath / parts / resolve / glob / multi-arg ctor
- Status: fixed
- Severity: limitation
- Evidence: W10.2 / `w117_path.py`: `Path("a","b","c")` / `PurePath` /
  `.parts` tuple / `.glob("*.txt")` / `.resolve()` match CPython.
  Parent: first-arg only; parts None; PurePath None; glob AttributeError.
- Files: `src/runtime/Runtime.cpp`, `src/Compiler.cpp` (`syntheticModuleExports`),
  `src/codegen/Codegen.cpp`, `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 10 W10.2. Coordinator: `-O0`/`-O2` match. PurePath is tag 16
  (no new type). Leftovers I-226 `rglob` / I-227 `open(Path)`.

### I-118  `open()` `.read` / `.readline` / `.close` / `"rb"`
- Status: fixed
- Severity: limitation
- Evidence: W10.1 / `w118_io.py`: `f.read()` / `readline()` / `readlines()` /
  `read(5)` / `open(..., "rb")` bytes / `close()` then `read()` ValueError /
  `with` read match CPython. Parent: AttributeError `'dict' ... 'read'`.
- Files: `src/runtime/Runtime.cpp`, `src/Compiler.cpp` (file arms),
  `src/codegen/Codegen.cpp`, `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 10 W10.1. Coordinator: `-O0`/`-O2` match; valgrind 0 errors
  on w118. Proven `typeOf=="file"` only (I-030). Leftovers I-222–I-225.

### I-119  hashlib `.update()`
- Status: fixed
- Severity: limitation
- Evidence: W11.3 / `w113_hash.py`: `md5(); update(b"hello"); update(b" world")`
  matches `md5(b"hello world")`. Same for sha1/sha256. Boxed
  `[h].update` / mixed `gu(dict); gu(hash)`. Parent: empty digest /
  AttributeError on boxed hexdigest.
- Files: `src/runtime/Runtime.cpp` (`g_pycHashes`, `PyHashlib_Update`),
  `src/Compiler.cpp` (hashobj `update` arm), `src/codegen/Codegen.cpp`
- Blocks merge: no
- Notes: Wave 11 W11.3. Sidetable, not `(2, "update")`. Recomputes digest
  from accumulated payload. `hash.copy()` not added.

### I-120  decimal `getcontext` / `localcontext`
- Status: fixed
- Severity: limitation
- Evidence: W11.4 / `w114_dec.py`: default prec 28; `prec=5` → `0.33333`;
  `localcontext` prec 3 restores. Parent: AttributeError / no export.
- Files: `src/runtime/Runtime.cpp`, `src/Compiler.cpp` (`syntheticModuleExports`)
- Blocks merge: no
- Notes: Wave 11 W11.4. `.prec` only. Rounding stays ROUND_HALF_EVEN.

### I-140  Boxed `fn(*(1,))` / `fn(1, *[2])` / `fn(*xs)` still drop the star
- Status: fixed
- Severity: wrong-answer
- Evidence: After W7.1, `hs[0](*(1,))` / `hs[0](1, *[2])` / `xs=[1]; hs[0](*xs)` match CPython.
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Filed reviewing W7.1 parent; closed by the I-133 `indirectArgListTemp` append (literal name + List/Tuple + positional prefix).

### I-141  Dynamic `*` on str / set is an empty splice
- Status: fixed
- Severity: wrong-answer
- Evidence: After W7.1, `f(*"ab")` → `('a', 'b')`; `f(*{1})` → `(1, None)`.
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Filed reviewing W7.1 parent; closed by `PyBuiltin_List` before the splice (I-136).

### I-142  Dynamic `*` plus required / unexpected keyword
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.1 / `w81_star.py`: `g(*mk(), b=3)` with required `b` → `(1, 3)`; `g3(*mk(), x=3)` → TypeError.
  Parent: TypeError missing `a` and `b` / silent `(1, 0)`.
- Files: `src/Compiler.cpp` (`emitForwardCallFromList`)
- Blocks merge: no
- Notes: Wave 8 W8.1. Coordinator: `-O0`/`-O2` match CPython. Unexpected `**dict` keys still I-151-class if filed.

### I-143  `os.path.join` still strlen-truncates NUL
- Status: fixed
- Severity: wrong-answer
- Evidence: W7.2 CASE / `w72_path.py`: `os.path.join("a\\x00b", "c")` → `'a\\x00b/c'`.
  Parent: `'a'` (`FromString(out.c_str())`).
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_OsPathJoin`)
- Blocks merge: no
- Notes: Wave 7 W7.3. Coordinator: `-O0`/`-O2` match CPython.

### I-144  Boxed `fn(a=1, b=2)` / extra `**` keys still wrong
- Status: fixed
- Severity: wrong-answer
- Evidence: W7.1b: `hs2[0](a=1, b=2)` → `(1, 2)`; `hs[0](**{"a": 1, "x": 2})` → TypeError.
  Parent: TypeError missing `b` / dict bound as `a`.
- Files: `src/Compiler.cpp`, `src/codegen/Codegen.cpp`, `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Wave 7 W7.1b. Same `Pyc_ApplyKw` channel as I-134.

### I-145  getattr 3-arg treats stored None as miss
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.2 / `w82_runtime.py`: `o.x = None; getattr(o, "x", 7)` → `None`; missing still `7`.
  Parent: both printed `7`.
- Files: `src/runtime/Runtime.cpp` (`Pyc_GetAttrDefault` + `pyc_format_attr_present`)
- Blocks merge: no
- Notes: Wave 8 W8.2. Coordinator: `-O0`/`-O2` match CPython. `hasattr` on stored None is still I-055-class.

### I-146  Dynamic `*` + keyword: multiple values for argument
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.1: `g4(*[1, 9], b=3)` and `g4(*mk2(), b=3)` → TypeError.
  Parent: `(1, 3)` (keyword overwrite).
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 8 W8.1. Coordinator: `-O0`/`-O2` match CPython.

### I-147  Dynamic `*` drops later positionals / later stars
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.1: `f(*mk(), 2)` and `f(*a(), *b())` → `(1, 2)`.
  Parent: dropped the tail / replaced the va list.
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 8 W8.1. Coordinator: `-O0`/`-O2` match CPython. Also fixed prefix `f(1, *mk())` null-slot seed.

### I-148  Dynamic `*` on builtins skips special-case arms
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.1: `print(*mkp())` → `1 2 3`.
  Parent: empty/None print (`call print` with 0 args).
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 8 W8.1. `print` / `min` / `max` / `zip` handled before the va forward. Coordinator: `-O0`/`-O2` match CPython.

### I-149  Dynamic `*` on None / int is an empty splice
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.2: `f(*None)` / `f(*1)` / `list(None)` / `list(1)` → TypeError; `list()` still `[]`.
  Parent: empty splice / `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_List`)
- Blocks merge: no
- Notes: Wave 8 W8.2. Coordinator: `-O0`/`-O2` match CPython. `tuple(None)` / `for x in None` not this ticket.

### I-150  Known-class method kwargs drop the keyword / LLVM arity
- Status: fixed
- Severity: crash
- Evidence: W8.3 / `w83_method.py`: `C().f(a=1)` / `C().f(**{"a": 2})` → `1` / `2`; unexpected named kw TypeError.
  Parent LLVM fail was `**dict` (empty-id Keyword skipped), not stripped `self`.
- Files: `src/Compiler.cpp` (`lowerMethodCall`)
- Blocks merge: no
- Notes: Wave 8 W8.3. Coordinator: `-O0`/`-O2` match CPython. dispatch-chain 77/77. Extra keys inside `**dict` and boxed method fallback leftovers not this ticket.

### I-151  I-148 min/max/zip star arms do not match CPython call forms
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.5 / `w85_star.py`: `min(*mk())` → `1`; `zip(*mz())` → 3-tuples.
  Parent: `[1, 2, 3]` / Zip2 of first two.
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 8 W8.5. Coordinator: `-O0`/`-O2` match CPython. Static `zip(a,b,c)` leftover I-156.

### I-152  hasattr treats stored None as missing
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.4 / `w84_runtime.py`: `hasattr(o, "x")` with `o.x is None` → True.
- Files: `src/runtime/Runtime.cpp` (`Pyc_HasAttr`)
- Blocks merge: no
- Notes: Wave 8 W8.4. Coordinator: `-O0`/`-O2` match CPython.

### I-153  tuple(None) / reversed(None) still empty after I-149
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.4: `tuple(None)` / `reversed(None)` → TypeError; `tuple()` / `tuple("ab")` kept.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Wave 8 W8.4. Coordinator: `-O0`/`-O2` match CPython.

### I-154  list(instance) without __iter__ iterates the attr dict
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.4: `list(C())` → TypeError; plain `list({"a":1})` still keys.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_List`)
- Blocks merge: no
- Notes: Wave 8 W8.4. Coordinator: `-O0`/`-O2` match CPython. `list(C)` the class is a leftover.

### I-155  Dynamic/static `*` + `**dict` still overwrites (I-146 leftover)
- Status: fixed
- Severity: wrong-answer
- Evidence: W8.5: `g4(*mk2(), **{"b": 3})` / `g4(*[1, 9], **{"b": 3})` → TypeError.
  `g(*[1], **{"b": 3})` with unbound `b` still `(1, 3)`.
- Files: `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 8 W8.5. Coordinator: `-O0`/`-O2` match CPython.

### I-156  Static `zip(a, b, c)` is still Zip2
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.1 / `w91_zip.py`: `zip([1,2],[3,4],[5,6])` / unequal / 4-way → N-tuples.
  Parent: Zip2 of the first two.
- Files: `src/Compiler.cpp` (static `zip` arm), `src/runtime/Runtime.cpp` (`PyBuiltin_ZipN`), `src/codegen/Codegen.cpp`, `include/pyc/runtime.h`
- Blocks merge: no
- Notes: Wave 9 W9.1. Coordinator: `-O0`/`-O2` match CPython. Runner 754/754. `argRes.size()==2` still Zip2. 0-arg / 1-arg now ZipN (bonus; parent was a link-fail). First-class `z=zip; z(a,b,c)` via `pyc_adapt_zip`.

### I-157  Dynamic `zip(*tuples)` still empty / list-only
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.1: `zip((1,2),(3,4),(5,6))`, `zip([1,2],(3,4))`, `zip(*[(1,2),…])`, `zip(*mt())`, `zip(*mt2())`.
  Parent: `[]` (`PyList_*` / Zip2 type==1 only).
- Files: `src/runtime/Runtime.cpp` (`pyc_zip_seq_len` / `pyc_zip_seq_item` / Zip2 / ZipN), `src/Compiler.cpp` (`emitZipFromVaList`)
- Blocks merge: no
- Notes: Wave 9 W9.1. Coordinator: `-O0`/`-O2` match CPython. Super (`type==7 && cell_content`) still TypeError (I-013). Non-list/tuple leftover is I-159. Do not re-open I-151.

### I-158  Nested generator calls are unwrapped; `list(int)` drains yield buffer
- Status: fixed
- Severity: latent
- Evidence: W12.4 / `w124_gen.py`: `list(inner())` → `[1, 2]`; `list(1)`
  after a yield → TypeError; `list(g())` → `[7]`. Parent: `[]` / stolen
  buffer / empty.
- Files: `src/Compiler.cpp` (`containsYield` skips nested defs; IR name
  registered), `src/runtime/Runtime.cpp` (`PyBuiltin_List` no drain)
- Blocks merge: no
- Notes: Wave 12 W12.4. Outer was wrongly marked a generator because
  `containsYield` walked into `def inner`. `print(outer())` then
  DECREF'd the real list and printed an empty yield buffer.

### I-159  `zip` of str / dict / set / bytes / None is empty
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.2 / `w92_zip.py` + CASE banner `# W9.2 / I-159`:
  `zip("ab","cd")` / 3-way / mixed / shortest / `zip(b"ab",[1,2])` match CPython;
  `zip(None,[1])` / `zip(1,[1])` → TypeError not `[]`.
  Parent: `pyc_zip_seq_len` type 3/17 → 0; Zip2 `!a||!b` → `[]`.
- Files: `src/runtime/Runtime.cpp` (`pyc_zip_seq_len`, `pyc_zip_seq_item`, Zip2)
- Blocks merge: no
- Notes: Wave 9 W9.2. Coordinator: `-O0`/`-O2` match CPython. Walks str +
  bytes/bytearray. None/int TypeError in the shared helper (ZipN /
  `pyc_adapt_zip` included). Super still TypeError (I-013). Leftover
  dict/set/bool/float is I-161. enumerate still list-only is I-162.
  Do not re-open I-156 / I-157.

### I-160  IMPLEMENTATION.md still says zip is 2-tuples
- Status: fixed
- Severity: doc-drift
- Evidence: IMPLEMENTATION.md tuple-returns list: `enumerate` (2-tuples),
  `zip` (N-tuples). Current Status header notes I-156 / I-160.
  Parent: `enumerate`/`zip` (list of 2-tuples).
- Files: `IMPLEMENTATION.md`
- Blocks merge: no
- Notes: Wave 9 W9.7. Historical log corrected; not a runtime change.

### I-161  `zip` of dict / set / bool / float still empty
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15 / `w915_wa.py`: `zip({1:2},[9])` / `zip({1},[9])` →
  `[(1, 9)]`; `zip(True,[1])` / `zip(1.0,[1])` → TypeError.
  Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`pyc_zip_seq_len`, `pyc_zip_seq_item`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Skips
  class/instance/module. Insertion order, not CPython hash order.
  Leftover I-202 (map/filter empty default).

### I-162  `enumerate` still walks lists only
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.3 / `w93_enum.py` + CASE banner `# W9.3 / I-162`:
  `enumerate((1,2))` / `"ab"` / `b"ab"` / `start=` on list and str match
  CPython; `enumerate(None)` → TypeError not `[]`.
  Parent: `PyBuiltin_Enumerate2`: `!iterable || type != 1` → `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Enumerate2`)
- Blocks merge: no
- Notes: Wave 9 W9.3. Coordinator: `-O0`/`-O2` match CPython. Reuses
  `pyc_zip_seq_len` / `pyc_zip_seq_item`. Super already TypeError (I-121).
  `enumerate(1)` TypeErrors for free. Leftovers I-163–I-167. Do not re-open
  I-159.

### I-163  `reversed` still skips tuple / bytes
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.4 / `w94_rev.py` + CASE banner `# W9.4 / I-163`:
  `reversed((1,2,3))` / `b"ab"` / `bytearray(b"ab")` match CPython;
  `reversed({1,2})` / `reversed(1)` → TypeError not `[]` / `setElems`.
  List + str kept. Parent: type 1 / 3 / 20 only; else `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Reversed`)
- Blocks merge: no
- Notes: Wave 9 W9.4. Coordinator: `-O0`/`-O2` match CPython. Reuses
  `pyc_zip_seq_len` / `pyc_zip_seq_item`. A4 ilist/flist reverse kept.
  Super/None still TypeError (I-013 / I-153). Catch-all is “not
  reversible”. Dict reverse-keys leftover is I-168.

### I-164  `enumerate` of dict / set / bool / float still empty
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `enumerate({1:2})` → `[(0, 1)]`; `enumerate({7})` →
  `[(0, 7)]`; `enumerate(True)` / `enumerate(1.0)` → TypeError.
  Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Enumerate2`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Reuses
  I-161 helpers.

### I-165  First-class `e=enumerate` drops start; `e(None)` is None
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `en=enumerate; list(en([1,2], 5))` → `[(5, 1), (6, 2)]`;
  `en(None)` / `en()` → TypeError. Parent: start dropped / printed None.
- Files: `src/runtime/Runtime.cpp` (`pyc_adapt_enumerate`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Now a
  kw adapter; `start=` forwarded. Direct `enumerate([1,2], 5)` kept.

### I-166  `enumerate(..., start=non-int)` silently uses 0
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `start="x"` / `1.5` / `None` → TypeError;
  `start=True` kept `[(1, 1)]`. Parent: `[(0, 1)]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Enumerate2`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Omitted
  start still 0 via `Enumerate` + boxed 0.

### I-167  `any`/`all`/`sorted`/`sum`/`min`/`max` still list-only
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.5 / `w95_seq.py` + CASE banner `# W9.5 / I-167`:
  `any((0,1))` / `all((1,0))` / `sorted((3,1,2))` / `sorted("bac")` /
  `sum((1,2,3))` / `min((3,1,2))` / `max((3,1,2))` / `any("abc")` /
  `all("")` / `sum((1,2), 10)` match CPython. List forms kept.
  Parent: non-list → empty default (All True, Any False, Sorted [],
  Sum start/0, Min/Max None).
- Files: `src/runtime/Runtime.cpp` (`pyc_is_seq_walk`, `PyBuiltin_Any`,
  `All`, `Sorted`, `Sum2`, `MinList`, `MaxList`)
- Blocks merge: no
- Notes: Wave 9 W9.5. Coordinator: `-O0`/`-O2` match CPython. Reuses
  `pyc_zip_seq_len` / `pyc_zip_seq_item`. Super still TypeError (I-013 /
  I-121). Leftovers I-169–I-174. Do not re-open I-162 / I-163.

### I-168  `reversed` of dict is TypeError, not reverse-keys
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.8 / `w98_adapt.py`: `list(reversed({1: 2}))` /
  `{"a":1,"b":2}` / `{}` → `[1]` / `['b','a']` / `[]`. Tuple form kept.
  Parent: TypeError not reversible.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Reversed`)
- Blocks merge: no
- Notes: Wave 9 W9.8. Coordinator: `-O0`/`-O2` match CPython. Reverse
  insertion-order keys. Modules and instances TypeError (I-154). Class
  objects still walk keys — I-180. Do not re-open I-163.

### I-169  `sum` of bytes / bytearray is 0
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.6 / `w96_seq.py` + CASE banner `# W9.6 / I-169 I-171`:
  `sum(b"ab")` / `sum(bytearray(b"ab"))` / `sum(b"ab", 10)` → `195` / `195` / `205`.
  Parent: Sum2 walked type 7 only; bytes/bytearray returned start/0.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Sum2`)
- Blocks merge: no
- Notes: Wave 9 W9.6. Coordinator: `-O0`/`-O2` match CPython. Walks type
  7/17/18 via `pyc_zip_seq_item`. Still does not walk type 3 (I-170).

### I-170  `sum` of str is 0, not TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.7 / `w97_sum_minmax.py`: `sum("ab")` → TypeError (int+str);
  `sum("")` → `0`; tuple/bytes kept. Parent: type 3 not walked; `addOne`
  replaced a failed `PyNumber_Add` with `0`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Sum2`)
- Blocks merge: no
- Notes: Wave 9 W9.7. Coordinator: `-O0`/`-O2` match CPython 3.14.
  Message is int+str, not the older “can't sum strings”. Leftover
  str/bytes start is I-176. None items skipped is I-177.

### I-171  `SortedWithCmp` still skips tuple / str / bytes
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.6 / `w96_seq.py`: `sorted((3,1,2), key=cmp_to_key(cmp))` /
  `sorted("bac", …)` / `sorted(b"bac", …)` match CPython; list form kept;
  `reverse=True` on tuple → `[3, 2, 1]`.
  Parent: `else return PyList_New(0)`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_SortedWithCmp`)
- Blocks merge: no
- Notes: Wave 9 W9.6. Coordinator: `-O0`/`-O2` match CPython. Same
  `pyc_is_seq_walk` arm as `PyBuiltin_Sorted`. Stored / aliased /
  qualified `cmp_to_key` leftover is I-175.

### I-172  First-class `sum`/`sorted`/`any`/`all` drop extra args; None is None
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.8: `s=sum; s((1,2), 10)` → `13`;
  `so=sorted; so([3,1,2], reverse=True)` → `[3,2,1]`;
  `a=any; a(None)` / `al=all; al(None)` → TypeError.
  Parent: `3` / `[1,2,3]` / `None`.
- Files: `src/runtime/Runtime.cpp` (`pyc_adapt_sum`, `pyc_adapt_sorted`,
  `pyc_adapt_any`, `pyc_adapt_all`, `pyc_register_callable_kw`)
- Blocks merge: no
- Notes: Wave 9 W9.8. Coordinator: `-O0`/`-O2` match CPython. Kw adapters;
  `start=` and `key=` also forwarded. First-class `min`/`max` `default=`
  is I-179. First-class `sorted(None)` / `sum(None)` is I-181. Direct
  `any(None)` still I-174.

### I-173  empty `min`/`max` is None, not TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.7: `min([])` / `min(())` / `min("")` / `max([])` → ValueError;
  `min([], default=99)` → `99`; `min([], default=None)` → `None`.
  Parent: `n==0` → `nullptr` (prints None). Register originally said
  TypeError; CPython 3.14 is ValueError.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_MinList`, `PyBuiltin_MaxList`),
  `src/Compiler.cpp` (omitted default → `Pyc_MissingDefault`)
- Blocks merge: no
- Notes: Wave 9 W9.7. Coordinator: `-O0`/`-O2` match CPython. Sentinel
  distinguishes omitted default vs `default=None` (I-178 closed here).

### I-174  `any`/`all`/`sorted`/`sum`/`min`/`max` of None/int/bool/float/dict
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `any(None)` / `any(1)` / `all(None)` / `sorted(None)` /
  `sum(None)` / `min(None)` → TypeError; `any({1: 0})` → True;
  `min({3, 1, 2})` → 1. Parent: False / True / `[]` / 0 / None.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. None is
  nullptr. Class/instance/module TypeError. Leftover I-202 (map/filter).

### I-175  First-class / aliased / qualified `cmp_to_key` is not a key
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `k=cmp_to_key(cmp); sorted([3,1,2], key=k)` /
  `ctk(cmp)` / `functools.cmp_to_key(cmp)` → `[1, 2, 3]`.
  Parent: stored key did not sort (`[3, 1, 2]`).
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_CmpToKey`, `PyBuiltin_Sorted`,
  `pyc_adapt_cmp_to_key`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Token
  stores the real cmp; Sorted routes to SortedWithCmp. Inline
  `key=cmp_to_key(cmp)` still Compiler `findCmpToKey`. Leftover I-203
  (`k(3) < k(1)` is not callable).

### I-176  `sum` with str/bytes/bytearray start is not TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `sum([], "")` / `sum([], b"")` / `sum([], bytearray())`
  → CPython 3.14 messages; `sum([], 10)` / `sum([1,2], 10)` kept.
  Parent: returned the start.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Sum2`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Rejected
  before walking, including empty iterables.

### I-177  `sum` skips None items
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.8: `sum([1, None])` / `sum([None])` → TypeError;
  `sum([1,2,3])` kept. Parent: `1` / printed None (skipped nullptr).
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Sum2` `addOne`)
- Blocks merge: no
- Notes: Wave 9 W9.8. Coordinator: `-O0`/`-O2` match CPython.
  `PyNumber_Add` TypeErrors int+None.

### I-178  empty `min`/`max` with `default=None` is ValueError
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.7 resume: `Pyc_MissingDefault` sentinel. `min([], default=None)`
  → `None`; `min([])` → ValueError. Parent after first W9.7 landing:
  both were ValueError (None is nullptr).
- Files: `src/runtime/Runtime.cpp`, `src/Compiler.cpp`, `include/pyc/runtime.h`,
  `src/codegen/Codegen.cpp`
- Blocks merge: no
- Notes: Wave 9 W9.7. Coordinator: `-O0`/`-O2` match CPython. First-class
  `m=min; m([], default=None)` still I-179.

### I-179  First-class `min`/`max` drop `default=` / `key=`
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.9 / `w99_adapt.py`: `m=min; m([], default=99)` → `99`;
  `m([], default=None)` → `None`; `m([3,1,2], key=lambda x: -x)` → `3`;
  `mx([], default=0)` → `0`. Parent: ValueError / ValueError / `1` / ValueError.
- Files: `src/runtime/Runtime.cpp` (`pyc_adapt_min`, `pyc_adapt_max`,
  `pyc_kw_has`, `pyc_register_callable_kw`)
- Blocks merge: no
- Notes: Wave 9 W9.9. Coordinator: `-O0`/`-O2` match CPython. Kw adapters.
  `pyc_kw_has` distinguishes omitted default vs `default=None`. Direct
  path is I-173 / I-178. Leftovers I-183 / I-184.

### I-180  `reversed` of a class object walks the class dict
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.9: `class C: pass; list(reversed(C))` → TypeError;
  `list(reversed({1: 2}))` kept `[1]`. Parent: `['__mro__']`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Reversed` type-2 arm)
- Blocks merge: no
- Notes: Wave 9 W9.9. Coordinator: `-O0`/`-O2` match CPython. `__mro__`
  → `'type' object is not reversible`. Instances still `__class__`
  (I-154). Plain dicts still reverse keys (I-168). `list(C)` leftover
  is I-182.

### I-181  First-class `sorted(None)` / `sum(None)` still silent
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.9: `so=sorted; so(None)` / `s=sum; s(None)` → TypeError.
  `so([3,1,2], reverse=True)` / `s((1,2), 10)` kept. Parent: `None` / `0`.
- Files: `src/runtime/Runtime.cpp` (`pyc_adapt_sorted`, `pyc_adapt_sum`)
- Blocks merge: no
- Notes: Wave 9 W9.9. Coordinator: `-O0`/`-O2` match CPython. Direct
  `sorted(None)` / `sum(None)` still I-174.

### I-182  `list`/`tuple`/`sorted`/`set` of a class object walk the class dict
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.10 / `w910_class.py`: `list(C)` / `tuple(C)` / `sorted(C)` /
  `set(C)` → TypeError; `list(C())` / `reversed(C)` kept TypeError;
  `list({1: 2})` kept `[1]`. Parent: `['__mro__']` etc.
- Files: `src/runtime/Runtime.cpp` (`pyc_is_class_dict`, `PyBuiltin_List`,
  `PyBuiltin_Sorted` type-2, `pyc_set_iter_to_list`)
- Blocks merge: no
- Notes: Wave 9 W9.10. Coordinator: `-O0`/`-O2` match CPython. `tuple`
  reuses List. Leftovers I-185 / I-186. Do not re-open I-154 / I-180.

### I-183  First-class `min()` / `max()` 0-arg is None, not TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.10: `m=min; m()` / `mx=max; mx()` → TypeError.
  Parent: printed None.
- Files: `src/runtime/Runtime.cpp` (`pyc_minmax_adapt_ok`, `pyc_adapt_min`,
  `pyc_adapt_max`)
- Blocks merge: no
- Notes: Wave 9 W9.10. Coordinator: `-O0`/`-O2` match CPython. Empty
  iterable is I-173. Direct `min()` is I-186.

### I-184  First-class `min`/`max` `default=` plus extra positionals is silent
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.10: `m(1, 2, default=0)` / `m([1], foo=1)` → TypeError;
  `m([], default=99)` / `m([1, 2])` kept. Parent: `1` / `1`.
- Files: `src/runtime/Runtime.cpp` (`pyc_minmax_adapt_ok`)
- Blocks merge: no
- Notes: Wave 9 W9.10. Coordinator: `-O0`/`-O2` match CPython. Direct
  path is I-186.

### I-185  class object still silent in remaining iter consumers
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.11 / `w911_class.py`: `enumerate(C)` / `zip(C,[1])` /
  `any(C)` / `all(C)` / `min(C)` / `max(C)` / `sorted(C, key=cmp_to_key)`
  → TypeError. Parent: `[]` / False / True / None / `['__mro__']`.
- Files: `src/runtime/Runtime.cpp` (`pyc_zip_seq_len`, `Any`, `All`,
  `MinList`, `MaxList`, `SortedWithCmp`, `Map`/`Filter`)
- Blocks merge: no
- Notes: Wave 9 W9.11. Coordinator: `-O0`/`-O2` match CPython. `map`/`filter`
  gated too. Leftover `"__mro__" in C` / `len(C)` is I-187. `sum(C)` is I-189.

### I-186  Direct `min`/`max` still drop 0-arg / `default=`+extras / unexpected kw
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.11: `min()` / `max()` TypeError (parent compile SEGV);
  `min(1, 2, default=0)` / `min([1], foo=1)` TypeError; `min([], default=99)`
  / `min(1, 2)` / `min([3, 1])` kept.
- Files: `src/Compiler.cpp` (min/max arm)
- Blocks merge: no
- Notes: Wave 9 W9.11. Coordinator: `-O0`/`-O2` match CPython. First-class
  path was I-183 / I-184. Star-args arm leftover not demanded.

### I-187  `"__mro__" in C` / `len(C)` still treat class as mapping
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.12 / `w912_class.py`: `"__mro__" in C` / `len(C)` → TypeError;
  `len({1: 2})` / `1 in {1: 2}` kept. Parent: `True` / `1`.
- Files: `src/runtime/Runtime.cpp` (`Pyc_Contains`, `PyBuiltin_Len`)
- Blocks merge: no
- Notes: Wave 9 W9.12. Coordinator: `-O0`/`-O2` match CPython. Class gate
  after `__contains__`/`__len__` lookup. Leftovers I-190 / I-192.

### I-189  `sum(C)` walks class-dict keys
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.12: `sum(C)` → `'type' object is not iterable`;
  `sum([1, 2, 3])` kept `6`. Parent: `int+str` via `0 + "__mro__"`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Sum2`)
- Blocks merge: no
- Notes: Wave 9 W9.12. Coordinator: `-O0`/`-O2` match CPython. DECREFs
  start/`0` before raise. Leftover I-191 (`sum(C())`).

### I-190  `len(C())` / `x in C()` still treat instance as mapping
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.13 / `w913_inst.py`: `len(C())` / `"__class__" in C()` /
  `1 in C()` → TypeError; `len(D())` / `1 in D()` / `2 in D()` → `5` /
  True / False; `len({1: 2})` / `1 in {1: 2}` / `len(C)` kept.
  Parent: `1` / True / False.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Len`, `Pyc_Contains`,
  `pyc_is_instance_dict`)
- Blocks merge: no
- Notes: Wave 9 W9.13. Coordinator: `-O0`/`-O2` match CPython. Instance
  gate after `__len__`/`__contains__` and I-187 class gate. Leftover
  I-193 (`len(sys)` / `x in sys`). I-191 / I-192 unchanged.

### I-191  `sum(C())` walks instance attr keys
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.14 / `w914_map.py`: `sum(C())` → `'C' object is not iterable`;
  `sum([1, 2, 3])` kept `6`. Parent: TypeError `int + str`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Sum2`)
- Blocks merge: no
- Notes: Wave 9 W9.14. Coordinator: `-O0`/`-O2` match CPython. Leftover
  I-194 (`sum(sys)`).

### I-192  `C[k]` / `C()[k]` still treat class/instance as a mapping
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.14: `C["__mro__"]` / `C()["__class__"]` → TypeError;
  `E()["x"]` / `{1: 2}[1]` kept. Parent: MRO list / class dict.
- Files: `src/runtime/Runtime.cpp` (`Pyc_Subscript`)
- Blocks merge: no
- Notes: Wave 9 W9.14. Coordinator: `-O0`/`-O2` match CPython. Class
  decorator CASE rewritten to attribute access (was `cls['k']`).
  Leftovers I-195 / I-196 / I-198.

### I-193  `len(sys)` / `x in sys` still treat module as mapping
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.14: `len(sys)` / `"argv" in sys` → TypeError;
  `len({1: 2})` / `1 in {1: 2}` / `len(C())` kept. Parent: `4` / True.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Len`, `Pyc_Contains`)
- Blocks merge: no
- Notes: Wave 9 W9.14. Coordinator: `-O0`/`-O2` match CPython.
  `sys.argv` is GetItem, not this.

### I-194  `sum(sys)` walks module keys
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15 / `w915_wa.py`: `sum(sys)` → `'module' object is not iterable`;
  `sum([1, 2, 3])` kept `6`. Parent: `int + str`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Sum2`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython.

### I-195  `sys[k]` still treats module as a mapping
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `sys["argv"]` → TypeError; `{1: 2}[1]` / `sys.argv`
  kept. Parent: returned argv.
- Files: `src/runtime/Runtime.cpp` (`Pyc_Subscript`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython.
  `Pyc_GetItem` not gated.

### I-196  `C[k]=v` / `del C[k]` still mutate class/instance/module dicts
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `C["x"]=1` / `del C["__mro__"]` / `sys["argv"]=1` →
  TypeError; `d[3]=4` / `E()["k"]=9` kept. Parent: assigned / deleted.
- Files: `src/runtime/Runtime.cpp` (`Pyc_SubscriptSetItem`, `Pyc_DelItem`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Class
  gate then `__setitem__`/`__delitem__` then instance/module.
  `Pyc_SetItem` not gated. `__delitem__` on instances included.

### I-197  `sorted`/`map`/`filter`/`set` of instance/module still walk keys
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `sorted(C())` / `sorted(sys)` / `map(str, C())` /
  `filter(None, C())` / `set(C())` → TypeError; `sorted({3:0, 1:0})`
  kept `[1, 3]`. Parent: walked `__class__` / module keys.
- Files: `src/runtime/Runtime.cpp`
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Leftover
  I-202 (map/filter of None/int).

### I-198  class object `E[k]` dispatches instance `__getitem__`
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.15: `E["x"]` → TypeError; `E()["x"]` kept `x`.
  Parent: `x`.
- Files: `src/runtime/Runtime.cpp` (`Pyc_Subscript`)
- Blocks merge: no
- Notes: Wave 9 W9.15. Coordinator: `-O0`/`-O2` match CPython. Class
  gate before `__getitem__`; instance after. Leftover I-201 (`C[:]`).

### I-201  `GetSlice` of class / instance / module / dict is `[]`
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.16 / `w916_slice.py`: `C[:]` / `C()[:]` / `sys[:]` → TypeError;
  `{1: 2}[:]` → KeyError; `[1,2,3][1:]` / `(1,2,3)[1:]` / `"abc"[1:]` kept.
  Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`Pyc_GetSlice`)
- Blocks merge: no
- Notes: Wave 9 W9.16. Coordinator: `-O0`/`-O2` match CPython. Super is
  I-057. None still `[]` (I-058). Leftover I-204 (int/set/bool/float).

### I-202  `map`/`filter` of None/int/bool/float still empty
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.16: `list(map(str, None))` / `map(str, True)` / `map(str, 1)` /
  `filter(None, 1.0)` → TypeError; `map(str, [1,2])` / `filter(None, [0,1,2])`
  / `map(str, {1:2})` kept. Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Map`, `MapN`, `Filter`)
- Blocks merge: no
- Notes: Wave 9 W9.16. Coordinator: `-O0`/`-O2` match CPython. None is
  nullptr. Leftover I-205 (bytes/bytearray still `[]`).

### I-203  `cmp_to_key` wrapper is not callable
- Status: fixed
- Severity: limitation
- Evidence: W9.16: `k=cmp_to_key(cmp); k(3)<k(1)` / `k(1)<k(3)` /
  `k(3)>k(1)` / `k(1)==k(1)` → False/True/True/True; `sorted([3,1,2], key=k)`
  → `[1,2,3]`; `k(3).obj` → 3. Parent: False / `.obj` None.
- Files: `src/runtime/Runtime.cpp` (`pyc_apply_impl`, `PyObject_CompareBool`,
  `PyBuiltin_Sorted`, `pyc_cmp_to_key_parts`)
- Blocks merge: no
- Notes: Wave 9 W9.16. Coordinator: `-O0`/`-O2` match CPython. Factory
  `{"cmp_to_key": cmp}`; K adds `"obj"`. No `__class__`. Leftover I-206.

### I-204  `GetSlice` of int / set / bool / float is `[]`
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.17 / `w917_tail.py`: `n[:]` / `{1}[:]` / `True[:]` / `1.0[:]`
  → TypeError; `[1,2,3][1:]` / `"abc"[1:]` kept. Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`Pyc_GetSlice`)
- Blocks merge: no
- Notes: Wave 9 W9.17. Coordinator: `-O0`/`-O2` match CPython. TypeError
  before step-0 (`1[::0]` TypeError). None still `[]` (I-058). Other
  unknown tags (complex/function) TypeError too.

### I-205  `map`/`filter` of bytes / bytearray is `[]`
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.17: `list(map(str, b"ab"))` → `['97', '98']`;
  `list(filter(None, b"ab"))` → `[97, 98]`; same for bytearray.
  Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Map`, `MapN`, `Filter`)
- Blocks merge: no
- Notes: Wave 9 W9.17. Coordinator: `-O0`/`-O2` match CPython. Items
  are ints 0–255 via `pyc_zip_seq_item`. Leftover I-208 (complex/function).

### I-206  `cmp_to_key` extra-arg / mixed ordering / K-as-key
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.17 (CPython 3.14): `k(3,4)` / `k(3)<1` → TypeError;
  `sorted([3,1,2], key=k(3))` → `[1,2,3]`; `k(3)(1).obj` → `1`;
  `k(3)<k(1)` / `sorted(..., key=k)` kept. Parent: dict / False /
  `[3,1,2]` / None.
- Files: `src/runtime/Runtime.cpp` (`pyc_apply_impl`, `PyObject_CompareBool`)
- Blocks merge: no
- Notes: Wave 9 W9.17. Coordinator: `-O0`/`-O2` match CPython. KeyWrapper
  is callable (rewraps). Mixed eq `k(3)==3` still False (I-209).
  `k(3)()` TypeErrors (arity).

### I-208  `map`/`filter` of complex / function still `[]`
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.18 / `w918_tail.py`: `list(map(str, 1+2j))` / `map(str, f)` /
  `filter(None, 1+2j)` / `filter(None, f)` → TypeError;
  `map(str, [1,2])` / `map(str, b"ab")` kept. Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Map`, `MapN`, `Filter`)
- Blocks merge: no
- Notes: Wave 9 W9.18. Coordinator: `-O0`/`-O2` match CPython. Else tail
  TypeErrors leftover tags. Leftover I-210 (any/sum/zip of same tags).

### I-209  `k(3) == 3` is False, not TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.18: `k(3)==3` / `k(3)!=3` → TypeError; `k(3)<1` kept;
  `k(3)<k(1)` / `k(1)==k(1)` / `sorted(..., key=k)` kept. Parent: False / True.
- Files: `src/runtime/Runtime.cpp` (`PyObject_CompareBool`)
- Blocks merge: no
- Notes: Wave 9 W9.18. Coordinator: `-O0`/`-O2` match CPython. Mixed K
  TypeErrors every op. Leftover I-211 (factory == K).

### I-210  `any`/`all`/`sum`/`zip`/`min` of complex / function still empty-default
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.19 / `w919_tail.py`: `any(1+2j)` / `all(1+2j)` / `sum(f)` /
  `zip(1+2j,[1])` / `enumerate(1+2j)` / `min(1+2j)` / `max(f)` → TypeError;
  `any([0,1])` / `sum([1,2])` / `zip([1],[2])` kept. Parent: False / True /
  0 / [] / [] / None / None.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Any`, `All`, `Sum2`,
  `pyc_zip_seq_len`, `MinList`, `MaxList`)
- Blocks merge: no
- Notes: Wave 9 W9.19. Coordinator: `-O0`/`-O2` match CPython. Else tails
  TypeError leftover tags. Dict/set walks kept. Leftover I-212 (`set` of
  same tags). I-211 stays out.

### I-212  `set` of complex / function still empty
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.20 / `w920_set.py`: `set(1+2j)` / `set(f)` / `set(None)` /
  `set(1)` / `set(True)` / `set(1.0)` → TypeError; `set([3,1,1,2])` /
  `set("ab")` / `set({1:2})` / `set()` kept. Parent: `set()`.
- Files: `src/runtime/Runtime.cpp` (`pyc_set_iter_to_list`)
- Blocks merge: no
- Notes: Wave 9 W9.20. Coordinator: `-O0`/`-O2` match CPython. Leftover
  I-213 first-class `s=set; s(None)` still empty.

### I-213  first-class `set(None)` still empty
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.21 / `w921_tail.py`: `s=set; s(None)` → TypeError;
  `s([1,2])` / `set()` / `set(None)` kept. Parent: `set()`.
- Files: `src/runtime/Runtime.cpp` (`pyc_adapt_set`)
- Blocks merge: no
- Notes: Wave 9 W9.21. Coordinator: `-O0`/`-O2` match CPython. Empty
  args still `set()`.

### I-211  `cmp_to_key` factory `==` K is TypeError, not AttributeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.21: `k==k(3)` → AttributeError; `k(3)==3` TypeError;
  `k(3)<k(1)` / `sorted(..., key=k)` kept. Parent: TypeError.
- Files: `src/runtime/Runtime.cpp` (`PyObject_CompareBool`)
- Blocks merge: no
- Notes: Wave 9 W9.21. Coordinator: `-O0`/`-O2` match CPython. Leftover
  I-219 (`k==k` factory vs factory).

### I-214  `dict.fromkeys` of leftover tags is `{}`
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.21: `dict.fromkeys(1+2j)` → TypeError;
  `dict.fromkeys([1,2], 0)` kept. Parent: `{}`.
- Files: `src/runtime/Runtime.cpp` (`PyDict_FromKeys`)
- Blocks merge: no
- Notes: Wave 9 W9.21. Coordinator: `-O0`/`-O2` match CPython. Walks via
  `pyc_set_iter_to_list` (str/tuple/set too).

### I-215  `collections.deque` of leftover tags is `[]`
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.21: `list(deque(1+2j))` → TypeError; `deque([1,2])` kept.
  Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyCollections_Deque`)
- Blocks merge: no
- Notes: Wave 9 W9.21. Coordinator: `-O0`/`-O2` match CPython. Walks via
  `pyc_set_iter_to_list`. Leftover I-217 (`deque(None)` vs 0-arg).

### I-216  `None[:]` is `[]`, not TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W9.21: `x=None; x[:]` → TypeError; `[1,2][1:]` kept `[2]`.
  Parent: `[]`.
- Files: `src/runtime/Runtime.cpp` (`Pyc_GetSlice`)
- Blocks merge: no
- Notes: Wave 9 W9.21. Coordinator: `-O0`/`-O2` match CPython.

### I-217  `deque(None)` is `[]`, not TypeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W12.1 / `w121_wa.py`: `deque()` → `[]`; `deque(None)` →
  TypeError. Parent: both empty.
- Files: `src/runtime/Runtime.cpp` (`PyCollections_Deque`),
  `src/Compiler.cpp` (`Pyc_MissingDefault` on 0-arg)
- Blocks merge: no
- Notes: Wave 12 W12.1. 0-arg is MissingDefault; None is nullptr.

### I-218  set `|` / `&` / `-` of None is silent
- Status: fixed
- Severity: wrong-answer
- Evidence: W12.1 / `w121_wa.py`: `{1} | None` / `&` / `-` → TypeError;
  `{1} | {2}` kept. Parent: `{1}` / `set()` / `{1}`.
- Files: `src/runtime/Runtime.cpp` (`pyc_set_none_operand`)
- Blocks merge: no
- Notes: Wave 12 W12.1. `^` leftover if it still swallows None.

### I-219  `cmp_to_key` factory `==` factory is True, not AttributeError
- Status: fixed
- Severity: wrong-answer
- Evidence: W12.1 / `w121_wa.py`: `k==k` → AttributeError; `k(3)<k(1)`
  still False. Parent: True.
- Files: `src/runtime/Runtime.cpp` (`PyObject_CompareBool`)
- Blocks merge: no
- Notes: Wave 12 W12.1. Two factories (no `obj`) AttributeError before
  dict eq. Two Ks still cmp (I-203).

### I-222  Boxed / first-class file methods still miss
- Status: fixed
- Severity: wrong-answer
- Evidence: W11.2 / `w112_io.py`: `[open(p)][0].read()` / `.readline()` /
  `.readlines()` / boxed `.write`; mixed `g(C()); g(open(p))` match
  CPython. Parent: AttributeError `'dict' ... 'read'`.
- Files: `src/runtime/Runtime.cpp` (`pyc_call_builtin_method` `g_pycFiles`)
- Blocks merge: no
- Notes: Wave 11 W11.2. Sidetable, not `(2, "read")`. User `C.read` and
  `d.read()` AttributeError kept. Bound `h = f.read; h()` still None
  (I-228). Closed file is erased from `g_pycFiles` so boxed post-close
  is AttributeError, not ValueError.

### I-223  `write(bytes)` in `"wb"` is a silent no-op
- Status: fixed
- Severity: wrong-answer
- Evidence: W11.1 / `w111_io.py`: `open(p,"wb").write(b"hello\\nworld")`
  then `open(p,"rb").read()` → `b'hello\\nworld'`. `write(b"xy")` on text
  and `write("hi")` on `"wb"` → TypeError. `write(bytearray(b"AB"))` on
  `"wb"` → `b'AB'`. Parent: empty file / wrote str / no TypeError.
- Files: `src/runtime/Runtime.cpp` (`pyc_file_write_adapter`)
- Blocks merge: no
- Notes: Wave 11 W11.1. Binary accepts type 17/18; text requires type 3.

### I-224  `readlines()` on `"rb"` is list[str]; closed is `[]`
- Status: fixed
- Severity: wrong-answer
- Evidence: W11.1 / `w111_io.py`: `open(p,"rb").readlines()` →
  `[b'hello\\n', b'world']`. Closed `readlines()` → ValueError (`closed`).
  Parent: `['hello\\n', 'world']` / `[]`.
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_FileReadlines`)
- Blocks merge: no
- Notes: Wave 11 W11.1. Uses `pyc_file_or_unreadable` + `pyc_file_make_data`.

### I-225  `read()` on write-only; `encoding=`; `readline(n)`
- Status: fixed
- Severity: limitation
- Evidence: W12.2 / `w122_enc.py`: `open(p,"w",encoding="utf-8").encoding`
  → `utf-8`; readback `hi`; `open(...,"rb",encoding="utf-8")` ValueError;
  default `open` still works. Parent: encoding dropped; `open(p,encoding=)`
  used the encoding string as fopen mode.
- Files: `src/Compiler.cpp` (`open` + npos), `src/runtime/Runtime.cpp`
  (`PyBuiltin_Open` 3-arg), `src/codegen/Codegen.cpp`
- Blocks merge: no
- Notes: Wave 12 W12.2. Text `f.encoding` stored on the file dict. No
  real codec; payload is still UTF-8 bytes. Binary has no `.encoding`.

### I-226  `Path.rglob` / `**` glob still missing
- Status: fixed
- Severity: limitation
- Evidence: W11 / `w11_rest.py`: `Path(base).rglob("*.txt")` →
  `['a.txt', 'b.txt']`. Parent: `[]` / AttributeError.
- Files: `src/runtime/Runtime.cpp` (`PyPathlib_Rglob`), `src/Compiler.cpp`
- Blocks merge: no
- Notes: Wave 11 W11.6. Recursive walk + fnmatch. `glob.glob("**")` still
  one-directory unless the pattern is `**/…`.

### I-228  Bound `f.read` is a token, not a bound method
- Status: fixed
- Severity: limitation
- Evidence: W12.3 / `w123_bound.py`: `h=f.write; h("hello")` / `f.read`
  / `readline` / `readlines` / `close` match CPython. Parent: `None`.
- Files: `src/runtime/Runtime.cpp` (`pyc_bind_if_file_method`, `pyc_apply_impl`)
- Blocks merge: no
- Notes: Wave 12 W12.3. GetItem on a `g_pycFiles` callable token returns
  `{__pyc_bound_self__, __pyc_bound_token__}`. Apply prepends self unless
  args[0] is already that file (`with` / proven write). StringIO leftover.

### I-227  `open(Path)` / PathLike still rejected
- Status: fixed
- Severity: limitation
- Evidence: W11.1 / `w111_io.py`: `open(Path(p)).read()` → `pathok`.
  Parent: ValueError (Open returned null; FileRead saw closed).
- Files: `src/runtime/Runtime.cpp` (`PyBuiltin_Open`)
- Blocks merge: no
- Notes: Wave 11 W11.1. `pyc_is_path_like` (type 3 or 16); path text in `str`.

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
