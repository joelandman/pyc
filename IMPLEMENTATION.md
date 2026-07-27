# pyc — Implementation Details

Design choices, known limitations, optimization status, and implementation notes.

## Design Choices of Omission

### `exec()` / `eval()` — Intentionally Unsupported
Security implications. No plan to implement.

### Full Import/Module System — Mostly Complete
Package imports (dotted paths, nested packages, namespace packages) and relative
imports (`from . import x`, `from .. import y`) are supported, including
transitive discovery of a package's own imports (a package's source is scanned
for further imports, not just the main script). Modules are cached — top-level
code runs at most once per process, matching CPython's `sys.modules` semantics.
Real CPython stdlib modules beyond the synthetic implementations (`sys`, `re`,
`os`, `subprocess`, `functools`, `cmath`, `time.perf_counter`) are not
compiled — pyc can't compile arbitrary CPython stdlib source — and always
report a clear ImportError instead of attempting to.

### `**kwargs` — Not Yet Implemented
Function calls support `*args` collection and call-site unpacking, but `**kwargs`
keyword expansion is not yet implemented.

### Full First-Class Function Objects — Partial
Functions have identity (`is`, `==`) and repr (`<function f at 0x...>`), but full
first-class object protocol (`__call__`, `__name__`, `__doc__` attributes) is not
implemented.

### Function Specialization — Single-Variant (A6)
The A6 specialization generates a native variant for a function only when **all**
call sites agree on the same numeric type signature. If a function is called with
`(int, int)` at one site and `(float, float)` at another, no variant is generated
and both sites use the boxed path. Generalized multi-dispatch (one variant per
distinct signature) is planned but requires speculative unboxing at call sites
(see "Planned" section below).

### Tail Call Optimization — Not Implemented
Recursive functions use O(n) stack space. No plan to implement TCO.

### Lazy Compilation — Not Implemented
All functions are compiled regardless of whether they are called. Compile time
scales with program size.

### JIT Compilation — Not Implemented
pyc is strictly AOT (Ahead-Of-Time). No runtime compilation or caching.

### `datetime` — No Microseconds, No Timezones, No `date`/`datetime` Subclassing
`date`/`datetime`/`timedelta` are implemented as two new opaque runtime types
(tags 14/15 — `PycDateTime` with a `hasTime` flag distinguishing `date` from
`datetime`, and `PycTimedelta`), heap-allocated via `new PyObject()` rather
than the existing `allocObject()` (see Runtime notes below for why).
`timedelta.microseconds` doesn't exist as a field and always reads back as 0
— sub-second precision was out of scope. There is no `tzinfo` support at all
(naive datetimes only, matching what most compiled-AOT use cases need) and no
`date.fromisoformat()`/`strptime`/`strftime`. `datetime` isn't modeled as a
subclass of `date` (pyc's class system can't reliably back a value with two
different C++ payload shapes depending on a flag the way real CPython's
subclassing does) — `isinstance(some_datetime, date)`-style checks aren't
supported. See Known Limitations below for the separate, more consequential
method-call/parameter-passing caveat.

### `pathlib.Path` — Single-Argument Construction, No `PurePath`/`WindowsPath`, No `.parts`
`Path` is a new runtime type (tag 16), simpler than `datetime`'s: it stores
its text directly in `PyObject.str` (no heap-allocated side struct — a
`Path` is just a string with different dispatch on `/`, attribute reads,
comparisons, and a handful of methods). Only `Path(single_arg)` construction
is supported — real Python's `Path("a", "b", "c")` multi-segment form isn't
(use `/` or `.joinpath()` instead). No `PurePath`/`WindowsPath`/`PosixPath`
class distinction, no `.parts`/`.anchor`/`.drive`/`.suffixes`, no
`.resolve()`/`.glob()`/`.rglob()`/`.with_name()`/`.with_suffix()`, no
`os.PathLike` protocol (a `Path` can't be passed to `open()` — use
`open(str(p))`). `type()` isn't specialized for `Path` (unlike `datetime`,
which does report the right class name) — lower priority since
`isinstance`/`type()` checks on paths are rare. See Known Limitations below
for the same method-call/parameter-passing caveat as `datetime`.

### `hashlib` / `base64` / `struct` — No `bytes` Type, Str-as-Byte-Buffer Throughout
pyc has no distinct `bytes` type — everything is `str` (type 3, unicode).
For these three modules, binary data is represented as a plain `str` whose
characters hold byte values 0-255. This is correct for ASCII/text content
(the dominant real-world use: hashing strings, base64-encoding tokens,
packing/unpacking numeric fields) but is not a faithful `bytes` emulation
— no `b'...'` repr, no distinct type identity, and real CPython's
`hashlib`/`base64` functions actually *reject* a plain `str` argument with
`TypeError` (they require real `bytes`), so pyc's versions are
deliberately more permissive, not behaviorally identical. This was a
conscious, user-approved scoping decision (the alternative — implementing
a real `bytes` type — was explicitly declined as a separate, larger
project) rather than an oversight. `hashlib` has no `.update()` (digests
are computed eagerly at construction, since the data is always fully
known in the call already) and no `.digest()` (raw-bytes form — only
`.hexdigest()`). `struct` supports the common format codes and
endianness prefixes but not native alignment padding or `n`/`N`.

### `statistics` — Partial Exact-Integer Preservation, Not Full `Fraction` Arithmetic
Real CPython's `statistics` module computes internally via `Fraction`
(exact rational arithmetic) and converts back to `int` when the exact
result happens to be a whole number — e.g. `statistics.mean([2, 4]) == 3`
(an `int`, not `3.0`), and even `statistics.pvariance([1,2,3,4,5]) == 2`
(also `int`). pyc replicates this only for the common, checkable case: all
inputs are `int`, and the relevant division divides evenly (checked with
exact 64-bit integer arithmetic, not a real `Fraction` type) — see
`pyc_stats_variance_exact_int`/`PyStatistics_Mean` in `Runtime.cpp`. An
input whose *exact rational* result is an integer despite a non-integer
intermediate mean (a real but rare case for `Fraction`-based arithmetic)
won't match CPython's type. `stdev`/`pstdev` were verified to always
return `float` regardless (even for a perfect-square variance), so no
int-preservation attempt was made there.

## Known Limitations

### Performance
- **Function parameters are still boxed**: Type tracking doesn't know parameter types at lowering
- **Mixed-type code falls back to boxed runtime path**: Native paths only trigger when `resultType` is proven numeric
- **Division by zero handling requires runtime call**: Native would produce inf/nan
- **`**` (power) for non-constant exponents uses boxed `Pyc_Pow`**
- **Only a handful of stdlib modules are implemented, synthetically**: `sys`,
  `re` (PCRE2-backed), `os`, `subprocess`, `functools`, `cmath`,
  `time.perf_counter`, `math`, `json`, `random`, `itertools` (subset),
  `collections` (subset), `datetime` (`date`/`datetime`/`timedelta`, see
  below), `pathlib` (`Path`, see below) — everything else reports
  ImportError rather than compiling real CPython stdlib source. A function
  named `get` called via `module.get()` collides with the dict `.get()`
  method shim and silently returns `None` — a pre-existing naming
  collision, not package-specific.
- **The generic method-call dispatch chain in `Compiler.cpp`'s
  `lowerMethodCall` matches on method *name* only for several branches**
  (`append`/`insert`/`remove`/`index`/`join`/`split`/etc. — the built-in
  list/string method shims), with no check on the receiver's type. This is
  a **real, general naming-collision hazard**: any synthetic module that
  picks a dict key matching one of these names breaks silently. Two
  concrete instances found and fixed while adding `os.path.join`/
  `os.remove`: `os.path.join(...)` was being routed to `PyString_Join`
  (treating the `os.path` dict as a string separator) instead of the
  intended token dispatch, and `os.remove(...)` was similarly routed to
  `PyList_Remove`. Fixed by adding `&& typeOf(obj) != "dict"` guards to
  those two specific branches (`Compiler.cpp`, the `"join"`/`"remove"`
  `methodName` checks) — **not** a general fix; any *future* module using
  another colliding name (`"index"`, `"split"`, `"count"`, `"insert"`,
  ...) will hit the same silent-wrong-dispatch failure mode until it's
  individually discovered and guarded the same way. Always smoke-test a
  new synthetic module's every dict key name against this dispatch chain,
  not just against ImportError/basic call success.
- **`datetime` method calls are not robust to untyped function parameters**:
  unlike `date`/`datetime`/`timedelta`'s attribute reads, arithmetic,
  comparisons, and `str()`, which are dispatched purely on the runtime
  type tag (14/15) and so work regardless of what the compiler could infer
  — method-call *syntax* (`.isoformat()`, `.weekday()`, `.isoweekday()`,
  `.total_seconds()`) is gated on `typeOf(obj)` in `Compiler.cpp`'s
  `lowerMethodCall`, the same dispatch class as the pre-existing
  `Match.group()`. `typeOf` tracking follows construction, plain
  assignment, and function return values, but **not** function parameters
  (confirmed by reading `inferParamTypesFromBody`, which only ever infers
  `int`/`float`/`boxed`) — so `def f(d): return d.isoformat()` returns
  `None` for a `date`/`datetime` `d` passed in as a parameter, even though
  `def f(d): return str(d)` and `def f(d): return d.year` both work
  correctly on the same value. Prefer `str()`/attribute access over method
  calls when a datetime value crosses a function boundary.
- **`pathlib.Path` method calls have the identical limitation**:
  `.exists()`/`.is_file()`/`.is_dir()`/`.mkdir()`/`.joinpath()` are
  `typeOf`-gated the same way as `datetime`'s methods, and don't survive
  an untyped function parameter (`.name`/`.parent`/`.suffix`/`.stem`, `/`,
  comparisons, and `str()` all do, since they route through the universal
  runtime-tag dispatch points).
- **Fixed while adding `pathlib`**: `Compiler.cpp`'s `lowerBinOp` never
  tagged a binary operation's *result temp* with a non-numeric type string
  — so `typeOf` lost track of a value the instant it passed through a
  binop, even when the operand types made the result type statically
  obvious. This didn't matter for `datetime`'s `+`/`-` at the time (nothing
  in that work chained a method call directly onto an arithmetic result),
  but it's a hard blocker for `pathlib.Path`, since `/`-chaining
  (`Path(x) / "sub" / "file.txt"`) is the primary way any real code
  constructs a nested path, and immediately calling `.is_dir()`/`.exists()`
  on the chained result is equally common. Fixed by having `lowerBinOp`
  additionally call `noteType` on the result temp when the operator is
  `truediv` and the left operand is a `"path"`, or when the operator is
  `add`/`sub`/`mul` and an operand is a `"date"`/`"datetime"`/`"timedelta"`
  — this also retroactively fixes the same latent gap for datetime
  arithmetic (`(d + delta).isoformat()` now works without an intermediate
  variable). Purely a compile-time bookkeeping fix — the runtime *values*
  were already correct either way, since arithmetic dispatch itself never
  depended on `typeOf` (see the "robust primitives" design above).
- **Severe pre-existing bug, fixed while investigating `hashlib`'s calling
  conventions: `with open(p, "w") as f: f.write(x)` silently never wrote
  anything.** `open()` creates the file on disk immediately (`fopen(p,
  "w")` truncates/creates on open, before any `write()` call), so the file
  existing was never proof the content was written — a gap in this
  project's own prior testing (`os`/`pathlib` smoke tests only checked
  `os.path.exists()`, never actual file content). Root cause: the
  with-statement's `__enter__`/`__exit__` dispatch (`Compiler.cpp`'s
  `With`-statement lowering) built its args list via
  `ir.addInstruction(..., {"PyList_NewBoxed", "1"}, ...)` — passing the
  *count* as a bare literal string `"1"` instead of a properly-declared IR
  const. `Codegen.cpp`'s `getOrLoad` resolves any operand name it doesn't
  recognize (which an undeclared literal like `"1"` never is) to a null
  pointer, so `PyList_NewBoxed` received a null count and allocated a
  *zero-length* list. `PyList_SetItemBoxed`'s boxed-list path only writes
  when the index is already within the list's current size
  (`Runtime.cpp`'s `PyList_SetItem`), so the intended `self` argument was
  silently dropped — `__enter__` received an empty args list, returned
  `None` (its own empty-args guard clause), and the with-target variable
  was bound to `None` instead of the file object. Every method call
  *inside* the with-block (`f.write(...)`) was therefore operating on
  `None`, dispatched through the generic `typeOf(obj)=="dict"` fallback
  (which doesn't prepend a receiver anyway — see below), silently
  returning `None` without error. Fixed by declaring proper `const`
  temps for the arg-list counts in both `__enter__` and `__exit__`
  dispatch, and by giving `open()`'s result its own `"file"` typeOf tag
  with a dedicated `.write()` dispatch branch (mirroring `datetime`/
  `pathlib`'s typeOf-gated methods) that explicitly prepends the receiver
  — the generic dict-dispatch fallback is designed for **non-bound**
  module-namespace calls (`os.path.exists(path)`, where the receiver
  genuinely isn't a "self") and must not be changed to always prepend a
  receiver, which would break every one of those. Only 2 other call
  sites in the whole file used the same bare-literal-count anti-pattern
  (both in this same with-statement code, now fixed); every other
  `PyList_NewBoxed` call site already used a properly-declared const.
  Verified fixed with a real regression test in `tests/runner.py` that
  checks actual written byte count via `wc -c`, not just file existence.
- **A pyc-level class cannot reliably back a stdlib-shaped container
  type**: confirmed by direct experiment while scoping `collections`.
  Subclassing `dict` (`class Counter(dict): ...`) does not behave as a
  real dict — `.items()` returned the instance's own attribute/method
  metadata instead of stored data. A plain class using
  `__getitem__`/`__setitem__` partially worked but silently dropped output
  on at least one path (`print(x[missing_key])` produced nothing at all).
  This is why `collections.Counter` returns a plain dict instead of a
  custom class instance, and why `defaultdict`/`namedtuple`/`deque` aren't
  implemented — `defaultdict` needs a per-instance "factory" slot dicts
  don't have, and `namedtuple` needs either attribute-style field access
  on a raw dict (unverified whether pyc supports this at all) or a new
  type, neither investigated in depth yet.
- **Dict iteration order is not insertion-order-preserving**: `PyObject`'s
  dict payload is `std::unordered_map<PyObject*, PyObject*>` keyed by raw
  pointer (not value — lookups are a linear scan comparing values, the hash
  map's own O(1) lookup is unused), and iterating it yields whatever order
  the hash buckets happen to produce. Real Python dicts guarantee
  insertion order (a language guarantee since 3.7); pyc's don't. This
  affects any code that iterates a dict with 2+ keys expecting insertion
  order (`for k in d`, `d.items()`, `json.dumps()` on a multi-key dict,
  dict `repr()`/`print()`), not just json — discovered while testing the
  json module (`from tests/runner.py`'s json test case deliberately avoids
  multi-key dicts because of this). A real fix means changing the dict
  storage to something order-preserving, which touches every dict
  operation in the runtime — out of scope for the stdlib-modules work that
  surfaced it; flagged here as a significant follow-up.
- **Fixed while adding json**: `Pyc_Subscript` (`d[key]`) used to raise
  `KeyError` for a key that legitimately maps to `None`, because it
  couldn't distinguish "key not found" from "key found, value is null"
  (both looked like a null `PyObject*` from `Pyc_GetItem`). Now scans the
  dict directly so a `None` value is returned correctly — needed for
  `json.loads('{"k": null}')["k"]` to work.
- **Float formatting sometimes uses scientific notation where CPython
  wouldn't**: e.g. `print(180.0)` prints `1.8e+02` instead of `180.0` —
  reproducible with a bare float literal, so it's a general `str()`/`print()`
  formatting bug, not specific to any particular computation. **Narrowed
  while adding `datetime`** (`timedelta.total_seconds()` kept tripping it):
  the trigger is exactly "whole-valued float whose integer part is evenly
  divisible by 10" — `10.0`, `20.0`, `50.0`, `100.0`, `9900.0`, `100000.0`
  all print in scientific notation, while `11.0`, `15.0`, `99.0`, `9999.0`,
  and any float with a nonzero fractional part (`50.5`) print correctly.
  Root cause not yet investigated (likely a trailing-zero-stripping
  heuristic in the printer that misfires into choosing `%g`-style output);
  avoid float values divisible by 10 in new CPython-output-comparison
  tests until this is root-caused.

### Type System
- **Conservative type tracking**: `widenLoopTypes()` widens to "boxed" on any type divergence at loop back-edges
- **No flow-sensitive type inference**: Type tracking is intra-procedural only
- **No union types**: A variable is either one type or "boxed"

### Runtime
- **`PyObject` is a flat struct**: `{refcount, type, value(i64), dvalue(double), list, dict, str}`
- **Most values flow as boxed `PyObject*`**: Native paths are optimizations, not the default
- **Exceptions use setjmp/longjmp**: Raise pops the frame before the jump; handler dispatch happens in generated code
- **Callables dispatch through a registry**: `Pyc_Apply(token, list)` with `__apply__` adapters
- **Newly discovered while designing `datetime`**: the existing opaque-handle
  allocator `allocObject()` (`Runtime.cpp`, used by `CompiledRegex`/
  `MatchObj`, types 8/9) allocates the `PyObject` via `calloc()`, which is
  undefined behavior for a struct whose `dict` member is a non-trivial
  `std::unordered_map` (`calloc` never runs the map's constructor — any
  code path that later touches `obj->dict` on such an object relies on
  zeroed memory happening to look like a valid empty map, which isn't
  guaranteed by the standard even if it works in practice with current
  libstdc++). Not fixed — out of scope for the datetime work that found
  it, and `CompiledRegex`/`MatchObj` don't appear to read `.dict` in
  practice, so this is latent rather than an observed failure. The new
  datetime types (14/15) deliberately avoid `allocObject()`, using
  `new PyObject()` instead — the same safe pattern `PyDict_New`/
  `PyList_New`/`PyUnicode_FromString` already use.
- **Fixed while adding `struct`**: `PyUnicode_FromString(const char* s)` —
  the primary `str` constructor, used almost everywhere a `str` is
  created — takes a bare `const char*` and assigns it into `PyObject.str`
  via `std::string`'s implicit-length (`strlen`-based) constructor. Any
  content with an embedded `0x00` byte silently **truncates** at that
  byte. Invisible for ordinary text (no legitimate `str` content contains
  NUL), but `struct.pack`'s output routinely does — e.g.
  `struct.pack("<i", 1000)` is the 4 bytes `E8 03 00 00`, and the old code
  path returned a 2-byte string. Fixed by adding a length-explicit
  `PyUnicode_FromStringAndSize(const char*, size_t)` (using
  `std::string::assign(s, n)`, which preserves embedded NULs and the
  exact length) and switching `struct.pack`'s and `base64.b64decode`'s
  return values to use it — the two new call sites that can produce
  arbitrary embedded-NUL content. Every *other* existing
  `PyUnicode_FromString` call site legitimately only ever constructs from
  NUL-free text (literals, formatted numbers, etc.), so this was scoped
  to just the two new sites rather than an audit of the whole file.
- **Fixed while adding `heapq`/`bisect`/`statistics`: seven existing list
  methods were silent no-ops on a homogeneous int/float list literal**
  (e.g. `h = [5, 1, 8, 3, 9, 2]; h.sort()` left `h` unchanged). Root
  cause: list literals whose elements are all one numeric type get
  stored via an existing A4 performance optimization (`list_item_type` 1
  or 2, elements in `PyObject.ilist`/`flist` — plain `int64_t`/`double`
  vectors, not boxed `PyObject*`) instead of the generic `list` vector.
  `.sort()`, `.insert()`, `.remove()`, `.index()`, `.count()`,
  `.reverse()`, `.extend()`, and `.copy()` (`PyList_Sort`/`Insert`/
  `Remove`/`Index`/`Count`/`Reverse`/`Extend`/`Copy` in `Runtime.cpp`)
  all operated on `lst->list` directly with no awareness of this, so for
  such a list each one silently read or mutated an *empty* vector — a
  true no-op for the mutators, and a wrong/empty result for the readers
  (`.index()` returning `-1`/"not found", `.count()` returning `0`) —
  with no visible failure signal, since none of them raise or return an
  error code. Found by spot-checking every A4-adjacent list method after
  discovering `.sort()`'s case while testing `heapq`/`bisect`
  (structurally identical requirement: in-place mutation or comparison
  over arbitrary `PyObject*` elements). **Not** affected, already
  correct: `.append()`, `.pop()`, `.pop(i)`, `.clear()` (each already had
  explicit `list_item_type==1`/`2` branches). Fixed centrally: a new
  `pyc_ensure_boxed_list()` helper converts a homogeneous list to the
  boxed representation in place (materializing a fresh `PyInt`/`PyFloat`
  per element, `list_item_type` reset to 0), called at the top of all
  eight affected functions plus every new `heapq`/`bisect`/`statistics`
  function (which share the identical requirement). This was a spot-check
  of methods adjacent to the one bug actually found, not an exhaustive
  audit — any other existing function reading `lst->list` directly
  without an explicit `list_item_type` check (there may be more,
  un-audited) would have the same latent bug.

### IR
- **Linear instruction list per function**: No CFG in IR; control flow is represented via labels and branches
- **Conservative result type metadata**: `int`, `float`, `bool`, `str`, or `boxed` — not a full type lattice
- **No SSA form**: Variables are stored in alloca slots

### Optimization Status

#### Landed (A1–A7 + container + P0/P1 + Phase 27)
- **A1**: Conservative type tracking with loop back-edge widening
- **A2**: Native `for ... in range(...)` with unboxed i64 loop variables
- **A3**: Native arithmetic for proven numeric `+ - * // % **` and unary minus
- **A4**: Homogeneous numeric lists with native `int64`/`double` element storage
- **A5**: Allocation sinking for numeric locals (native i64 alloca)
- **A6**: Specialized function variants from proven call-site types (all sites must agree);
  variants with proven numeric return type return native i64/double (not boxed PyObject*);
  variants can dispatch to other specialized variants (including self-recursion)
- **A7**: Allocation counters and microbenchmark guardrails
- **Container typing**: per-index `listElementTypes` / `subscriptElementTypes`, return
  element maps, nested `list_float`/`list_int` intermediates
- **P0 structured unpack**: `structuredElementLayout` / `pairOfStructuredLayout` for
  nbody-shaped `List[(list_float, list_float, float)]` and pairs thereof; for-loop and
  default-param layout propagation; safe unpack (handles only on mixed tuples)
- **P1 scalar freelist**: thread-local free-lists for plain int/float boxes
- **Safe native params**: only when every call site agrees; defaulted params stay boxed
- **List Auto setters**: only for known list containers (never dicts)
- **Native get/set fallbacks** on boxed lists after slice/`sorted` demotion
- **Phase 27 param type inference**: pre-scan function body AST to infer param types
  from numeric use contexts (BinOp/Compare with numeric constants, UnaryOp)
- **Phase 27 return type fixpoint**: infer return type from body with self-recursive
  call propagation; enables native `add` of recursive call results
- **Phase 27 native i1 icmp**: native numeric comparisons emit i1 directly (no PyBool_New)
- **Phase 27 dead funcval elimination**: skip callee lowerExpr for known direct functions

#### Planned (not implemented)
- IR-level constant folding
- Full arena allocator beyond scalar freelist
- Dead code elimination at IR level
- Full insertion-ordered dicts
- Native `**` / rsqrt and full mass/mag float chain in nbody
- Extend recursive specialization to mutual recursion and float-returning functions
- **Generalized multi-dispatch specialization**: generate one specialized variant
  per distinct call-site type signature (e.g. `__specialized_add_ii` and
  `__specialized_add_ff` for a function called with both `(int, int)` and
  `(float, float)`). The current A6 specialization only generates a variant when
  *all* call sites agree on the same signature, so a function called with mixed
  int and float args gets no variant at all.

  Multi-variant generation was prototyped and works (variants are correctly
  created with per-sig native params and per-sig native return types). However,
  **non-recursive call sites can't dispatch to the variants** due to a
  chicken-and-egg problem: the A6 codegen dispatch checks whether call-site
  arguments are *already native* (i64/double in LLVM IR) before routing to a
  variant. But a variable like `s = add(s, i)` receives a boxed `PyObject*`
  result from the first call, so on the next loop iteration `s` is still boxed
  and the dispatch check fails. The variant never fires.

  This cycle does not affect self-recursive functions like `fib` because the
  param `n` is typed as int from body-level inference (`n <= 1`, `n - 1` with
  int constants), so recursive calls within the variant have native args.

  **What would be needed to make it work:**
  1. **Speculative unboxing at call sites**: the codegen dispatch should check
     whether a variant exists for the *declared/inferred types* of the arguments
     (not whether the args are already native), and unbox them at the call site
     if a matching variant exists. This requires knowing the variant exists at
     the call site and inserting unbox calls.
  2. **Native return value propagation to the receiver slot**: when a variant
     returns i64/double, the call result should be stored in a native alloca
     (not boxed), so the receiver variable stays native across loop iterations.
     This requires the call-site code to know the variant's return type and
     allocate a native slot for the result.
  3. **Per-variant return type inference**: each variant's return type must be
     computed from its own signature (an all-int variant returns i64, an
     all-float variant returns double), not from the original function's merged
     return type. This was implemented in the prototype via per-sig
     `inferParamTypesFromBody` re-invocation.

  The infrastructure for (3) and the multi-variant generation itself are
  straightforward. The speculative unboxing in (1) and native-slot propagation
  in (2) are the deeper changes that require modifying the codegen call-site
  dispatch to look up variants by inferred type rather than by runtime LLVM IR
  type.

### Correctness Guarantees
- Every optimization preserves a boxed fallback path
- Native paths only trigger when `resultType` is proven numeric
- Mixed types fall back to boxed `PyNumber_*` calls
- Division by zero uses runtime call with proper Python semantics
- `getAsPyObject()` ensures values are properly boxed when they escape native context
- Dict item assignment uses `Pyc_SetItem` (not list Auto helpers)
- Native param unboxing is suppressed when any call site is non-numeric or a default exists
- `GetItemDouble` only on homogeneous float lists — never on mixed body tuples
- Boxed unpack scalars are not placed in `numericFloatLocals`

### Code Alignment Requirements
All `CreateLoad` calls that read from `PyObject` struct fields must use explicit alignment
specifiers via `CreateAlignedLoad`. The `PyObject` struct layout is:
- `refcount` (i32) at offset 0 — requires `Align(4)`
- `type` (i32) at offset 4 — requires `Align(4)`
- `value` (i64) at offset 8 — requires `Align(8)`
- `dvalue` (double) at offset 16 — requires `Align(8)`

Missing alignment specifiers cause LLVM to generate incorrect machine code for values
exceeding 32 bits, resulting in silent truncation. This was fixed by replacing all
`CreateLoad` calls on struct fields with `CreateAlignedLoad` with appropriate alignment.

## Architecture

```
Python source
    │  Python C API (ast.parse)
    ▼
ASTNode tree  (PythonParser.cpp)
    │  LoweringVisitor
    ▼
ModuleIR  (IR.h / IR.cpp)
    │  Codegen::generate
    ▼
llvm::Module  (Codegen.cpp)
    │  LLVM passes (O0–O3)
    ▼
.o object file
    │  clang++ + Runtime.cpp
    ▼
native executable
```

### Runtime (`src/runtime/Runtime.cpp`)
Standalone C++ file, no CPython dependency. Provides:
- `PyObject` (flat struct: `refcount`, `type`, `value`/`dvalue`/`list`/`dict`/`str`/`cell_content`)
- Refcounting, arithmetic, comparison, print, and all builtins
- Types: int, list, dict, str, float, bool/None, cell, super proxy, compiled regex,
  match object, exception, function, exception class, complex
- Exceptions use setjmp/longjmp frames
- Callables dispatch through a registry of `__apply__` adapters (`Pyc_Apply`)
- Linked into every compiled binary

### IR (`include/pyc/IR.h`, `src/ir/IR.cpp`)
Linear instruction list per function. Instructions:
- `const`, `fconst`, `bconst`, `nconst` — constants
- `assign` — variable assignment
- `add`/`sub`/`mul`/`div`/`truediv`/`mod`/`pow` — arithmetic
- `icmp` — integer comparison
- `br`, `label` — control flow
- `call` — function call
- `ret` — function return

IR instructions carry conservative result type metadata (`int`, `float`, `bool`, `str`, `boxed`).

## Testing

### Test Suite
- `tests/runner.py`: 300 inline test cases + file-based regression tests
- Each case compiled and compared against CPython output
- File cases: `tests/opt_*.py`, `tests/nbody.py`, `tests/fib*.py`, `tests/builtins*.py`, etc.

### Running Tests
```bash
cd build && make check   # or: ctest
PYC_BINARY=./build/pyc python3 tests/runner.py
```

### Benchmarking
```bash
# N-Body benchmark
python3 tests/nbody.py 5000000
./build/pyc tests/nbody.py -o nbody_compiled -O2
./nbody_compiled 5000000

# Profiling
perf record /tmp/nbody_compiled 5000000
perf report
```

## Development History

Initially scaffolded with Grok (xAI). Extended substantially with Claude (Anthropic).
See `GROK.md` for early history.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

## Benchmarking

### N-Body Simulation

The `tests/nbody.py` file is used as a performance benchmark. It's an N-body
gravity simulation from the Computer Language Benchmarks Game.

```bash
# Python interpreter baseline
python3 tests/nbody.py 5000000

# Compiled binary
./build/pyc tests/nbody.py -o nbody_compiled -O2
./nbody_compiled 5000000
```

**Expected output:**
```
-0.169075164
-0.169059907
```

### Profiling

```bash
perf record /tmp/nbody_compiled 5000000
perf report
strace -c /tmp/nbody_compiled 5000000
```

### Microbenchmarks

**Simple Loop Test:**
```bash
echo 'for i in range(10000000): x=i' > /tmp/loop_test.py
python3 /tmp/loop_test.py
./build/pyc /tmp/loop_test.py -o /tmp/loop_test -O2
/tmp/loop_test
```

**Arithmetic Intensive Test:**
```bash
echo 'x=0.0
for i in range(1000000):
    x += i * 0.5
    x *= 1.000001
print(x)' > /tmp/arithmetic_test.py
python3 /tmp/arithmetic_test.py
./build/pyc /tmp/arithmetic_test.py -o /tmp/arithmetic_test -O2
/tmp/arithmetic_test
```

**Homogeneous List Test:**
```bash
echo 'lst = [i for i in range(1000000)]
s = 0
for x in lst:
    s += x
print(s)' > /tmp/list_test.py
python3 /tmp/list_test.py
./build/pyc /tmp/list_test.py -o /tmp/list_test -O2
/tmp/list_test
```

**Function Call Test:**
```bash
echo 'def add(a, b):
    return a + b

s = 0
for i in range(100000):
    s = add(s, i)
print(s)' > /tmp/call_test.py
python3 /tmp/call_test.py
./build/pyc /tmp/call_test.py -o /tmp/call_test -O2
/tmp/call_test
```

### Allocation Counters (A7)

The runtime tracks allocations per type via atomic counters:

- `PyAlloc_GetIntCount()` — `PyInt_FromLong` allocations (excludes small int cache)
- `PyAlloc_GetFloatCount()` — `PyFloat_FromDouble` allocations
- `PyAlloc_GetListCount()` — `PyList_New` allocations
- `PyAlloc_GetDictCount()` — `PyDict_New` allocations
- `PyAlloc_GetStrCount()` — `PyUnicode_FromString` allocations
- `PyAlloc_GetTotal()` — sum of all above

These counters are exposed via `extern "C"` functions in `runtime.h` for external measurement.
