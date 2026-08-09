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

### `**kwargs` — Now Mostly Working; Two Narrow Gaps Remain
This entry has accumulated fixes across several passes of the same bug
hunt; kept as one section since all four findings below are different
facets of the same feature.

**Fixed: `f(**some_dict)` at a call site used to segfault.** Spreading a
real `dict` object into a callee's ordinary named parameters (`inner(a,
b, c)` called as `inner(**{"a": 1, "b": 2, "c": 3})`, or via a dict
variable) is handled by a dedicated runtime helper that, at the call
site, is given the callee's parameter names and looks each one up in the
dict. That helper used to be a C varargs function (`Pyc_ExpandKwargs`)
that scanned its variadic arguments for a null-pointer sentinel to know
where they ended — except the call site (`Compiler.cpp`) never actually
appended that sentinel, and `Codegen.cpp`'s generic fallback for
undeclared external functions always declares a plain fixed-arity
(non-vararg) LLVM signature sized to exactly match each call site's real
argument count. So the `va_arg` scan always read one or more arguments
past the real ones into undefined stack/register contents looking for a
terminator that was never there — undefined behavior that crashed with
a segfault on every `f(**kwargs)` call, confirmed against real CPython
(`inner(**d)` for a plain 3-key dict). Fixed by rewriting the helper
(now `Pyc_ExpandKwargsList`) to take the parameter names as a single
boxed list argument instead of C varargs, so the call site always passes
exactly two real arguments — sidestepping the sentinel problem entirely
rather than patching around it.

**Fixed: combining `*args` and `**kwargs` catch-all parameters together
on the same function crashed compilation entirely.** `def f(*args,
**kwargs): ...` failed LLVM module verification on every *indirect*
call (through a first-class value, closure, or decorator) — **breaking
the single most common use of `**kwargs` in real Python code: a generic
decorator's inner wrapper**, `def wrapper(*args, **kwargs): return
f(*args, **kwargs)`. Confirmed this crashed compilation even as a
*nested* function inside a closure (the standard `@deco` pattern), and
even when the wrapper's body never actually read `kwargs` at all —
merely declaring a nested function with both parameters was enough.
Root cause: every function that might be called indirectly gets a
generated `__apply__N` adapter in `Codegen.cpp` — a wrapper that unpacks
a boxed argument list into the real function's native parameter shape.
That adapter's parameter-shape analysis scanned for the *first*
parameter name starting with `*` to find "the" vararg slot — but
`**kwargs` also starts with `*` (stored internally as `"**kwargs"`), so
for a signature with both, the loop found `*args` and stopped, never
noticing `**kwargs` as a *second*, separate slot, leaving the adapter's
call to the real function one argument short. Fixed by detecting
`*args` and `**kwargs` as two independent slots (checking for a
*second* leading `*` character) and supplying a placeholder for the
`**kwargs` slot.

**Fixed: the `**kwargs` catch-all *parameter* itself now actually
collects the caller's keyword arguments, for direct calls.** `def
f(**kwargs): print(kwargs)` called as `f(a=1, b=2)` used to print `[]`
(an empty **list**, `type()` showed `<class 'list'>`) instead of
CPython's `{'a': 1, 'b': 2}` (a real dict) — the caller's excess keyword
arguments weren't collected into anything at all, for *any* call shape,
not just the indirect one described above. Root cause was the same
"first star-prefixed name wins" bug pattern as the adapter crash above,
but in the separate, *direct*-call-site logic in `Compiler.cpp`'s
`lowerCall` (used when calling a named function directly, not through
`Pyc_Apply`): the code that collects `*args` overflow into a list also
stopped at the first `*`-prefixed parameter, so a trailing `**kwargs`
parameter never got a value collected for it — and separately, keyword
arguments that didn't match any *regular* parameter name were simply
dropped on the floor instead of being routed anywhere.

Fixed by detecting `*args` and `**kwargs` as independent slots (mirroring
the `Codegen.cpp` adapter fix above) and, whenever the callee has a
`**kwargs` slot: unconditionally giving it a real (initially empty) dict
so a call with zero keyword arguments still supplies every parameter the
declared IR function expects (the crash-shaped failure mode), then — if
the call actually has keyword arguments that don't name a regular
parameter — building a populated dict from exactly those and using it
instead. Verified against real CPython: `**kwargs` alongside regular
positional parameters, `**kwargs` alone, `*args` and `**kwargs` together
in every arg-count combination (`h(1,2,3,x=1,y=2)`, `h(1,2,3)`, `h(x=1)`,
`h()`), iterating and summing the collected dict's values, and
`.get(key, default)` on the result.

**Fixed on a later pass: indirect calls losing `**kwargs` entirely.**
Indirect calls (through a closure/decorator/first-class value, going
through the `Codegen.cpp` `__apply__<name>` adapter rather than
`lowerCall`'s direct-call path) always got an empty dict for a
`**kwargs` slot — worse, the placeholder itself was the wrong *type* (an
empty **list**, not a dict), regardless of what the caller passed.
Confirmed via both `g = f; g(a=1, b=2)` (a stored function reference)
and the standard decorator-forwarding pattern (`w(x=1, y=2)` where `w`
wraps `def wrapper(*args, **kwargs): return fn(*args, **kwargs)`), and
even `g()` with zero keyword args (which should get `{}`, not `[]`).

Root cause: `Compiler.cpp`'s indirect-call lowering builds the flat
argument list Pyc_Apply expects (`indirectArgListTemp`) incrementally,
appending each call-site argument as it's processed — *except* keyword
arguments, which were instead pushed onto a separate `argRes` vector
that indirect calls never actually read from at all (that vector only
feeds the *direct*-call codegen path). So keyword arguments to an
indirect call were silently discarded before ever reaching `Pyc_Apply`,
regardless of the adapter's own (also-broken) placeholder logic.

Fixed at two levels:
- **Call site** (`Compiler.cpp`): when an indirect call has keyword
  arguments and/or dict spreads, they're merged into a single real dict
  (`PyDict_New` + `PyDict_SetItem`/`PyDict_Update`) and appended as the
  *last* element of the flat argument list — after all positional
  arguments and any spliced `*args` contents, which are already
  appended in call-site source order by the time this runs. An ordinary
  positional-only indirect call (no keyword arguments at that call site)
  appends nothing extra, so it's byte-for-byte unaffected.
- **Adapter** (`Codegen.cpp`): the generated `__apply__<name>` adapter
  doesn't know the calling convention of any particular call site — only
  its own target's declared signature (`hasVar`/`hasKwVar`). When
  `hasKwVar`, it now checks at runtime whether the incoming list has one
  more element than the minimum required count *and* that trailing
  element's type tag is a dict (2); if so, that element is bound to the
  `**kwargs` slot and excluded from the `*args` tail (correctly
  disambiguating a target with *both* `*args` and `**kwargs`, e.g.
  `both(*args, **kwargs)` called indirectly with a mix of positional and
  keyword arguments); otherwise it falls back to a fresh, empty dict
  (fixing the wrong-type placeholder too, for every indirect call to a
  `**kwargs` target — not just the ones with actual keyword arguments).

While fixing this, also found and fixed a small, genuinely pre-existing
refcount leak in the two functions this work exercises most heavily:
`Pyc_CallMethod` and `PyBuiltin_SuperMethod` each build a temporary
argument list (to prepend `self`/`cls`) and pass it to `Pyc_Apply`
without ever freeing it — a leak on *every* `instance.method()` call and
every `super()` call, confirmed present on the unmodified commit via
valgrind on a plain, unrelated `y.show()` method call. Fixed by
capturing `Pyc_Apply`'s result before `Py_DECREF`ing the temporary list,
matching the pattern already used correctly by `pyc_call_dunder1`/
`pyc_call_dunder2` elsewhere in the same file.

Verified against real CPython: a stored function reference with
keyword args, with zero keyword args (dict, not list, and empty), a
function combining a positional param with `**kwargs`, and the full
`*args`+`**kwargs` decorator-forwarding shape (`both(*args, **kwargs)`
called indirectly with positional-only, keyword-only, and mixed
arguments). Added as a permanent `tests/runner.py` regression. Full
suite and import tests stay green; `valgrind --tool=memcheck` shows 0
new errors (fewer than before, thanks to the leak fix above).

**Fixed on a later pass**: missing keys in a `**dict` spread not
  falling back to the parameter's default, and mixing a positional
  argument with a `**dict` spread mis-binding. Both had the same root
  cause: the dict-spread path unpacked its runtime helper's results into
  every parameter position unconditionally, regardless of what was
  already there. `inner(a, b, c=99)` called as `inner(**{"a": 1, "b": 2})`
  (omitting `c`, which has a default) gave `3` instead of `102` — the
  missing key produced `None` (→ `0` in arithmetic) rather than
  consulting `c`'s default, unlike the direct `key=value` keyword-
  -argument path, which already consulted `funcDefaultValues` correctly.
  `mixed(a, b, c)` called as `mixed(1, **{"b": 2, "c": 3})` gave `23`
  instead of `123` — the spread dict didn't happen to also supply `"a"`,
  so the unconditional overwrite clobbered the already-correct
  positional `1` with `None`.

  Fixed by replacing the batch "look up every parameter name, unpack the
  whole result list" design (`Pyc_ExpandKwargsList`) with one
  `Pyc_DictGetOrDefault(dict, paramName, fallback)` call per parameter,
  where `Compiler.cpp` computes the exact right `fallback` for each
  position at compile time: an already-bound value (a positional
  argument, or a `key=value` keyword argument matched earlier in the
  same call) takes priority, then the parameter's registered default (if
  any), then boxed `None` (matching the existing, separate,
  undocumented gap of not raising `TypeError` for a genuinely missing
  required argument — not attempted here). This eliminated the whole
  batch-unpack step entirely rather than patching around its
  unconditional-overwrite behavior. Verified against real CPython:
  omitted-defaulted-parameter, all-parameters-present (unaffected,
  confirmed no regression), positional argument combined with a spread
  dict, a spread dict combined with a direct `key=value` keyword
  argument in the same call, and an all-defaults call with an empty
  spread dict (`f(**{})`). Added as permanent `tests/runner.py`
  regressions. Full suite and import tests stay green; `valgrind
  --tool=memcheck` shows 0 errors.

**Still not fixed (a separate, narrower, distinct gap from the indirect-
call fix above)**: a `**dict` spread's own unmatched entries (keys that
don't name any regular parameter) are not routed into a `**kwargs`
catch-all parameter either, even though direct `key=value` arguments now
are — routing them would need a *runtime* set-difference between the
spread dict's keys and the callee's named parameters, since (unlike
direct keyword arguments) the dict's actual keys aren't known until
runtime. Confirmed still present: `def f(**kwargs): ...` called
directly as `f(**{"p": 1, "q": 2})` gives `kwargs == {}`, not `{'p': 1,
'q': 2}`.

Verified fixes added as permanent `tests/runner.py` regressions. Full
suite and import tests stay green after each fix; `valgrind
--tool=memcheck` shows 0 errors throughout.

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

### `hashlib` / `base64` / `struct` — Now Bytes-Aware (Previously Str-as-Byte-Buffer)
**Superseded**: pyc previously had no distinct `bytes` type — this
section used to document that as a "conscious, user-approved scoping
decision... explicitly declined as a separate, larger project." That
decision was reopened (see the `bytes`/`bytearray` section below) and a
real `bytes`/`bytearray` type now exists. `hashlib.md5/sha1/sha256`
accept `str`/`bytes`/`bytearray` interchangeably (still more permissive
than real CPython, which rejects a plain `str` with `TypeError` — that
part of the original permissiveness stance is unchanged, just extended
to also accept the new types). `base64.b64encode`/`b64decode` and
`struct.pack` now return real bytes (previously str) — see the
`bytes`/`bytearray` section for the full detail on this change.
`hashlib` now has `.digest()` (raw bytes) alongside `.hexdigest()`, since
a real bytes type makes that the natural representation; still no
`.update()` (digests are computed eagerly at construction, since the
data is always fully known in the call already). `struct` still supports
only the common format codes and endianness prefixes, not native
alignment padding or `n`/`N`.

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

### `copy.copy`/`copy.deepcopy` — AST-Structural Dispatch, Not Token+Registry
Every other synthetic module function (including this session's
`hashlib`/`base64`/`struct`/`heapq`/`bisect`/`statistics`) reaches its
implementation via the generic dict-lookup dispatch in
`Compiler.cpp`'s `lowerMethodCall` (`typeOf(obj)=="dict"` → `Pyc_GetItem`
+ `Pyc_Apply`). `copy.copy`/`copy.deepcopy` can't use that path: the
`copy` module's own dict is itself `typeOf`-tagged `"dict"` (exactly like
`os.path`, `math`, every other module dict), and there is already an
**unconditional** `.copy()` branch in that same dispatch chain (for
`list.copy()`/`dict.copy()`) that would claim `copy.copy(x)` first and
call `PyDict_Copy` on the *module dict itself* — a collision that,
unlike `os.path.join`'s (Phase 1, fixed with a `typeOf(obj)!="dict"`
guard), has no such fix available, because the colliding receiver
genuinely *is* `"dict"`-typed. Resolved by recognizing
`copy.copy(...)`/`copy.deepcopy(...)` structurally in the AST (the same
`isPathlib`/`isHashlib`-style literal-or-aliased-module-name check used
for `pathlib.Path`/`hashlib.md5`), dispatching directly to
`PyCopy_Copy`/`PyCopy_Deepcopy` before the generic chain is ever
reached. `from copy import copy, deepcopy` (bare-name, including `as`
aliasing) is handled the same way as `datetimeCtorAliases`/
`hashlibCtorAliases`, via a `copyFuncAliases` map.

### `functools.partial`/`lru_cache`, `operator.itemgetter`/`attrgetter` — Reuse the Existing "Descriptor Bundle" Mechanism
pyc already represents a closure's captured free variables as a plain
boxed list `[funcTokenOrObj, cell0, cell1, ...]` — a "descriptor bundle".
`Pyc_Apply` (`Runtime.cpp`) already knows how to call one: it extracts
`funcTokenOrObj`, **prepends** the remaining list elements to whatever
argument list the caller supplies, and dispatches. This session's
`functools.partial`/`lru_cache` and `operator.itemgetter`/`attrgetter`
all reuse this *unmodified* — "a callable that remembers some captured
state" is just a list literal built by a small runtime function
(`PyFunctools_Partial`, `PyOperator_Itemgetter`, etc.), with **no new
type, no new dispatch machinery, and no Compiler.cpp changes for the
call side** — only construction needed new code. This is why these
features are robust to being passed through function parameters,
stored in variables, etc. (verified directly) — they go through the
same, already-general indirect-call path every closure already uses.

### Two Real Compiler Bugs Found While Building `functools`/`operator`
Both in `Compiler.cpp`, both pre-existing (not introduced by this
session's earlier phases), both would affect **any** code hitting the
same shape, not specifically `functools`/`operator`:

1. **A value returned from the generic dict-dispatch method-call path
   was never marked as "may hold a callable token"**, so assigning it
   to a variable and later calling that variable could miscompile.
   Concretely: `add5 = functools.partial(operator.add, 5); add5(10)` —
   `functools.partial(...)`'s call goes through the same generic
   `typeOf(obj)=="dict"` → `Pyc_GetItem` + `Pyc_Apply` dispatch as every
   other synthetic-module function call (`os.path.exists(...)`, etc.,
   `Compiler.cpp`'s `lowerMethodCall`, the branch just before the
   class-instance-method fallback). That dispatch's result temp was
   never added to `callableTokenTemps`, so `lowerAssign`'s existing
   propagation (`callableTokenTemps.count(val)` → mark the target name in
   `namesThatMayHoldCallableTokens`) never fired for `add5`, and a later
   `add5(10)` call fell through to being treated as a plain, unresolved
   direct-call name instead of a dynamic `Pyc_Apply` dispatch. Fixed by
   unconditionally marking that dispatch's result as a callable-token
   temp — safe because the generic dict-dispatch path's return value is
   *always* statically unknown (could be anything, including a callable
   bundle), so treating it as "might be callable" can't be wrong, only
   occasionally unnecessary.
2. **A single shared `lastLambdaSynthetic` flag, used to let
   `f = lambda: ...; f()` resolve as a fast direct call to the lambda's
   IR function (skipping the dynamic-dispatch machinery), leaked across
   unrelated statements.** The flag is set whenever *any* lambda
   expression is lowered, anywhere — including as another call's
   argument, e.g. `functools.reduce(lambda a, b: a + b, [1,2,3,4])` —
   and was only ever cleared by being consumed inside `lowerAssign`
   (`Compiler.cpp`). Since a lambda-as-call-argument isn't consumed by
   any assignment, the flag stayed set after that statement, and the
   *next*, completely unrelated simple assignment
   (`add5 = functools.partial(...)`) picked it up via `lowerAssign`'s
   `else if (!lastLambdaSynthetic.empty())` fallback, aliasing `add5`
   directly to the earlier, unrelated lambda's IR function. Calling
   `add5(10)` (1 argument) against a lambda expecting 2 then failed
   LLVM's IR verifier with an argument-count mismatch — a hard compile
   failure, not a silent wrong answer, but a real bug regardless (and a
   silent-wrong-answer variant is plausible for a lambda with a
   *compatible* arity that just happens to be the wrong one). Fixed by
   clearing `lastLambdaSynthetic` at the very top of `lowerAssign`,
   before lowering the current statement's own RHS — so the flag can
   only ever reflect a lambda freshly produced by *this* statement's own
   RHS expression, never a leftover from an earlier one.

### `csv.writer` — AST-Structural Dispatch, Same Reason as `copy.copy`
`csv.writer(f)` needed the same treatment as `copy.copy`/`copy.deepcopy`
(above), for a related but distinct reason: `.writerow(row)` needs an
explicit receiver — the *same* lesson `file.write()`'s fix established
(the generic, non-bound dict dispatch has no way to supply one) — which
means the writer object returned by `csv.writer(f)` needs a custom
`typeOf` tag (`"csvwriter"`) so `.writerow()` can be gated on it. The
generic dict-dispatch call path has no mechanism to attach a custom tag
to its result, so `csv.writer(f)`'s *construction* also has to be
recognized structurally in the AST (mirroring `pathlib.Path`/
`hashlib.md5`'s pattern), even though `csv.writer` itself doesn't have
`copy.copy`'s specific unconditional-`.copy()`-collision problem.
`PyCsv_Writer` is a direct-call function (`Runtime.cpp`) taking the file
object directly, not the token+registry `PyObject* Fn(PyObject* argsList)`
convention every other module function in this session's work uses.

### `itertools.groupby` — Same AST-Structural Reason as `csv.writer`, But for `key=`
`groupby(iterable, key=...)`'s `key=` is a genuine *keyword* argument,
which the generic dict-dispatch call path has no mechanism to read
through at all (confirmed as a real, previously-latent bug: before this
fix, `itertools.groupby(words, key=lambda w: w[0])` silently grouped by
the whole word instead of `w[0]`, since `key` was simply never passed to
`PyItertools_Groupby`). Fixed the same way as `csv.writer`: `groupby`'s
construction is recognized structurally in the AST (both the
`itertools.groupby(...)`-qualified form and the bare-name form after
`from itertools import groupby`, via a `groupbyCtorAliases` set mirroring
`csvWriterCtorAliases`), extracting `key=` (or a second positional
argument — `groupby(iterable, key)` is valid without the keyword too)
directly from the call's AST and passing it to `PyItertools_Groupby`,
which — like `PyCsv_Writer` — was converted from the token+registry
`PyObject* Fn(PyObject* argsList)` convention to a direct 2-raw-argument
call, since it's now never reached via the generic dispatch at all.

### `chain.from_iterable` — Same `pyc_ensure_boxed_list` Gap as the `heapq`/`bisect`/`statistics` Phase
`itertools.chain.from_iterable([[1,2],[3,4]])` (with homogeneous
int/float list literals as the *inner* lists) silently returned `[]`
before this fix, for the identical reason found and fixed for
`list.sort()`/`heapq`/etc. in the prior phase: literal lists whose
elements are all one numeric type are stored via the A4 fast-path
(`list_item_type` 1/2, data in `ilist`/`flist`, not the generic boxed
`list` vector), and the new function read `inner->list` directly without
first calling `pyc_ensure_boxed_list()`. This is a direct instance of
the exact class of bug already documented and (partially) audited for in
the `heapq`/`bisect`/`statistics` phase — a reminder that the earlier
audit was a spot-check of *existing* functions at the time, not a rule
enforced for *all future* list-consuming functions; every new function
that reads a list's `.list` field directly needs this call, and it's
easy to forget for a *nested* list (the outer list here was already
correctly boxed, going through it caught nothing).

### Newly Discovered, General, Pre-Existing Bug: List Comprehensions Don't Support Multi-Variable `for a, b in pairs` Unpacking
Found while verifying `itertools.groupby`'s output via `[(k, list(g)) for
k, g in groupby_result]` — completely unrelated to `groupby` or any
other feature in this session. `[k for k, g in [["a", 1], ["b", 2]]]`
returns `[None, None]` instead of `['a', 'b']`; a **plain `for` loop**
with the identical unpacking (`for k, g in pairs: print(k, g)`) works
correctly and prints the right values. So the bug is specific to
*comprehension*-form `for`-clauses with more than one loop target, not
tuple/list unpacking in general (which plain `for` loops, plain
assignment (`a, b = pair`), and function parameters all handle
correctly elsewhere in this codebase).

**Partially fixed:** AST-level change applied (`Comprehension.target`
changed from `std::string` to `std::shared_ptr<Expr>` in `frontend/ast.h`
for both `ListComp` and `SetComp`). `lowerListComp` (`Compiler.cpp`)
updated to check if target is Name or tuple/list pattern, calling
`lowerUnpackTarget()` for non-Name targets (same pattern as
`lowerDictComp`). `build_list_comp` and `build_set_comp` in `ir/builder.cpp`
updated to handle unpack targets with element-by-element stores.

**Current issue:** Despite the fix being in place, runtime unpacking still
not working — `[None, None]` output persists. Debug investigation revealed:
- `lowerListComp` debug output not appearing (file not created despite
  fopen calls in code)
- LLVM IR shows comprehension lowered, but `lowerUnpackTarget` NOT being
  called (no `PyList_Unpack2` calls in IR)
- Target node type check at `Compiler.cpp:9381-9386` may not be matching
  actual AST node type
- AST from `parse_helper.py` shows target is `Tuple` node with `Name`
  children — should match non-Name branch

Workaround: use a plain `for` loop instead of a comprehension whenever
destructuring multiple values per iteration, until this is fully fixed.
See KnownGapsPlan.md for detailed tracking.

### `collections.deque`/`namedtuple`/`defaultdict`
`deque` reuses `pathlib.Path`'s pattern exactly: construction is
recognized structurally in the AST (both `collections.deque(...)`-qualified
and the bare-name form after `from collections import deque`, via a
`dequeCtorAliases` set mirroring `pathCtorAliases`) so the result can
carry a compile-time `"deque"` typeOf tag — the generic dict-dispatch path
has no mechanism to attach one. It's a plain list at runtime (type 1, no
new type); `.appendleft`/`.popleft`/`.rotate` are new typeOf-gated direct
calls, and three existing typeOf-gated checks (`.pop`/`.copy`/`.clear`)
were extended to also accept the `"deque"` tag.

`namedtuple` reuses the "descriptor bundle" mechanism from
`functools.partial` (above): `namedtuple(typename, field_names)` is pure
token+registry (no AST recognition needed — it takes only positional
arguments, so the generic dispatch handles it fine, unlike `deque`),
returning a bundle `[constructorToken, fieldNamesList]`. Calling the
bundle builds a plain dict pairing each field name to its positional
argument; attribute reads (`p.x`) already work generically since
`lowerAttribute` routes every bare attribute read through `Pyc_GetItem`
regardless of the receiver's type — a dict with a string key `"x"`
already supports `.x` syntax with zero new runtime code for that part.

`defaultdict(default_factory)` is also pure token+registry. Its factory
is **not** stored as a visible dict entry — that was the first
implementation tried, under a reserved key
(`"__pyc_default_factory__"`), and it broke immediately during
verification: `print(dd)` showed the marker key alongside the real data
(`{'a': [1, 2], '__pyc_default_factory__': 'PyBuiltin_ListFactory'}`),
because a `defaultdict` is, at runtime, just a dict — nothing distinguishes
"real" keys from the marker for print/len/iteration/`.items()` to skip.
Fixed by moving the factory out-of-band into
`g_pycDefaultFactories` (`Runtime.cpp`, declared next to `Pyc_Subscript`),
a `std::unordered_map<PyObject*, PyObject*>` keyed by the dict object's
own pointer — the exact same pattern already used for open file objects
(`g_pycFiles`). `Pyc_Subscript`'s dict-miss path checks this map before
raising `KeyError`: if the dict has a registered factory, it's called
with an empty argument list (same convention as any other zero-arg
factory call in this codebase), the result is stored under the missing
key (mutate-on-access, matching real `defaultdict`), and returned.

**Real bug found and fixed while wiring up `defaultdict(list)`**: a bare,
*uncalled* reference to a builtin type name — the `list` in
`defaultdict(list)`, as opposed to an actual call like `list(x)` — had no
runtime representation at all. `lowerCall`'s `funcName == "list"` branch
only fires for real calls; a bare `Name` node for `"list"` fell through
to the generic bare-name fallback, which just returns the raw identifier
string as if it were a variable — and since no variable named `list` was
ever assigned, this resolved to an uninitialized/null value at runtime.
Confirmed empirically: `defaultdict(list)` compiled and ran, but every
auto-populated key raised `KeyError` instead of returning `[]`, because
the stored "factory" was silently `None`. This is architecturally the
same gap `B13` already solved for bare exception-class references
(`exc = ValueError`) — so it's fixed the same way: `list`/`dict`/`int`/
`float`/`str`, when referenced bare and not shadowed by a local, now
resolve to a zero-arg factory callable-token
(`PyBuiltin_ListFactory`/`PyBuiltin_DictFactory`/etc., `Runtime.cpp`),
usable anywhere a callable value is expected — not just inside
`defaultdict(...)`. Confirmed this doesn't disturb `isinstance(x, list)`:
its typecode fast path reads the classinfo argument's *AST node*
directly (`node->children[2]->id`), not the lowered value, so the extra
callable-token temp this produces is simply unused dead IR in that case,
never affecting the actual typecode dispatch.

**Limitation surfaced clearly by `namedtuple`, now fixed**: `dict`
(`include/pyc/object_struct.h`) was previously backed by
`std::unordered_map<PyObject*, PyObject*>`, so no dict in this compiler
preserved insertion order the way real Python 3.7+ dicts do —
`Point(x=3, y=4)` printed as a dict could come out as `{'y': 4, 'x': 3}`.
Fixed by replacing the dict payload with
`std::vector<std::pair<PyObject*, PyObject*>>` (linear-scan value-equality
lookup, same O(n) complexity as before since the `unordered_map`'s
raw-pointer hash was never used for the actual value comparison);
`PyDict_SetItem` updates existing keys in place to preserve their
insertion position. `Point(x=3, y=4)` now correctly prints
`{'x': 3, 'y': 4}`.

### `re.IGNORECASE` — Severe Pre-Existing Bug, Found and Fixed
`PyBuiltin_ReSearch` (`Runtime.cpp`, the function backing both
`re.search` and `re.match` — the latter is aliased to it, a separate
pre-existing gap noted below) had its own inlined `pcre2_compile` call
that hardcoded `PCRE2_CASELESS` unconditionally, instead of going
through the shared `compileRegex()` helper every other `re.*` function
already used correctly (`options=0`, case-sensitive). The comment
sitting directly above the bug even said *"We always compile with
PCRE2_CASELESS for now so re.IGNORECASE is implicit. This matches what
test/regex.py expects."* — i.e. the existing test suite's one flag-using
case (`re.search(r"hello", s, re.IGNORECASE)`) passed only because the
bug happened to produce the flag's effect unconditionally, not because
the flag was actually read. Confirmed against real CPython:
`re.search("Hello", "hello")` incorrectly matched under the old code.
Compounding this, `re.IGNORECASE`/`MULTILINE`/`DOTALL` didn't exist as
real values at all before this fix — no entry in `makeReModuleDict()` —
so a bare `re.IGNORECASE` reference silently evaluated to `None` via the
generic dict-miss path, and even where the compiler's `re.*` dispatch
accepted a 3rd positional argument, it was explicitly discarded (comment:
*"ignore extra args like re.IGNORECASE"*).

Fixed by: (1) removing the hardcoded flag and making `compileRegex()`
accept a real PCRE2 options bitmask, (2) adding a small
`pyc_re_flags_to_pcre2()` translator for the three flags pyc supports,
(3) adding real `IGNORECASE=2`/`MULTILINE=8`/`DOTALL=16` int-valued dict
entries to `makeReModuleDict()` (mirroring how `math.pi`/`math.e` are
real dict entries, not placeholders), (4) extracting `flags=` (keyword
or positional) in `Compiler.cpp`'s `re.*` AST-structural dispatch block,
the same technique `re.sub`'s pre-existing `count=` extraction already
used. Every `re.*` runtime function gained a trailing `flags` parameter
as a result (`PyBuiltin_ReSearch`/`ReFinditer`/`ReFindall`: 2→3 args;
`PyBuiltin_ReCompile`: 1→2 args; `PyBuiltin_ReSub`: 4→5 args;
`PyBuiltin_ReSplit`: 3→4 args) — a real, deliberate signature change
across the module, not additive-only.

**Adjacent gaps closed while touching this code, found along the way**:
`re.split`'s `maxsplit` parameter was declared but literally named as a
commented-out unused parameter (`PyObject* /*maxsplit*/`) — accepted
syntactically, silently ignored. Now implemented for real. `"split"` was
also missing from both `makeReModuleDict()` and the `re` entry in
`syntheticModuleExports()` — meaning `import re as x; x.split(...)`
(the non-structural, aliased-import path) would have failed to resolve
at all, even though `re.split(...)` (the literal-name path, which is
AST-structurally intercepted) worked. Both fixed.

**Fixed**: `re.match(...)` is now dispatched to a separate `PyBuiltin_ReMatch`
runtime function that compiles with `PCRE2_ANCHORED`, so it only matches at
the start of the string. `re.match("b", "abc")` correctly returns None.

### `bytes`/`bytearray` — Reopened a Previously-Declined Scope Decision
`FEATURES.md`/`IMPLEMENTATION.md` previously stated that a real `bytes`
type "was explicitly declined as a separate, larger project" in favor of
the str-as-byte-buffer convention `hashlib`/`base64`/`struct` used. The
user explicitly reopened that decision and asked for a real
implementation. Design, mirroring `pathlib.Path` (type 16)'s established
precedent of reusing the `str` field with just a new type tag rather than
inventing new storage:

- **Type tags 17 (`bytes`, immutable) and 18 (`bytearray`, mutable)**,
  both storing raw content in the existing `str` (`std::string`) field.
  This means **no `Codegen.cpp` struct-layout/alignment changes** (codegen
  only touches fields 0-3: refcount/type/value/dvalue directly) and **no
  `Py_DECREF` branch** (plain `delete obj` already runs `std::string`'s
  destructor) — the same two properties that made `complex` (type 13, a
  *genuinely new* value shape needing new fields) more invasive to add
  than `bytes` turned out to be.
- **Real bug found and fixed, not just a missing feature**: `b"..."`
  literals didn't merely fail to compile — `PythonParser.cpp`'s
  `Constant` handling fell through its catch-all `else` branch for any
  value that wasn't bool/int/float/str/None/complex, silently producing
  an **empty str literal**. `x = b"hello"; print(x)` used to print a
  blank line, no error, no warning.
- **Embedded-NUL literals (`b"\x00\x01"`) work correctly, with no
  text-escaping trick needed**, because the `Compiler.cpp`→`Codegen.cpp`
  pipeline is pure in-memory C++ objects (IR instruction operands are
  `std::string`, never re-serialized to text and re-parsed) — a
  `std::string`'s exact length, embedded NULs included, survives the
  whole pipeline for free. The one place NUL-safety was actually at risk:
  the existing `"const"` IR opcode's string-literal path hands its global
  string pointer to `PyUnicode_FromString` (`const char*` + implicit
  `strlen()` — NUL-truncating), even though the LLVM global itself is
  built length-correctly via `CreateGlobalStringPtr`. Fixed by adding a
  **dedicated `"bytesconst"` IR opcode** (mirroring the existing
  `"nconst"` opcode used for `None`) whose `Codegen.cpp` handler computes
  the length directly from the in-memory operand string and calls the new
  length-aware `PyBytes_FromStringAndSize(ptr, len)` instead.
- **A second, unrelated compiler bug found while wiring up construction**:
  `bytes(...)`/`bytearray(...)` calls initially compiled to always pass
  `null` for every argument, regardless of what was actually passed
  (`bytes(5)` produced `b''`, not `b'\x00\x00\x00\x00\x00'`). Root cause:
  `lowerCall`'s `neverDynamic`/`specialBuiltinNames` sets — the whitelist
  of builtin names that must collect their arguments normally into
  `argRes` rather than being routed through the dynamic `Pyc_Apply(token,
  ...)` path (used for calling arbitrary callable-valued names) — didn't
  include `"bytes"`/`"bytearray"`. Since no local variable named `bytes`
  is ever assigned, the dynamic path's callee-token lookup silently
  resolved to nothing, and its separately-built (empty) argument list was
  used instead of the real call-site arguments. Fixed by adding both
  names to both sets. This is the same class of "known builtin name
  routed through the wrong call-lowering path" bug as the `functools`/
  `operator` phase's `lastLambdaSynthetic` finding earlier in this
  session — worth checking this whitelist whenever a new zero/multi-arg
  builtin-style function name is added.
- **Semantics that genuinely diverge from `str`, not reusable verbatim**:
  indexing (`b"hi"[0]`) returns an **int** (0-255), not a length-1
  bytes object, unlike `str[i]`. Concatenation (`bytes + bytearray`)
  follows the **left operand's type** (matches CPython exactly, verified
  against real Python: `bytearray + bytes -> bytearray`, `bytes +
  bytearray -> bytes`).
- **Deliberate behavior change to already-shipped modules**:
  `hashlib.md5/sha1/sha256` now accept `str`/`bytes`/`bytearray`
  interchangeably (still more permissive than real CPython, which
  requires actual `bytes` — an already-documented, unchanged stance).
  `base64.b64encode`/`b64decode` and `struct.pack` now **return real
  bytes**, matching CPython exactly, instead of str — this changed
  printed output shape for already-existing test cases (`b'...'` instead
  of bare text), requiring their hardcoded expected strings to be
  updated. `hashlib.*().digest()` (raw bytes) is new, alongside the
  existing `.hexdigest()`.
- **Explicitly out of scope**: `bytes % formatting`, `memoryview`,
  `.join()`/full `.split()` parity, the buffer protocol, binary-mode file
  I/O (`open(path, "rb")` returning real bytes — no file read-as-bytes
  exists at all yet, unrelated to this phase).

### `decimal.Decimal` — Arbitrary Precision via `libmpdec`, Not a Fixed-Precision Approximation
The user was explicitly asked whether `decimal.Decimal` should be a
fixed-precision `(int64, scale)` approximation (no new dependency, less
work) or real arbitrary precision via a bignum library (matching real
CPython, which is itself built on `libmpdec`) — and chose the latter.

**Build dependency**: `libmpdec-dev` (Debian/Ubuntu package name),
found via `find_library(MPDEC_LIB mpdec)` in `CMakeLists.txt`, failing
loudly (`message(FATAL_ERROR ...)`) if not found — the same "fail loudly
like `find_package(LLVM REQUIRED ...)`" pattern already used there. It
was already installed on the development machine (`libmpdec.so`/
`libmpdec.a` at `/usr/lib/x86_64-linux-gnu/`, header found automatically
via the default multiarch include path — verified directly, no explicit
`-I` needed anywhere, including the separate `runtime.bc` LTO-bitcode
custom command). Linked into `pyc_runtime` alongside the existing
`pcre2-8` link; also added to the **compiled-program** final-link
commands in `Compiler.cpp` (`-lpcre2-8 -lmpdec`, 3 call sites) — these
are separate from `pyc_runtime`'s own CMake link step, since they're
plain strings building the `clang++` invocation used to link a *user's*
compiled program against `libpycrt.a`, found by grepping for the
existing `-lpcre2-8` occurrences and updating all of them the same way
(a step easy to miss, since it's not part of the normal CMake dependency
graph at all).

**Type tag 19.** Storage: unlike `complex` (type 13, which added new
native `double` fields directly to `PyObject` since it's a genuinely new
value shape), an `mpd_t*` is a heap-allocated opaque `libmpdec` struct —
stashed in the existing `value` field via pointer cast, the same pattern
already used for `CompiledRegex*`/`MatchObj*`/`PycDateTime*`/
`PycTimedelta*` (types 8/9/14/15). This means a `Py_DECREF` branch
calling `mpd_del()` is required — the one thing `complex` didn't need
(no out-of-struct payload) and, being type 13, got right by omission
rather than by a considered choice; important not to repeat that
omission for a type that actually owns a heap payload.

**Context**: one shared global `mpd_context_t`. `libmpdec`'s own
`mpd_defaultcontext()` differs from CPython's real defaults (38
significant digits, `MPD_ROUND_HALF_UP` — confirmed via a standalone
probe program before writing any integration code) — CPython's actual
default is 28 digits, `ROUND_HALF_EVEN`. Both are set explicitly via
`mpd_qsetprec`/`mpd_qsetround` after calling `mpd_defaultcontext()`,
verified end-to-end against real CPython (`Decimal(1) / Decimal(3)`
produces the identical 28-digit result in both). `decimal.getcontext()`/
`localcontext()` precision mutation is not implemented — every operation
uses this one fixed context.

**The key design difference from `complex` (quality, not just
feature-completeness)**: per the numeric-system research done before
implementing this, `complex`'s arithmetic is wired through a
compile-time-only `complexVars` tracking set in `Compiler.cpp` plus
hand-rolled `Codegen.cpp` call-site special-casing — which is why (see
the test-infrastructure section above) it's demonstrably broken across
function boundaries, for `==`, and for unary negation. Decimal is
instead wired into the **existing generic** `PyNumber_Add`/`Subtract`/
`Multiply`/`Divide`/`TrueDivide`/`Negate` functions (a `type==19` branch
added to each) and `PyObject_CompareBool`/`PyObject_TruthValue`. Since
`Compiler.cpp`'s `numericResultType()` is simply never taught to treat
type 19 as numeric, every Decimal arithmetic op automatically falls to
these generic functions regardless of whether the compiler statically
proved the value's type at that call site — confirmed working correctly
when a Decimal value crosses a function-parameter boundary untyped (see
the `add_decimals()` case in `tests/runner.py`), the exact scenario
`complex` gets wrong. Zero `Codegen.cpp` call-site special-casing was
needed for this, unlike `complex`'s `PyComplex_New` construction path.

**Mixed-type arithmetic**: `Decimal + int` auto-converts the int operand
via a small shared helper (`pyc_decimal_operand`, returning either the
Decimal's own `mpd_t*` directly or a freshly-converted temporary for an
int/bool operand). `Decimal + float` returns `NULL` (the existing
"unsupported operand combo" convention already used throughout
`PyNumber_*`) — confirmed this matches real CPython exactly: `Decimal('1.5')
+ 1.5` raises `TypeError` in real Python too, `Decimal` does not
implicitly coerce from `float`. Comparisons follow the same shape:
Decimal-vs-Decimal and Decimal-vs-int are exact (`mpd_qcompare`/
`mpd_qset_i64`, no lossy double coercion); Decimal-vs-float goes through
the same string-round-trip construction `Decimal(float)` uses internally
— an approximation, not exact binary comparison, matching this
codebase's established "don't gold-plate a rarely-hit edge" precedent
(e.g. `statistics`'s partial exact-int preservation).

**`.quantize(Decimal('0.01'))`** — construction is recognized
structurally in the AST (mirroring `hashlib.md5`/`pathlib.Path`/
`collections.deque`'s pattern) specifically so the result can carry a
`noteType(res, "decimal")` compile-time tag, needed to gate
`.quantize()`'s typeOf-based dispatch (`Decimal` itself takes only
positional arguments, so — unlike `csv.writer`/`groupby`, which need
AST recognition to read a keyword argument — the generic dict-dispatch
call convention would actually work fine for *construction*; the tag is
needed purely for the *method* dispatch afterward).

### `set` — Real Set Type (Type 20), Insertion-Ordered, Dedup-by-Value

Sets were entirely unimplemented (set literals printed `None`, set
comprehensions had no parser handler). Added a real set type backed by
`std::vector<PyObject*>` (`PyObject::setElems`) with linear-scan
value-equality dedup (`PyObject_CompareBool` op==0) — the same O(n)
approach the dict container already used. CPython sets don't guarantee
insertion order, but pyc's do (a deliberate choice: makes
`{x for x in iter}` match list-comp output for the common
single-iteration test cases, and is the simpler implementation).

- **Operators** (`|`/`&`/`-`/`^`) dispatch via `PyNumber_BitOr`/`BitAnd`/
  `BitXor`/`Subtract` — these check `a->type == 20` before the numeric
  paths, so set operators and integer bitwise ops coexist.
- **Comparison** (`==`/`!=`/`<=`/`<`/`>=`/`>`) maps to
  subset/superset/proper-subset/proper-superset in `PyObject_CompareBool`
  (sets have no lexicographic ordering in CPython either).
- **Method dispatch** (`lowerMethodCall`) gates set methods on
  `typeOf(obj) == "set"` and comes *before* the list-method branches
  that share names (`remove`/`pop`/`copy`/`update`/`clear`); the list
  `remove` guard additionally excludes `typeOf == "set"` to avoid the
  same name-collision silent-wrong-dispatch class already documented for
  `os.path.join`/`os.remove`.
- **`PyBuiltin_SetFactory`** (zero-arg `set` reference) mirrors the
  existing `list`/`dict`/`int`/`float`/`str` factory tokens, for
  `collections.defaultdict(set)`.
- **`set(iterable)` construction** emits `PySet_New` + `PySet_Update`,
  which materializes any iterable via `pyc_set_iter_to_list` (handles
  homogeneous int/float lists' `ilist`/`flist` storage — the same
  `pyc_ensure_boxed_list`-class bug found repeatedly elsewhere, fixed
  here by sizing the result list from `ilist.size()`/`flist.size()`
  rather than the empty boxed `list` vector).

### Severe, General, Pre-Existing Bug: `if`/`while`/Ternary Conditions on Boxed Non-Numeric Values Were Always False
Found while verifying `decimal.Decimal`'s truthiness (`bool(Decimal('0'))`
correctly returned `False`, but `if Decimal('5'):` — a condition, not a
`bool()` call — printed the falsy branch). Chasing that down surfaced a
**far more general** bug, unrelated to Decimal specifically and
predating this entire session: the `"br"` IR instruction's condition
codegen (`Codegen.cpp`, the single shared code path backing `if`,
`while`, and ternary `x if cond else y` — confirmed by inspecting
`--emit-llvm` output directly) handled a boxed (pointer-typed) condition
by unboxing its raw `.value` int64 struct field and comparing that to
zero:
```cpp
llvm::Value* unboxed = unboxToI64(cval);
cval = builder.CreateICmpNE(unboxed, ..., "cond.i1");
```
This is correct **only** for boxed `int`/`bool` (whose `.value` field
*is* the number) — for every other boxed type (`str`, `list`, `dict`,
`bytes`, `Decimal`, ...), `.value` is simply unused/zero regardless of
the object's actual content, so the comparison was unconditionally
false. Confirmed empirically: `s = "hello"; if s: print("truthy")`
printed nothing at all — for *any* non-empty string, list, or dict held
in a variable. (Boxed *numeric* conditions, and anything already
producing a native `i1`/`i32` — e.g. the result of a comparison via
`PyObject_CompareBool` — were unaffected, which is presumably why this
had gone unnoticed: comparisons like `if x > 0:` are extremely common
and work fine; bare truthiness checks like `if x:` on a non-numeric `x`
are what's broken, and evidently under-exercised by both this codebase's
own test suite before today and, apparently, by whatever workloads have
exercised pyc up to this point.)

Fixed by making `PyObject_TruthValue` (previously `static`, i.e. only
callable from within `Runtime.cpp` itself) callable from generated code:
dropped `static`, added a declaration in `include/pyc/runtime.h`, an
LLVM extern `FunctionType` declaration in `Codegen.cpp` (`(ptr) -> i32`),
and replaced the `unboxToI64`-based comparison in the `"br"` handler with
a direct call to it. This automatically also fixed `Decimal`'s
truthiness in conditions (no Decimal-specific code needed at the
codegen level) and gets the *numeric* boxed cases exactly right too
(`PyObject_TruthValue` already correctly handles int/float/bool/
Decimal), not just the newly-fixed non-numeric ones.

**A second, smaller bug found and fixed in the same few lines**: right
below the condition-type handling, a check meant to release the boxed
condition temp's reference after extracting its truth value —
```cpp
if (cval->getType()->isPointerTy())
    emitDecRefIfOwned(cname);
```
— was dead code in both the old and new version: by this point, `cval`
has *always* already been reassigned to an `i1` value (the whole point
of the preceding block), so this check could never be true, and the
boxed condition temp's ownership was never released here — a minor,
silent per-condition refcount leak (not a crash; confirmed via
`valgrind --tool=memcheck`, 0 errors before and after, with a small
baseline "definitely lost" figure present even in a control script with
no boxed conditions at all, i.e. unrelated to this specific bug). Fixed
by capturing whether the *original* loaded value was a pointer in a
separate `bool cvalWasPointer` before any reassignment, and checking
that instead.

**A third, smaller bug found and fixed while fixing the first**:
`PyObject_TruthValue`'s own list branch (`obj->type == 1`) checked
`!obj->list.empty()` unconditionally — the same
`pyc_ensure_boxed_list()`-class bug found repeatedly elsewhere in this
codebase (the `heapq`/`bisect`/`statistics` phase, and again in
`chain.from_iterable`): homogeneous int/float list literals store their
data in `ilist`/`flist` (`list_item_type` 1/2), not `list`, so this was
always empty (and thus always falsy) for e.g. `if [1, 2, 3]:` — while a
mixed-type list literal like `[1, "a", 2]`, forced onto the generic
boxed-list storage by its heterogeneous element types, correctly
evaluated truthy. `PyBuiltin_Bool` (the `bool()` builtin's own
implementation, a separate function) already had the correct three-way
`list_item_type` check — `PyObject_TruthValue` did not; fixed to match.

**Verification given the blast radius**: this change affects the
codegen path for essentially every `if`/`while`/ternary in every
compiled program, so verified unusually thoroughly — the full
`tests/runner.py` suite (319/319) and `test/import_tests/` (9/9) both
stay green, plus a dedicated `valgrind --tool=memcheck` run (0 errors)
on a script deliberately exercising boxed-condition truthiness inside a
function, a `while` loop, a ternary, and mixed with `Decimal`
construction — compared against a trivial-script valgrind baseline
(numeric-only conditions) to confirm the small "definitely lost" byte
count present in both is pre-existing/proportional-to-program-size, not
newly introduced by this fix (the fix can only *add* missing DECREF
calls relative to before, never remove ones that were previously
firing, since the DECREF call site was unconditionally dead code prior
to this fix — see the second bug above).

### Two More Real Bugs Found by Hunting for More Instances of the Same Class
After fixing the truthiness bug above, deliberately audited the rest of
`Runtime.cpp` for more of the same underlying pattern — a function
reading `obj->list` directly without checking `list_item_type` (the
homogeneous int/float fast-path storage, where the real data lives in
`ilist`/`flist` instead and `list` is empty). Most list-consuming
functions in this file were already correctly guarded (this exact bug
class had been found and fixed piecemeal several times before, in the
`heapq`/`bisect`/`statistics` phase and again in `chain.from_iterable`)
— but two real gaps remained, both found by grepping every
`->list.empty()`/`->list.size()` call site in the file and checking each
one against its surrounding `list_item_type` handling:

1. **`PyObject_CompareBool`'s list-comparison branch** (`a->type == 1 &&
   b->type == 1`, backing `==`/`!=`/`<`/`>`/`<=`/`>=` between two lists)
   read `a->list`/`b->list` directly with no guard at all — the exact
   same bug `PyObject_TruthValue`'s list branch had. Since two
   homogeneous int/float list literals both have an empty `.list`
   (their data is in `.ilist`/`.flist`), this branch's element-count
   check (`al.size() == bl.size()`) was always comparing `0 == 0` —
   **always true** — for two homogeneous lists, regardless of their
   actual contents or lengths. Confirmed against real CPython:
   `[1,2,3] == [1,2,4]` (different content) and `[1,2,3] == [1,2]`
   (different length) both incorrectly evaluated `True` under the old
   code. A mixed-type list literal like `[1, "a", 2]` is forced onto the
   generic boxed storage by its heterogeneous elements, so comparisons
   involving at least one mixed-type list were unaffected — which is
   presumably why this had gone unnoticed: `[1, 2, 3] == [1, 2, 3]`
   (same content, common in tests) also happens to look "correct" by
   the same accidental `0 == 0` logic, so only *unequal* homogeneous
   lists exposed the bug, and evidently weren't exercised together with
   equality checks anywhere in this codebase's existing test suite
   before now. Fixed by calling `pyc_ensure_boxed_list()` on both
   operands before reading `.list` — the same fix shape as
   `PyObject_TruthValue`'s.
2. **`list + list` concatenation wasn't implemented at all** — a
   different flavor of the same hunt, not a storage-representation bug
   this time but a flat-out missing feature: `PyNumber_Add` had no
   `type==1 && type==1` branch whatsoever, so `[1,2,3] + [4,5]` returned
   `None` unconditionally (confirmed against real CPython), regardless
   of whether either list used the homogeneous fast-path storage or not.
   Implemented as a new `PyList_Concat(a, b)` (normalizes both operands
   via `pyc_ensure_boxed_list()` first, then builds a fresh boxed result
   list with each element `Py_INCREF`'d — the same ownership shape as
   the existing `PyList_Repeat`, placed immediately above it in
   `Runtime.cpp`), wired into `PyNumber_Add` as `a->type == 1 && b->type
   == 1`. Verified against real CPython for homogeneous-int,
   homogeneous-float, mixed-type, and empty-operand combinations, plus
   `+=` (augmented assignment, which already routed through
   `PyNumber_Add` — no separate fix needed for that).

Both verified against real CPython, added to `tests/runner.py`, and
checked with `valgrind --tool=memcheck` (0 errors) given both touch
refcounting on every list comparison/concatenation.

### `del list[i]` Did Nothing At All — A Different Root Cause From the Same Hunt
Continuing the same audit turned up a third bug, though not another
instance of the `list_item_type` storage-representation pattern this
time — a plain missing dispatch branch. `Compiler.cpp`'s `del`-target
lowering for a `Subscript` target (`del obj[idx]`) called
`PyDict_DelItem(obj, key)` **unconditionally**, for any `obj`, with no
check on its runtime type at all:
```cpp
} else if (target->type == "Subscript") {
    // del d[k]
    ...
    ir.addInstruction(currentFunc, "call", {"PyDict_DelItem", obj, idx}, dummy);
```
`PyDict_DelItem` only acts when `dict->type == 2`, silently returning
`PyBool_New(0)` (no-op, no error) otherwise — so `del lst[i]` on *any*
list, not just ones using the homogeneous fast-path storage, has always
silently done nothing. Confirmed against real CPython: `lst = [1,2,3];
del lst[1]; print(lst)` printed `[1, 2, 3]` (unchanged) instead of
`[1, 3]`.

Fixed by adding a new `Pyc_DelItem(obj, key)` (placed next to
`Pyc_SetItem`/`Pyc_GetItem`/`Pyc_Subscript` — the existing
runtime-type-dispatching functions this naming convention already
belongs to) that checks `obj->type` at runtime: dict key deletion
(delegates to `PyDict_DelItem`) or list item removal by index
(normalizes via `pyc_ensure_boxed_list()` first, then `Py_DECREF`s the
removed element and erases it from `.list`), raising `IndexError` with
CPython's exact message (`"list assignment index out of range"`, verified
against real CPython) for an out-of-range index. `Compiler.cpp`'s
del-Subscript lowering now calls this instead of `PyDict_DelItem`
directly.

**A second, smaller, genuinely separate bug found while verifying the
fix above** (pre-existing in `PyDict_DelItem` itself, not introduced by
the `Pyc_DelItem` change — this one has nothing to do with lists):
`PyDict_DelItem` never raised `KeyError` for a missing key, silently
returning `PyBool_New(0)` instead — confirmed `del d["missing"]` on a
real dict raised nothing at all under the old code, where real CPython
raises `KeyError: 'missing'`. Fixed to match `Pyc_Subscript`'s existing
`KeyError`-raising convention exactly (same `pyc_make_exc`/`pyc_raise`
pattern, raw key object as the message so `pyc_exc_message`'s existing
repr-quoting applies). Verified `except KeyError as e: print(e)` now
matches real CPython's `KeyError: 'missing'` exactly. (Also noticed,
but explicitly did **not** chase down at the time, since it's a
different and narrower pre-existing gap unrelated to `del`/dict/list:
`type(e).__name__` on a caught exception instance printed `None`
instead of the exception's class name — an exception-object type-
introspection formatting issue, not part of this bug hunt's pattern at
the time. Fixed on a later pass — see the dedicated entry further down.)

### User Variable Names Could Collide With the Compiler's Internal Temp Namespace — Fixed
Found by continuing the same bug hunt into `copy.copy()`. This one was
architecturally unlike the others above — not a storage-representation
mismatch or a missing dispatch branch, but a genuine **namespace
collision**: the compiler allocates internal temporary IR values using
names of the form `t<N>`/`c<N>`/`i<N>`/`s<N>` (`tempCounter`, reset per
function), inlined directly as `"t" + std::to_string(tempCounter++)` and
three sibling patterns at **392 separate call sites** across
`Compiler.cpp` (not centralized through one helper). These names lived
in **the same namespace** as real user variable names — nothing
prevented a Python variable actually named `t3` or `c0` from being
treated as, and silently confused with, one of these internal temps.
Originally documented here as "too large in scope to fix in this pass";
revisited and fixed on explicit request.

**Confirmed, reproduced, both failure modes exist**:
- **Silent data corruption** (the worse of the two): `c0 = "hello";
  print(c0)` prints `0` instead of `"hello"` — no error, no crash, just
  a wrong answer, because `c0` collided with an unrelated
  compiler-generated constant temp that happened to also be named `c0`
  in that function.
- **Hard compile failure**: `b = [1, 2, 3]; import copy; c2 =
  copy.copy(b)` fails LLVM module verification entirely (`Store operand
  must be a pointer` — a malformed `store ptr %t10, i64 3` instruction)
  and refuses to produce a binary. Bisected precisely: renaming the
  variable from `c2` to anything else (e.g. `x`) makes the exact same
  program compile and run correctly; `copy.deepcopy` (vs. `copy.copy`)
  on the identical `b` doesn't trigger it either — the collision is
  about the *variable name* `c2` matching an internal temp generated
  around that point in that function, not about `copy.copy` itself.
- **Not deterministic per-name in isolation**: a function parameter
  literally named `t3` compiled and ran *correctly* in one test. Whether
  a given `t<N>`/`c<N>`-named variable collides depends on the exact
  sequence of temp allocations already performed in that specific
  function by the time the name is used — which can change with
  unrelated edits elsewhere in the same function. This makes the bug
  genuinely unpredictable from the outside, not a fixed list of "these
  N names are always broken."

**Why `tempCounter` resets to 0 per function matters for real-world
risk**: since every function starts counting from 0 again, *low*
numbers (`t0`, `t1`, `c0`, `c1`, `c2`, ...) are generated near the start
of essentially every function body — these are exactly the
short-name-plus-small-number style names a programmer might plausibly
choose (loop temporaries, coefficients `c0`/`c1`/`c2` in numeric code),
making this more than a purely theoretical risk, even though it's still
an unusual naming style overall.

**The fix**: of the two shapes considered when this was first documented
(a validation pass rejecting collision-prone names, or closing the
collision permanently by changing the temp-naming prefix), the second
was chosen — it removes the hazard entirely rather than merely detecting
it, and turned out to be mechanical rather than a redesign. Python
identifiers are restricted to `[A-Za-z0-9_]` (plus non-ASCII letters,
never ASCII punctuation); LLVM's local-value-name syntax explicitly
permits `$` unquoted (already used throughout this codebase's own
runtime bitcode, e.g. in Itanium-mangled C++ template symbols pulled in
by the LTO step) and accepts arbitrary characters when quoted regardless.
Prepending `$` to all four generated prefixes — `t`/`c`/`i`/`s` all
became `$t`/`$c`/`$i`/`$s` — makes every internal temp name something no
valid Python identifier can ever be, closing the collision permanently,
while remaining completely unremarkable as an LLVM value-name hint (LLVM
prints it as `%"$c0"` — quoted, but syntactically ordinary, no different
in kind from names it already quotes elsewhere).

Applied as a single mechanical, literal-string substitution across all
392 call sites (a scripted find-and-replace of the four exact generation
expressions, not a hand-edit of each site — verified no comment or
unrelated string literal in `Compiler.cpp` coincidentally matched the
substituted text first). Nothing outside `Compiler.cpp` inspects or
parses these name strings by character content (confirmed by grepping
`Codegen.cpp`/`LLVMDCE.cpp`/`IR.cpp` for any code that reads the first
character of an operand name), so no other file needed changes. Longer,
already-low-collision-risk generated names (`__nesteddef_`,
`__lc_lst_`, `assert_fail_`, and similar `__`- or word-prefixed labels
used elsewhere in the same file) were left as-is — they were never the
confirmed collision vector and aren't standalone short names a
programmer would plausibly pick.

Verified against both original repros (`c0 = "hello"; print(c0)` now
correctly prints `hello`; `b = [1,2,3]; import copy; c2 = copy.copy(b)`
now compiles and runs correctly) plus a broader sweep of previously
collision-prone names (`t0`, `t1`, `c0`, `c1`, `i0`, `i1`, `s0`, `s1`,
function parameters `t2`/`c3`, a loop-local `t3`, and instance attributes
`c5`/`t9`) — all match real CPython exactly. Full suite and import tests
stay green; `valgrind --tool=memcheck` shows 0 errors on both a mixed
collision-name smoke test and the existing `nbody.py` numeric benchmark.

### `tuple`, `divmod`, `pow` — the Same `neverDynamic` Bug Class Found in an Earlier Phase, With More Victims
Continuing the hunt (directed to keep going after the namespace-collision
finding above) surfaced a fourth bug, a recurrence of a bug class already
found and fixed once this session for `bytes`/`bytearray`: `lowerCall`'s
`neverDynamic`/`specialBuiltinNames` sets are a whitelist of builtin
names that must collect their call arguments normally; anything *not* on
the list gets routed through the dynamic `Pyc_Apply(token, ...)` path
instead (the mechanism that makes `f = some_func; f()` work for
first-class function values) — and since no local variable named
`tuple`/`divmod`/`pow` is ever assigned in ordinary code, that path's
callee-token lookup silently resolves to nothing, so the call
unconditionally returns `None`, even though each of these already has a
correctly-implemented dispatch branch in `lowerCall` that's simply never
reached. Confirmed: `tuple([1,2,3])`, `divmod(17,5)`, `pow(2,10)` all
returned `None` unconditionally under the old code.

Found by systematically grepping every `funcName == "..."` branch in
`lowerCall` and diffing the resulting name list against
`neverDynamic`— not every name missing from that diff turned out to be
broken (`reversed`, `combinations`, `cmp_to_key` were also missing but
work correctly, apparently via a different, not fully investigated,
fallback path — not pursued further since they weren't actually
broken); `tuple`, `divmod`, and `pow` were confirmed broken by direct
testing and fixed by adding all three to both sets.

`tuple(x)` was made to behave exactly like `list(x)` rather than
implementing real tuple semantics — pyc has no distinct tuple type at
all (tuple *literals* like `(1, 2, 3)` already map straight to `list`
internally, an existing, deliberate, documented scoping decision predating
this session, not a new gap introduced here), so `tuple([1,2,3])`
printing as `[1, 2, 3]` rather than CPython's `(1, 2, 3)` is consistent
with that existing choice, not a fresh inconsistency. `divmod()`
likewise already returned (and still returns) a 2-element list rather
than a tuple, for the same reason.

**A second, separate, smaller gap found while fixing 2-arg `pow()`**:
3-arg modular exponentiation (`pow(base, exp, mod)`) had never been
implemented at the runtime level at all — `lowerCall`'s `pow` branch
only ever passed 2 arguments to `PyBuiltin_Pow` regardless of how many
were given, so `pow(2, 10, 1000)` silently ignored the modulus and
returned the un-modded `1024` instead of `24`. Implemented as a new
`PyBuiltin_Pow3` using fast modular exponentiation (avoids overflow for
larger exponents than the naive repeated-multiplication approach
`PyBuiltin_Pow` itself uses for the 2-arg case), verified against real
CPython for positive/negative modulus and the exact `ValueError`
message CPython raises for a zero modulus (`pow() 3rd argument cannot
be 0`) and a negative exponent with a modulus specified.

**Test-writing lesson repeated from the complex-numbers case earlier**:
the first version of this test printed `tuple(x)`'s and `divmod()`'s
raw results directly, which made this runner's live-CPython comparison
(always preferred over the hardcoded `expected` string when the source
runs successfully under real CPython) permanently disagree with pyc's
correct-for-its-own-architecture list-shaped output — rewritten to index
into / unpack the results instead of printing the container directly,
sidestepping the representation difference entirely rather than fighting
the test harness's comparison priority.

### `{**mapping}` Dict-Literal Unpacking Silently Lost Data — Fixed
Found by continuing the hunt into other container-literal constructs.
Real Python's `ast.Dict` node represents a `**expr` entry inside a
`{...}` literal (e.g. `{**d1, "x": 1, **d2}`) with a **`None` key** in
its `keys` list — the paired `values` entry is the unpacked mapping
expression itself (`d1`, `d2`). `PythonParser.cpp`'s `Dict` handling
never special-cased this: it called `buildAST(key, keyChild.get())`
unconditionally for every key, and `buildAST` has no way to process a
bare Python `None` object as if it were a real AST node (`None` has no
`.type`/attributes to extract), producing a garbage child instead of
either working correctly or failing clearly. Confirmed against real
CPython: `{**d1, **d2}` printed as `{None: {'b': 2}}` — `d1`'s entries
were silently lost entirely, not merely reordered.

Fixed in two coordinated places: `PythonParser.cpp` now detects the
`None`-key sentinel and tags that entry's key child with a distinct
`"DictUnpack"` node type instead of trying to `buildAST` a `None`;
`Compiler.cpp`'s `lowerDict` checks for that tag and emits a
`PyDict_Update(dictRes, src)` merge call for that entry instead of the
normal `PyDict_SetItem(dictRes, key, val)` — reusing the existing
`PyDict_Update` function already used for `dict.update()`, no new
runtime code needed. Verified against real CPython for
`{**d1, **d2}`, `{**d1, "c": 3}` (mixing unpacked and literal entries),
and `{**d1, **d3}` where `d3` also has a key `d1` has (confirming the
later mapping's value correctly wins on key conflict, matching real
Python's merge-order semantics). The permanent test checks individual
keys via subscript rather than comparing the merged dict's full repr;
dict iteration/print order now matches insertion order (the dict payload
was changed from `std::unordered_map` to `std::vector<std::pair>` after
this fix was written — see the "dict iteration order" note above), so
the original caution about ordering no longer applies, but the
subscript-based test remains valid and more precise.

### F-String Format Specs (`:.2f`, `:05d`, `:>10`, ...) and `!r`/`!s`/`!a` Conversion — Fixed
Found alongside the dict-unpacking bug while testing other f-string/
container-literal edge cases. `f"{x:.2f}"` used to print the
*unformatted* value (`3.14159`, not `3.14`) — the entire `:spec` portion
of a `FormattedValue` node was silently discarded. Unlike the
dict-unpacking bug above, this wasn't an undiscovered bug so much as a
previously undocumented, pre-existing, deliberate MVP-era scope cut:
`PythonParser.cpp`'s `FormattedValue` handling had an explicit comment,
predating this session — `// format_spec (e.g. :.2f) — skip for MVP` —
right where the `format_spec` attribute needed to be read from Python's
AST. The `!r`/`!s`/`!a` conversion flag (the `node->op` field) *was*
correctly captured by the parser but was then **also** never read by
`Compiler.cpp`'s `lowerFormattedValue`, which unconditionally called
`PyStr_FromAny` regardless of the requested conversion — so `f"{x!r}"`
and `f"{x}"` used to produce identical output too. Originally documented
here as a substantial, self-contained feature project rather than a
bounded bug fix; implemented on explicit request, alongside `str.format()`
(see further down) since both need the same underlying formatter.

**The implementation**:
- `PythonParser.cpp` now captures `format_spec` as a real `children[1]`
  AST subtree (when present) rather than skipping it. `format_spec` is
  itself a `JoinedStr` node in Python's AST — it can contain nested
  expressions for a dynamic width/precision (`f"{x:{width}.{prec}f}"`)
  — so it's captured as a full subtree and lowered exactly like any
  other `JoinedStr` (reusing the existing `lowerJoinedStr`/`lowerExpr`
  machinery to produce a plain runtime string), rather than assuming
  it's always a static literal. This got dynamic specs working "for
  free," with no separate static-vs-dynamic code path.
- A new runtime function, `Pyc_FormatValue(value, specStr)`
  (`Runtime.cpp`), implements a practical subset of Python's Format
  Specification Mini-Language: `[[fill]align][sign]["#"]["0"][width][","|"_"]["." precision][type]`,
  covering fill/align (`<>^=`, with an optional fill character), sign
  (`+`/`-`/space), `#` (alternate form — implemented for integer base
  prefixes `0b`/`0o`/`0x`, not for floats' always-show-decimal-point
  behavior), `0` (zero-pad), width, `,`/`_` (thousands grouping),
  precision, and type codes `s`/`d`/`b`/`o`/`x`/`X`/`f`/`F`/`e`/`E`/`g`/`G`/`%`/`c`.
  Not implemented (documented, not chased down): `n` (locale-aware —
  treated as a plain numeric type instead), `#` for floats, and
  `decimal.Decimal` operands with a numeric type code (falls back to
  plain `str()` + padding rather than the mini-language's numeric
  formatting).
- `Compiler.cpp`'s `lowerFormattedValue` now reads the conversion code
  (applying `PyBuiltin_Repr` for `!r`/`!a`; `!s` and the no-conversion
  default are treated identically, since pyc has no `__format__`
  protocol for the two to meaningfully diverge on) and lowers
  `format_spec` when present, then calls `Pyc_FormatValue`.

Verified against real CPython across float precision/width/align/sign/
thousands-separator, int width/zero-pad/hex-octal-binary/thousands-
separator, string width/align/precision-truncation, percentage type,
dynamic width+precision, negative-number zero-padding (`f"{-42:05d}"`
→ `"-0042"`, sign staying left of the zero-fill), and `!r` conversion.
Added as permanent `tests/runner.py` regressions. Full suite and import
tests stay green; `valgrind --tool=memcheck` shows 0 errors.

### Assigning a Native Comparison Result to a Variable Crashed LLVM Verification — Fixed
Continuing the hunt turned up a severe, broad-impact bug distinct from
the earlier truthiness bug (which affected `if`/`while`/ternary
*conditions*, not assignments) — but caused by the same underlying
pattern: the codegen path for "native" (unboxed) values didn't fully
agree with itself about who's responsible for boxing an `i1`.

The `"icmp"` IR opcode has a fast path for comparisons between two native
`i64`/`i64` or `double`/`double` operands: it stores the raw, unboxed
LLVM `i1` result directly in the codegen value map, with an existing
comment promising that "when the result is used in a non-branch context
(e.g. assigned to a variable...), `getAsPyObject` boxes it lazily via
`PyBool_New`." That promise held for call arguments (`getAsPyObject`
does correctly handle `i1` via `CreateZExt` + `PyBool_New`) but **not**
for the `"assign"` opcode's own "box native values" section, which only
handled `i64` (`boxI64`) and `double` (`boxDouble`) — silently falling
through with no boxing at all for `i1`, so the raw 1-bit LLVM value got
stored into a `PyObject**` slot and then passed to `Py_INCREF` as if it
were already a pointer.

Confirmed via minimal repros: `x = 1 < 2` (literal comparison), `x = 1 <
2 < 3` (chained comparison), `def f(a, b): return a < b` called with
plain int arguments (comparison of function parameters proven native by
the compiler's own type inference), and `flag = i < 3` inside a `for i
in range(n):` loop (an entirely ordinary "name a comparison result"
idiom) all either crashed LLVM module verification outright (`Store
operand must be a pointer`, or `Call parameter type does not match
function signature! i1 true ... call void @Py_INCREF(i1 true)`) or, in
some builds, silently miscompiled. Fixed by adding an `i1` branch to the
`"assign"` opcode's boxing switch in `Codegen.cpp`, mirroring
`getAsPyObject`'s existing correct handling exactly (`CreateZExt` to
`i32`, then `PyBool_New`). Verified fixed against all four repros,
matching CPython exactly; added as permanent regression cases in
`tests/runner.py`.

### `sorted()`/`.sort()` `reverse=` and `key=`, `min()`/`max()` `key=` — All Silently Ignored, Now Fixed
Continuing the hunt, `sorted(x, reverse=True)`, `list.sort(reverse=True)`,
`list.sort(key=...)`, and `min`/`max`'s `key=` argument were all found
completely unimplemented: the keyword was silently dropped and the
plain unmodified ascending sort (or the un-keyed min/max) was returned
instead. Confirmed against real CPython for each.

**Root cause, shared by all of them**: `sorted`, `min`, and `max` are
builtins with no entry in `funcParamNames` (that map only tracks
user-defined function signatures). `lowerCall`'s generic keyword-argument
handling has a fallback for exactly this case — "no known parameter
list, so just append every keyword's value onto the end of the
positional-argument list" (`for (auto& kw : kwArgs) argRes.push_back(kw.second);`).
That fallback is fine for call sites that don't separately try to
recover a specific keyword's meaning from `argRes` by position — but
each of `sorted`/`min`/`max`'s own special-case lowering *did* exactly
that: `sorted`'s handler treated `argRes[1]` as a positional `key=`
argument whenever `argRes.size() >= 2`, and `min`/`max`'s handler
treated *any* second-or-later `argRes` entry as another value to
compare. Once `reverse=`/`key=` support was added and their values
started landing in `argRes` via the generic fallback, both handlers
misread the appended keyword value as something else entirely:
`sorted([3,1,2], reverse=True)` had its `reverse` boolean misread as a
key *function*, corrupting the sort into `[2, 1, 3]`; `min([3,1,2],
key=lambda x: -x)` had the lambda misread as a second value to compare
against the list itself, printing the lambda object.

Fixed by capturing `posArgCount` — the true positional-argument count,
already computed earlier in `lowerCall` *before* the generic kwarg-append
fallback runs — and using it instead of `argRes.size()` when deciding
whether a given `argRes` slot is a real positional argument versus a
keyword value that got appended afterward. `key=`/`reverse=` are now
extracted directly from `kwArgs` by name (as they already were for the
cases that worked), and the position-based fallback only fires when
`posArgCount` proves the value was genuinely positional.

Runtime changes: `PyBuiltin_Sorted`/`PyList_Sort` gained a 3rd `reverse`
parameter (`std::reverse` on the result after the existing sort/key
logic); `PyList_Sort` also gained the `key=` support `.sort()` never had
at all (mirrors `PyBuiltin_Sorted`'s existing key-based approach:
compute a key per element via `Pyc_Apply`, sort an index vector, reorder
`.list` directly). `PyBuiltin_Min2`/`Max2`/`MinList`/`MaxList` all gained
a `key` parameter (a new shared `pyc_apply_key1` helper applies it once
per compared item, comparing keys instead of raw values while still
returning the original item). `cmp_to_key(cmp)`'s comparator-based sort
path was **not** extended with `reverse=` support — documented as a
narrower remaining gap (that specific combination wasn't hit by this
hunt).

Verified against real CPython for every combination: `sorted()` with no
args, `reverse=` alone (both `True` and `False`), `key=` alone, `key=`
and `reverse=` together, `.sort()` with the same four combinations, and
`min`/`max` with `key=` in both the multi-arg (`min(3, 1, key=...)`) and
single-iterable (`min([3,1,2], key=...)`) forms. All added as permanent
`tests/runner.py` regression cases. Full suite (340/340) and import
tests (9/9) stay green; `valgrind --tool=memcheck` shows 0 errors.

### `@classmethod`/`@property`/`@staticmethod` Decorators Were Silently Discarded on Methods — Fixed
Continuing the hunt into class features turned up a bug architecturally
similar in size to the `tempCounter` namespace-collision finding above —
not a missing dispatch branch or a storage-representation mismatch, but
a decorator that was recognized by the parser and then thrown away.
`lowerClass`'s method-lowering loop filtered `Decorator` children out of
the AST nodes it lowered into the method body and never inspected the
decorator list itself anywhere else — every method, regardless of
`@staticmethod`/`@classmethod`/`@property` or no decorator at all, was
registered in the class dict identically: a plain callable token whose
first parameter was positionally bound whenever the method was called.
Originally documented here as "too large in scope to fix in this pass";
revisited and fixed on explicit request.

**Confirmed, reproduced, three distinct failure modes (all now fixed)**:
- **`@classmethod`, called via the class**: `A.cm()` where `cm` read
  `cls.x` returned `None` instead of the class attribute's value — `cls`
  was simply never bound to anything when there was no instance to
  supply it.
- **`@classmethod`, called via an instance**: `a.cm()` (same method)
  returned the *correct* answer, but only by accident — `cls` actually
  got bound to the instance `a`, and instance attribute lookup happened
  to fall back to the class dict for `cls.x`. This "worked" for
  read-only attribute access but broke for anything relying on `cls`
  genuinely being the class object (e.g. `cls()` to construct a new
  instance would have called the instance, not the class).
- **`@property`**: `a.doubled` (no call parens — plain attribute
  access) never invoked the getter at all — attribute access resolved
  it to the method's raw callable-token string and returned *that*
  unevaluated (`print(a.doubled)` printed `A__doubled`, the method's
  internal compiled function name; `type(a.doubled)` printed `<class
  'str'>`, instead of CPython's `42` / `<class 'int'>`).
- **`@staticmethod` appeared to work but for an unrelated reason**: a
  zero-arg static method called via the class (`A.sm()`) happened to
  produce the right answer purely because it took no `self`/`cls`
  parameter, so there was no positional-binding mismatch to expose. A
  static method taking *real* parameters, called via an instance
  (`instance.static_method(1, 2)`), was genuinely broken — `self` got
  wrongly prepended on top of the real arguments.

**The fix**, rather than the two- or three-file redesign originally
assumed necessary, turned out to be a single new runtime dispatch point
plus a lightweight compile-time tag:

- `lowerClass` now detects `@staticmethod`/`@classmethod`/`@property` on
  each method (scanning its `Decorator` children for a matching `Name`,
  mirroring the existing top-level-function decorator detection it never
  had a method-level counterpart for) and, when found, stores the
  method's class-dict entry as a tagged 2-element list `[kind,
  realToken]` instead of a bare token string. A plain (undecorated)
  method's entry is still just a bare string — purely additive, no
  change to the always-worked path.
- A new runtime function, `Pyc_CallMethod(methodVal, receiver, argsList)`
  (Runtime.cpp), is the single dispatch point both call shapes now go
  through — `Compiler.cpp`'s `lowerMethodCall` no longer decides
  self/cls-prepending itself. It inspects `methodVal`'s shape: a bare
  token (plain method) prepends `receiver`, *unless* `receiver` is
  itself a class dict (has `__mro__`) rather than an instance (has
  `__class__`) — that's Python's "unbound method" call shape,
  `ClassName.method(instance, ...)`, where the caller already supplied
  self explicitly and nothing should be prepended (this exact behavior
  already worked before the fix, purely by coincidence of how a class
  reference's `typeOf` happened to be tagged — now it's an explicit,
  intentional check). `@staticmethod` never prepends anything.
  `@classmethod` prepends *the class*, not the raw receiver — resolved
  via the same `__mro__`/`__class__` check, so it's correct whether
  called via `ClassName.method()` (receiver is already the class) or
  `instance.method()` (receiver is an instance; the class is
  `receiver.__class__`).
- A second new runtime function, `Pyc_GetAttr(obj, attrName)`, wraps the
  existing `Pyc_GetItem` specifically for `lowerAttribute`'s bare
  (non-call) attribute-read path: if the looked-up value is a
  `"property"`-tagged marker, it calls the getter with `self=obj` and
  returns the result instead of the raw marker. Every other internal
  `Pyc_GetItem` call site (module dispatch, method-token lookups during
  a call, class-dict internals) is deliberately left untouched, since
  those must never trigger property auto-invocation.
- `Compiler.cpp` also gained a new, explicit `ClassName.method(...)`
  branch in `lowerMethodCall`, checked *before* the pre-existing generic
  "receiver's static type is `dict`" fallback (a class reference's
  `typeOf` is tagged `"dict"` for an unrelated reason — reusing module-
  namespace-style dispatch — and would otherwise have swallowed this
  case with no way to distinguish it from `sys.stderr.write(...)`-style
  dispatch). Both this new branch and the existing `instance.method(...)`
  fallback now route through the same `Pyc_CallMethod`.

**A debugging note worth keeping**: the first attempt at the new
`ClassName.method(...)` branch also gated on `!isShadowedLocal(name)`,
by analogy with a similar check used elsewhere for builtin exception
names — this silently defeated the branch entirely, because every class
name gets `noteType(className, "dict")` called at its own definition
site, and `isShadowedLocal` treats *any* `valueTypes` entry as
"shadowed." Removed; a local variable that happens to share a class's
name is a narrower, pre-existing ambiguity the old fallback never
resolved either.

Verified against real CPython: `@classmethod` called via the class and
via an instance (both correctly see `cls`, not an instance);
`@classmethod` mutating a class attribute (`cls.x += 1`); `@property`
computing a value from `self`; `@staticmethod` with real parameters
called both via the class and via an instance; the unbound-method idiom
(`ClassName.method(instance, ...)`); and multi-level inheritance with
`super()` (unaffected by the dispatch rewrite, still correct). Added as
permanent `tests/runner.py` regressions. Full suite and import tests
stay green; `valgrind --tool=memcheck` shows 0 errors.

**A related, more severe bug found and fixed while testing the above**:
`x ** N` for a small constant integer `N` (0–8) uses a
repeated-multiplication fast path (`lowerBinOp`) that decided "is this
complex arithmetic" via `typeOf(left) == "boxed"` — but `"boxed"` means
only "not statically known to be int/float," not "is complex." Any
function parameter or other untyped value (the overwhelmingly common
case for a method body, but not limited to methods — confirmed via the
plain top-level `def f(y): return y ** 2`) was misrouted through
`PyComplex_Mul` instead of ordinary multiplication: a silent wrong
answer (`None`) for an int-valued argument, and for some float-valued
arguments an outright compiler crash (an LLVM assertion failure,
`Invalid operand types for ICmp instruction`). Fixed by checking the
actual `complexVars` tracking set instead — the same check already used
correctly for the regular (non-power) complex add/sub/mul/truediv
dispatch a few lines below. Verified against real CPython for both a
plain function and a method; added as a permanent regression.

**Found while testing the above, fixed on a later pass: `x ** N` with a
float argument still crashed the compiler.** `def f(y): return y ** 2;
f(3.5)` failed the same LLVM assertion (`Invalid operand types for ICmp
instruction`), even as the only call site — this survived the
`isComplex` fix above (a different, already-fixed symptom). Root cause
was a *third*, independent bug, one level deeper: `inferParamTypesFromBody`
statically guesses a parameter's type from its usage within the function
body alone (`y ** 2` looks numeric, so it guessed `y` as `int`) — this
runs *before* any call-site type is known, since a function's body is
lowered before its callers are seen. That guess got baked into the `mul`
instruction's `resultType` (via the pow-expansion fast path) and into
the function's inferred return type. When the *only* real call site
later turned out to pass a `float`, call-site analysis correctly
allocated the parameter as a native `double` — but codegen still trusted
the stale `resultType="int"`/`nativeReturnType="int"` tags baked in
earlier, routing a native `double` value into the `int64`-unboxing path
(`unboxToI64` → `CreateIsNull` on a `double`), an LLVM type mismatch
that crashed compilation outright rather than just producing a wrong
answer.

Fixed at two levels: (1) `Codegen.cpp`'s native int/float dispatch
(shared by `mul`/`add`/`sub`, and duplicated similarly for `div`/`mod`)
now refuses the int path whenever an operand actually resolved to a
native `double` at codegen time, falling through to the existing,
correct float path instead — a general defensive fix against this whole
category of "static guess disagreed with the eventual real type," not
just the pow case specifically; (2) `Compiler.cpp`'s specialized-variant
generation (A6) no longer propagates a native return type onto a
variant whose call-site-derived signature disagrees with the earlier
body-only int/float guess for any parameter, avoiding an LLVM
function-type/body mismatch at the declaration level too. Verified
against real CPython: the exact crashing repro, the same function also
called with an int (still takes the fast native path, unaffected), `*`/
`//`/`%` directly (not just via the pow fast path), and a
purely-int-typed function (no float anywhere) confirmed unaffected.
Added as a permanent `tests/runner.py` regression.

**Found while testing the above, fixed on a later pass: dynamic class
instantiation via a variable didn't work.** `X = Foo; X()` (or `cls()`
inside a plain function, or a class value pulled from a container, e.g.
`registry["foo"](7)`) returned `None` instead of a new instance. Root
cause: the "instantiate a class" call-site logic in `lowerCall`
recognizes a class instantiation *structurally*, by checking whether the
literal `Name` being called matches a `knownClasses` entry at compile
time — a class reference reached through a variable never matches, so
the call fell through to the generic `Pyc_Apply` dynamic-dispatch
fallback instead, which had no equivalent "create an instance dict +
call `__init__`" behavior (a dict-typed token, the class dict itself,
matched none of `Pyc_Apply`'s recognized callable shapes).

Fixed by teaching `Pyc_Apply`'s runtime fallback to recognize a class
dict (has a `"__mro__"` key) and, when found, construct a new instance
dict, bind `"__class__"` to it, resolve `__init__` by walking the
class's `__mro__` (a new `pyc_lookup_via_mro` helper, mirroring
`PyBuiltin_SuperMethod`'s existing MRO-resolution pattern but starting
at index 0 instead of "just past the defining class"), and call it if
found. This check runs *before* the existing `__call__`-dispatch check
in the same fallback: a class's own dict entries are its *instance*
methods, so a class defining `__call__` for its instances would
otherwise make the `__call__` lookup spuriously match on the class dict
itself (which has no bound instance to call it on).

This surfaced two further, more severe, genuinely pre-existing bugs
along the way, both fixed here too:
- **`__init__` default-argument globals were keyed by the bare literal
  `"__init__"`, shared across every class in the module, instead of
  per-class.** With two or more classes each defining an `__init__` with
  a default at the same positional index, they all pointed at the same
  global storage slot, so whichever class's default assignment ran last
  at module-init time silently clobbered every earlier class's default
  value — for *any* instantiation, structural or dynamic. Confirmed:
  `class A: def __init__(self, n=1)` followed by `class B: def __init__
  (self, n=2)` elsewhere in the same file, then `A()` (zero arguments)
  incorrectly returned `n=2`, not `1`. This also meant `IRFunction::
  defaultGlobals` was never populated for any `__init__` at all, so
  `Codegen.cpp`'s indirect-call adapter (used by `super().__init__()`, a
  stored bound-method reference, or the new dynamic-instantiation code
  above) had no way to find the default value and silently passed a null
  argument instead — confirmed via `super().__init__()` on a base class
  with a defaulted parameter, which printed `None` instead of the
  default. Fixed by keying the default-slot global names,
  `funcDefaultCount`/`funcDefaultValues`, and `IRFunction::defaultGlobals`
  all by the per-class `initFuncName` (e.g. `"Foo__init__"`) instead of
  the shared literal `"__init__"`.
- **`Pyc_CallMethod` and `PyBuiltin_SuperMethod` each leaked a small
  temporary argument list on every call.** Both build a fresh list to
  prepend `self`/`cls` before calling `Pyc_Apply`, and neither freed it
  afterward — a real (if small) refcount leak on *every*
  `instance.method()` call and every `super()` call, found via valgrind
  while verifying the fix above and confirmed present on the unmodified
  commit for a plain, unrelated method call. Fixed by capturing
  `Pyc_Apply`'s result before `Py_DECREF`ing the temporary list.

Verified against real CPython: `X = Foo; X()` with and without
`__init__` arguments/defaults, a class value from a dict lookup, a class
passed as a plain function parameter, `super().__init__()` with a
defaulted base-class parameter, and two classes in the same module each
defaulting a same-position `__init__` parameter to different values.
Added as a permanent `tests/runner.py` regression. Full suite and import
tests stay green; `valgrind --tool=memcheck` shows 0 new errors (fewer
than before, thanks to the leak fix above).

### Several Common `str` Methods Were Entirely Unimplemented — Fixed
While probing string methods during the same hunt, `.format()`,
`.rsplit()`, `.partition()`, and `.rpartition()` were all found to have
**zero** dispatch code anywhere in `Compiler.cpp` — not a missing-branch
bug like `tuple`/`divmod`/`pow` earlier (those had working
implementations that were simply unreachable), just genuinely
unimplemented methods, each silently returning `None` instead of raising
an error or working. Originally documented here as a features-coverage
task rather than a bug fix (`.format()` in particular depends on the
same Format Specification Mini-Language flagged unimplemented for
f-strings at the time); implemented on explicit request, once that
mini-language existed as `Pyc_FormatValue` (see the f-string entry
above).

- **`.rsplit(sep=None, maxsplit=-1)`**: when `maxsplit < 0` (the
  default, matching real Python) it delegates straight to the existing
  `.split()` implementation, since the two produce identical results
  with no limit. The two only diverge once `maxsplit` caps the count —
  `.rsplit()` scans from the end of the string, keeping the *rightmost*
  `maxsplit+1` pieces (`"a,b,c,d".rsplit(",", 1) == ["a,b,c", "d"]`)
  where `.split()` would keep the leftmost ones. The whitespace-mode
  form (`sep=None`) needed its own from-the-right algorithm to match
  CPython's exact behavior of preserving internal whitespace runs in
  the unsplit prefix (`"  a  b  c  ".rsplit(None, 1) == ["  a  b", "c"]`
  — only the *trailing* whitespace of that prefix is trimmed, not the
  internal run between "a" and "b").
- **A related, separate, pre-existing bug found and fixed while
  verifying the above**: both `.split()` and the new `.rsplit()` only
  detected "whitespace mode" via "no argument was given at all" — an
  *explicit* `None` passed positionally (`s.split(None)`,
  `s.rsplit(None, 1)`, both valid, common Python idioms) fell through to
  the literal-separator path with the separator silently coerced to a
  plain single space, producing spurious empty-string elements for any
  run of more than one whitespace character (confirmed:
  `"a  b   c".split(None)` gave `['a', '', 'b', '', '', 'c']` instead of
  `['a', 'b', 'c']`). Fixed by detecting an explicit `None` argument from
  the AST (`Constant` node with `is_none` set), not just argument
  absence.
- **`.partition(sep)` / `.rpartition(sep)`**: return a 3-element
  `[before, sep, after]` (list, not a tuple — pyc's existing, unrelated
  "no distinct tuple type" architectural choice, not a new gap);
  `partition` finds the first occurrence of `sep`, `rpartition` the
  last. Real CPython raises `ValueError` for an empty separator; this
  takes the more lenient "no match" fallback instead (`[s, "", ""]` /
  `["", "", s]`) rather than raising, a narrower simplification.
- **`.format(*args, **kwargs)`**: a new `PyBuiltin_StrFormat` parses the
  `{field[!conv][:format_spec]}` template mini-language — `"{{"`/`"}}"`
  for literal braces, an empty field (`"{}"`) auto-numbering through the
  positional args in order, a digit-only field as an explicit positional
  index, any other field as a keyword lookup — and delegates actual
  value formatting to `Pyc_FormatValue`, the same formatter f-strings
  use. Nested field access (`"{0.attr}"`, `"{0[1]}"`) is not supported —
  a narrower, documented gap.

Verified against real CPython: `.rsplit()` with and without `maxsplit`,
both separator and whitespace modes, both forms of the `None`-detection
fix; `.partition()`/`.rpartition()` with a found and a not-found
separator; `.format()` with positional, explicit-index, keyword, and
mixed arguments, a format spec, `!r` conversion, and literal-brace
escaping. Added as permanent `tests/runner.py` regressions (indexing
into `.partition()`'s result rather than printing it raw, to avoid the
same list-vs-tuple representation difference noted above). Full suite
and import tests stay green; `valgrind --tool=memcheck` shows 0 errors.

### `obj.attr += x` Crashed at Runtime With a `KeyError` — Fixed
Continuing the hunt into class features turned up a severe, very common
bug: augmented assignment on an instance attribute (`b.n += 3`, or any
other `op=` on `obj.attr`) crashed at runtime with an uncaught
`KeyError`, even though the equivalent `b.n = b.n + 3` worked correctly.

**Root cause**: `PythonParser.cpp`'s `AugAssign` handling only special-cased
`Name` targets; every other target shape (`Subscript` *and* `Attribute`)
fell into a single shared branch tagging the node with the same
`"__subscript__"` sentinel and storing the raw target AST node as
`children[0]` — with no record of which kind of target it actually was.
This is a real divergence from the plain `Assign` node's handling just
above it in the same file, which already correctly distinguishes
`Attribute` targets (tagged `"__attr_assign__"`) from `Subscript`
targets (tagged `"__subscript__"`) — the `AugAssign` case was simply
never updated to match when that distinction was added for `Assign`.

`Compiler.cpp`'s `lowerAugAssign` then unconditionally treated anything
tagged `"__subscript__"` as a `Subscript` node — reading `children[0]`
as the container object (correct by coincidence, since `Attribute` nodes
also store their base object as `children[0]`) and `children[1]` as the
index expression to look up. But an `Attribute` node has no `children[1]`
at all (its attribute name lives in the node's `id` field, not as a
child expression) — so `idx` silently lowered to an empty string, and
the generated code called `Pyc_Subscript(instance_dict, "")`, a dict
lookup for the literal key `""`, which doesn't exist on any real
instance — hence the `KeyError`.

Fixed in both files: `PythonParser.cpp`'s `AugAssign` handling now
checks the target's type the same way `Assign` already does, tagging
`Attribute` targets `"__attr_assign__"`; `Compiler.cpp`'s
`lowerAugAssign` gained a new `"__attr_assign__"` branch that reads the
attribute name from `attrTarget->id` and does a proper `Pyc_GetItem`/
`<op>`/`Pyc_SetItem` sequence — lowering the base object expression only
once and reusing the result for both the read and the write (matching
the existing `"__subscript__"` branch's care to avoid double-evaluating
an object expression with a side effect, e.g. `get_obj().attr += 1`
must call `get_obj()` exactly once).

Verified against real CPython: multiple ops in sequence on the same
attribute (`+=`, `-=`, `*=`), a `str` attribute (`+=` concatenation, not
just numeric), a nested attribute chain (`o.inner.n += 100`), and the
double-evaluation case above (confirmed the side-effecting call happens
exactly once, matching CPython). Added as permanent `tests/runner.py`
regressions. Full suite (344/344) and import tests (9/9) stay green;
`valgrind --tool=memcheck` shows 0 errors.

### User-Defined Exception Subclasses Didn't Work At All — Fixed
Continuing the hunt into exception handling turned up a large,
foundational gap: **any** user-defined class subclassing a builtin
exception type (the completely ordinary `class MyError(Exception): pass`
idiom) was broken, in two independent ways depending on whether it was
instantiated with arguments. Originally documented here as "too large in
scope to fix in this pass"; revisited and fixed on explicit request.

**With a positional argument — hard compile crash, for the whole file**:
`raise MyError("boom")` failed LLVM module verification entirely
(`Incorrect number of arguments passed to called function!` on a call to
the synthesized `MyError__init__`) and refused to produce a binary at
all, even though nothing else in the file was wrong. Root cause: a
class with no explicit `__init__` gets a synthesized wrapper
(`lowerClass`'s "B6" logic) that looks up the parameter list of the
*nearest base class's* `__init__` via the `classInitParams` map — but
that map is only ever populated for classes **defined in the same
compilation unit** (inside the `hasOwnInitDefined` branch). `Exception`
(and every other builtin exception name) is never in it, so the lookup
failed silently and fell back to a bare `["self"]` parameter list — a
1-arg `__init__`. Separately, the call-site instantiation logic always
forwarded *every* positional argument the caller actually wrote,
regardless of what the synthesized `__init__` was declared to accept —
so a 1-param declared function got called with 2 arguments (`self` +
the message), a mismatch LLVM's verifier rejects outright.

**With no arguments — compiled, but the exception was neither caught by
name nor by any generic handler**: `raise MyError()` produced an
uncaught fatal error with a garbled dict-repr message (`Exception:
{'__class__': {'__mro__': [...]}}`) instead of being catchable by
`except MyError:` *or* `except Exception:`. Root cause: pyc has two
entirely separate exception representations that didn't know about each
other. Builtin exceptions (`ValueError("x")`, etc.) are recognized
structurally at the call site and construct a dedicated runtime-typed
"structured exception" object (`pyc_make_exc`, type tag 10) — the only
object shape `pyc_exc_type_name`/`pyc_exc_matches` (Runtime.cpp, used by
every `except` clause's type check) knew how to read. But `class
MyError(Exception): ...` is a perfectly ordinary user-defined class as
far as `lowerClass`/instantiation are concerned — it's registered in
`knownClasses` and instantiated exactly like any other class, producing
a plain `dict` (type 2) with a `"__class__"` key, which is neither a
structured exception (type 10) nor a legacy string exception (type 3) —
so `pyc_exc_type_name` fell through to a hardcoded `"Exception"` default
and `pyc_exc_matches` could never match it by its real name.

**The fix** avoids the deep redesign originally assumed necessary
("making `class MyError(Exception)` instantiate a real
structured-exception object") by instead teaching the *existing*
exception-matching machinery to also understand a plain class-instance
dict, using metadata every class already carries:

- **Construction-site fix** (`Compiler.cpp`, the `initParams.empty()`
  fallback in the class-instantiation `Call` lowering): when a class has
  no `__init__` anywhere in its base chain (the exact condition that used
  to fall into the broken bare-`["self"]` synthesis), check whether its
  compile-time-computed `classMRO` includes any name from the existing
  `builtinExcNames()` set. If so, skip `__init__` synthesis entirely —
  there's no real base `__init__` to call — and instead collect the
  constructor's positional arguments into a list and store it as
  `instance.args`, mirroring CPython's own
  `BaseException.__init__(self, *args)`. A class with its own explicit
  `__init__` (even one that calls `super().__init__(...)`) is unaffected
  by this branch; that remains a separate, narrower, still-unsupported
  case (calling into a builtin base's `__init__` via `super()` isn't
  implemented).
- **Runtime-side fix** (`Runtime.cpp`): `pyc_exc_type_name`,
  `pyc_exc_matches`, and `pyc_exc_message` each gained a `type == 2`
  branch alongside their existing `type == 10` (structured) and
  `type == 3` (legacy string) branches. A class instance's `__mro__`
  (already stored in every class dict, a compile-time-flattened list of
  ancestor class names used for `super()` support — e.g. `class
  MyError(Exception)` has `__mro__ == ["MyError", "Exception"]`) supplies
  the type name (`__mro__[0]`) and the exact ancestor chain for matching
  (walk the whole list, not just a linear parent lookup, since MRO
  already includes the builtin ancestor names); the new `args` list
  populated by the construction-site fix supplies the message
  (`args[0]`, matching CPython's single-arg `Exception.__str__`).
  `PyObject_PrintBase` gained a matching check (`pyc_instance_is_exception`,
  reusing the same `__mro__` walk) so `print(e)`/f-string formatting of
  an uncaught-and-not-yet-`__str__`-defined instance shows the message
  rather than the raw dict repr.

Verified against real CPython: raising with a message and catching by
exact name; catching via an ancestor class (`class SpecificError(MyError)`
caught by `except MyError:`); catching via generic `except Exception:`;
propagation out of a function call; multiple constructor arguments
landing in `e.args` (list-shaped, per pyc's existing, unrelated
list-vs-tuple architectural choice — not a new gap); and a non-matching
`except ValueError:` correctly falling through to the right outer
handler rather than swallowing the exception. Two pre-existing,
already-established differences remain, both applying equally to
*every* exception in pyc, not specific to this fix: uncaught tracebacks
don't include the `File "...", line N` detail CPython shows (still
true), and `type(e).__name__` on any caught exception instance printed
`None` — fixed on a later pass, see the dedicated entry further down.
Added as permanent `tests/runner.py` regressions. Full suite and import
tests stay green; `valgrind --tool=memcheck` shows 0 errors.

### `lst[-1]` — Negative List Indexing Was Broken For Every List Storage Representation
Continuing the hunt turned up what may be the highest-impact bug found
this session: negative indexing on a list — `lst[-1]` for "the last
element", one of the single most common indexing idioms in Python —
either raised a bogus `IndexError` or produced a silently wrong answer,
depending on the list's internal storage representation. Confirmed
against real CPython: `[1, 2, 3][-1]` must be `3`.

**Root cause**: pyc stores lists three ways depending on what's proven
about their contents at compile time — a homogeneous-int fast path
(`ilist`, a raw `std::vector<long>`), a homogeneous-float fast path
(`flist`), and a generic boxed fallback (`list`, `std::vector<PyObject*>`).
Every *generic* indexing path already normalizes negative indices
correctly (`Pyc_Subscript`/`Pyc_GetItem`/`PyList_GetItemObj`/
`PyList_GetItemI64`, and the equivalent str/bytes indexing) with the
standard `if (idx < 0) idx += length;` step. But `Codegen.cpp`'s A4/A7
optimization — which routes a subscript on a list *proven homogeneous
int/float at compile time* straight to a native fast-path call instead
of the boxed `Pyc_Subscript` — calls six functions
(`PyList_GetItemInt64`, `PyList_GetItemDouble`, `PyList_SetItemInt64`,
`PyList_SetItemDouble`, `PyList_SetItemInt64Auto`,
`PyList_SetItemDoubleAuto`) that all took an **unsigned** `size_t`
index with no negative-normalization step at all. A negative `i64`
index (e.g. `-1`) reinterpreted as `size_t` becomes an enormous value
(`0xFFFFFFFFFFFFFFFF`), which fails every bounds check — for the get
functions this hits the "index out of range" fallback and raises a
bogus `IndexError` even though the index is perfectly valid; for the
set functions, which don't raise, it just silently no-ops, dropping the
assignment entirely.

This means the exact failure mode depended on what pyc could prove about
the list at compile time — not something a user could predict from
their own code:
- A homogeneous-int/float list literal or one built via `list()`:
  `lst[-1]` raised `IndexError: list index out of range`; `lst[-1] = x`
  silently did nothing.
- A mixed-type (boxed) list: neither fast path applies, so indexing goes
  through the always-correct `Pyc_Subscript`/`PyList_GetItemObj` — but a
  separate, narrower compile-time constant-folding path (see below)
  still produced a wrong answer (`0` instead of the last element) in at
  least one shape.
- A list received as an untyped function parameter: also broken, since
  the fast path applies based on what's known about the list's
  *contents*, tracked independently of how it arrives at a given call site.

Fixed by adding a shared `pyc_normalize_list_index` helper (mirroring
the exact `if (index < 0) index += length;` pattern already used
everywhere else) and calling it at the top of all six functions before
any bounds check; changed their signatures from `size_t` to a signed
`long` so a negative index survives the C++ call unchanged (the LLVM
side was already declaring the parameter as a plain `i64`, which has no
signedness of its own — only the C++-side interpretation was wrong, so
no `Codegen.cpp` changes were needed).

Verified against real CPython: get and set on both int and float lists,
last/second-to-last/most-negative-valid-index cases, an out-of-range
negative index still correctly raising `IndexError`, and a list passed
through an untyped function parameter (to confirm the fix isn't merely
a special case for literal lists). Added as permanent `tests/runner.py`
regressions. Full suite (350/350) and import tests (9/9) stay green;
`valgrind --tool=memcheck` shows 0 errors.

**A separate, narrower, minor finding noticed while verifying this
fix, not previously documented**: a homogeneous list of `bool` values
(`[True, False, True]`) loses its bool-ness when read back —
`lb[0]` prints `1` instead of `True`. This is a representation-loss
issue (the int fast-path storage is a raw `std::vector<long>` with no
per-element flag distinguishing "this came from a bool literal"), not
related to the indexing bug above (reproduces with plain `lb[0]`, no
negative index involved) and much narrower in impact. Not fixed here.

### `type(x).__name__` Printed `None` For Every Type — Fixed
Originally found and documented narrowly (as `type(e).__name__` on a
caught exception instance) while working on an unrelated `del`/dict fix;
revisited and fixed on explicit request. Confirmed the gap was actually
broader than first documented: `type(x).__name__` printed `None` for
*every* type, not just caught exceptions — `type(5).__name__`,
`type("x").__name__`, etc. all printed `None` too.

**Two layered root causes, both fixed**:
- `PyBuiltin_Type` (the `type()` builtin) showed the wrong class for
  two whole categories of value: any user-defined class instance
  (`type(instance)`) and any structured/builtin exception instance
  (`type(e)` for a caught `ValueError`, etc.) both showed a generic
  placeholder (`<class 'dict'>` for the former, since a class instance
  is a plain dict with a `"__class__"` key and the dispatch switch had
  no case distinguishing that from a genuine plain dict; `<class
  'object'>` for the latter, since structured exceptions have their own
  type tag — 10 — with no case in the switch at all, falling to the
  generic default). Fixed by adding a `type == 10` case (structured
  exceptions already carry their type name directly in `obj->str`) and
  making the `type == 2` case check for a `"__class__"` entry, and if
  present, use the same `__mro__[0]` lookup already relied on for
  `super()` and for exception-instance matching (see
  `pyc_exc_instance_mro`'s comment far above — despite the
  exception-flavored name, it was already general-purpose, not
  exception-specific) to get the real class name instead of falling
  back to the generic `dict` label. A genuine plain dict (no
  `"__class__"` entry) still correctly shows `<class 'dict'>`.
- Even with `type()` itself fixed, `.__name__` on the result still
  didn't work — because pyc's `type()` returns a **formatted display
  string** (`"<class 'ValueError'>"`) rather than a real type object (a
  bigger, separate architectural gap not addressed here: pyc has no
  actual type/class object protocol, only this string formatting), and
  a plain `str` value has no attribute dict for a normal `.__name__`
  lookup to find anything in. Fixed by adding a dedicated case to
  `Pyc_GetAttr` (the centralized bare-attribute-read dispatcher added
  for the `@property` fix elsewhere in this document): when the target
  is a string matching the `"<class '...'>"` shape and the requested
  attribute is exactly `"__name__"`, parse the class name back out of
  the string instead of doing a real attribute lookup. Also strips any
  module-qualifier prefix (`"__main__."`) if present, so `.__name__`
  behaves correctly regardless of whether `type()`'s own display string
  includes one.

One remaining, deliberate simplification, not fully matching CPython:
`type()`'s formatted string for a user-defined class omits the
`__main__.` module qualifier CPython includes (`<class 'MyError'>`
instead of `<class '__main__.MyError'>`) — pyc doesn't track modules
the way CPython does. This doesn't affect `.__name__`'s correctness,
since it strips any module prefix either way.

Verified against real CPython: a builtin exception, a user-defined
exception subclass, a plain user-defined class instance (including one
reached through inheritance), a genuine plain dict (confirming it's
still correctly distinguished from a class instance), and several
non-exception builtin types (`int`, `str`, `list`, `float`). Added as
permanent `tests/runner.py` regressions. Full suite and import tests
stay green; `valgrind --tool=memcheck` shows 0 errors.

### Operator/Protocol Dunder Methods Weren't Dispatched At All — Fixed
Continuing the hunt into class features turned up a gap far broader than
the already-documented `__add__` finding above: apart from
`__init__`/`__str__`/`__repr__` (and `@classmethod`/`@property` from an
earlier pass), essentially **no** Python "special method" was ever
dispatched. Confirmed via a battery of ordinary classes: `__lt__`
(`v1 < v2` always `False`), `__sub__`/`__mul__` (always `None`),
`__neg__` (always `None`), `__len__` (`len(instance)` always reported
the instance's raw *attribute count*, not the `__len__` result —
confirmed wrong for a class with `__len__` returning `2` but 3 real
attributes), `__bool__` (silently ignored), the container protocol
(`obj[key]` for a class with `__getitem__` **crashed with an uncaught
`KeyError`**, since a class instance's own attribute dict essentially
never contains the caller's actual subscript key; `obj[key] = val` and
`key in obj` were similarly ignored), the iterator protocol (`for x in
obj:` for a class implementing `__iter__`/`__next__` silently iterated
the instance's own raw attribute dict instead — no error, just
completely wrong values), and `__call__` (calling an instance like a
function silently returned `None`).

**`__eq__` deserves special mention as the most deceptive case**: it
*appeared* to work. Both operands of `p1 == p2` are dict-backed (a class
instance is a plain dict with a `"__class__"` entry), so `==` fell
through to a generic structural dict-equality comparison — right when
two instances happen to hold identical attribute values, wrong
otherwise. Confirmed: `Point(1,2) == Point(9,9)` incorrectly evaluated
`True` with a real `__eq__` defined and completely ignored; `a == 5` for
a class whose `__eq__` always returns `True` gave `False`.

**The fix**: a single shared lookup helper, `pyc_lookup_dunder(obj,
method)` — checking the instance dict first, then the class dict via
`"__class__"` — generalized from what used to be a private,
identically-bodied helper (`GetStrOrRepr`) that only the existing
`__str__`/`__repr__` dispatch in `PyObject_Print` could see, since it
lived much further down `Runtime.cpp` than several of the functions
needing it now. Moved up to before its earliest consumer
(`PyObject_TruthValue`); `GetStrOrRepr` itself now just calls it. Two
small helpers, `pyc_call_dunder1`/`pyc_call_dunder2`, wrap the existing
`Pyc_Apply` call convention for the common "call with just `self`" /
"call with `(self, other)`" shapes. Every dispatch site below now
checks for and calls the matching dunder before falling through to its
existing builtin-type-specific logic, so a genuine plain dict, list, int,
etc. is completely unaffected (`pyc_lookup_dunder` only ever matches a
`type == 2` object that actually has a `"__class__"` entry):

- `PyObject_CompareBool` — `__eq__`/`__ne__`/`__lt__`/`__le__`/`__gt__`/
  `__ge__`. `__ne__` with no direct override falls back to `not
  __eq__(...)`, matching CPython's own default when a class defines
  `__eq__` but not `__ne__`.
- `PyNumber_Add`/`Subtract`/`Multiply`/`Divide`/`TrueDivide`/`Remainder`
  — `__add__`/`__sub__`/`__mul__`/`__floordiv__`/`__truediv__`/`__mod__`.
- `PyNumber_Negate` — `__neg__`.
- `PyBuiltin_Len` — `__len__`.
- `PyObject_TruthValue` — `__bool__`, falling back to `__len__` if
  `__bool__` isn't defined (the correct CPython precedence) before the
  pre-existing "non-empty dict" default for a class with neither.
  `PyBuiltin_Bool` (the bare `bool()` builtin) turned out to be an
  entirely separate, independent reimplementation of
  `PyObject_TruthValue`'s logic — down to duplicating its own
  already-fixed homogeneous-list bug pattern — so the new dispatch
  didn't reach it at all until `PyBuiltin_Bool` was simplified to
  delegate outright, removing the duplication instead of patching it a
  second time.
- `Pyc_Subscript` — `__getitem__`, checked before the existing
  dict-scan-and-raise `KeyError` logic.
- A **new** `Pyc_SubscriptSetItem` — `__setitem__`. Deliberately a
  separate function from the existing `Pyc_SetItem`, which is *also*
  used for plain attribute assignment (`obj.attr = val`) and various
  internal class/instance-dict setup — none of which should ever
  trigger a user-defined `__setitem__`. Only `Compiler.cpp`'s genuine
  `obj[key] = val` assignment lowering (plain assignment, the
  read-modify-write half of `obj[key] op= val`, and the
  tuple-unpacking-to-a-subscript-target case below) now calls the new
  function; attribute assignment and internal setup still call the
  original `Pyc_SetItem`, unaffected.
- `Pyc_Contains` — `__contains__`, checked before the existing
  type==2 branch (which scans the instance's own attribute *names* —
  meaningless for almost any real class).
- A **new** `pyc_materialize_iterator_protocol`, wired into
  `PyBuiltin_List` (used by both `for x in obj:` and the bare `list(obj)`
  builtin) — `__iter__`/`__next__`. This fits pyc's existing, deliberate
  "eager materialization" architecture (already used for generator
  expressions and most `itertools` functions) rather than implementing
  true lazy iteration, a much larger, separate architectural change: the
  *entire* iterator is drained up front into a real list before the
  for-loop/`list()` call ever sees it, so a `__next__` that never raises
  `StopIteration` would hang here exactly as any other already-documented
  "no lazy iterator, no infinite iterables" case would. `__next__` is
  invoked through the same `setjmp`-based try/except machinery
  `Compiler.cpp`'s generated code already uses for every other
  try/except in a compiled program (a fresh `jmp_buf` + `pyc_try_push`/
  `pyc_try_pop` per call), so a `StopIteration` raised deep inside the
  user's `__next__` body — an ordinary compiled Python function, not
  something this helper can special-case — correctly unwinds back to
  the materialization loop instead of propagating past it entirely; a
  non-`StopIteration` exception is deliberately re-raised outward,
  matching real Python's behavior when `__next__` raises something else.
- `Pyc_Apply` — `__call__`, checked in the fallback branch that used to
  unconditionally return `None` for any token that wasn't a recognized
  callable shape (string, function object, or descriptor bundle).
  Delegates to the existing `Pyc_CallMethod` (from the `@classmethod`/
  `@property` work) to bind `self` correctly, in case `__call__` is
  ever itself decorated (unusual, but not disallowed).

**Deliberate simplifications, not attempted here**: only the *left*
operand's dunder is consulted for every binary operator above — no
`__radd__`/reflected-method fallback when the left operand lacks the
primary dunder but the right operand defines the reflected one. The
bare `iter(x)` builtin itself is still entirely unimplemented (confirmed
while testing: a class whose `__iter__` does `return iter(self._data)`
rather than the common `return self` idiom yields nothing, since
`iter()` itself silently returns `None` — `pyc_materialize_iterator_protocol`
correctly falls back to an empty list rather than crashing when
`__iter__`'s return value turns out not to be a class instance with its
own `__next__`).

**A related, separate bug found and fixed while testing the above**:
`self.x, self.y = x, y` — tuple-unpacking where a target is an attribute
or subscript, not a plain name — silently did nothing at all for every
non-`Name` target. `lowerUnpackTarget` only ever handled a `"Name"` leaf
target; an `Attribute` or `Subscript` target fell through a guard that
just returned, with no error. Confirmed via the extremely common
`self.x, self.y = x, y` idiom in `__init__` (found while writing test
classes for the operator-dispatch fixes above — most natural test
classes with two-plus attributes use exactly this idiom), which left
both attributes unset (reading back as `None`), and via `d["a"], d["b"]
= 1, 2` / `a[0], a[1] = 5, 6` (subscript targets). Fixed by adding
`Attribute` and `Subscript` cases to `lowerUnpackTarget`, mirroring the
existing `Pyc_SetItem`-based logic `lowerAssign`'s `"__attr_assign__"`/
`"__subscript__"` branches already use for the non-unpacking form of the
same assignments.

Verified against real CPython across every dunder method listed above,
individually and combined (a `Vec` class exercising comparison,
arithmetic, unary negation, and `__len__` together; a `Container` class
exercising the full get/set/contains container protocol; a `Range2`
self-iterator class exercising both `for` and `list()`; a `Counter`
class exercising `__call__` with internal state mutation across
multiple calls), plus the attribute/subscript-unpacking fix on its own.
Added as permanent `tests/runner.py` regressions. Full suite (433/433)
and import tests (9/9) stay green; `valgrind --tool=memcheck` shows 0
errors.

## Known Limitations

### Performance
- **Function parameters are still boxed**: Type tracking doesn't know parameter types at lowering
- **Mixed-type code falls back to boxed runtime path**: Native paths only trigger when `resultType` is proven numeric
- **Division by zero handling requires runtime call**: Native would produce inf/nan
- **`**` (power) for non-constant exponents uses boxed `Pyc_Pow`**
- **Only a handful of stdlib modules are implemented, synthetically**: `sys`,
  `re` (PCRE2-backed), `os`, `subprocess`, `functools`, `operator`, `cmath`,
  `time.perf_counter`, `math`, `json`, `random`, `itertools` (subset),
  `collections` (subset), `datetime` (`date`/`datetime`/`timedelta`, see
  below), `pathlib` (`Path`, see below), `hashlib`, `base64`, `struct`,
  `heapq`, `bisect`, `statistics`, `string`, `textwrap`, `copy`, `uuid`,
  `shutil`, `glob`, `csv` —
  everything else reports ImportError rather than compiling real CPython
  stdlib source. A function named `get` called via `module.get()`
  collides with the dict `.get()` method shim and silently returns
  `None` — a pre-existing naming collision, not package-specific.
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
- **Dict iteration order is now insertion-order-preserving** (CPython 3.7+
  guarantee). The dict payload was changed from
  `std::unordered_map<PyObject*, PyObject*>` (hash-bucket order, never
  matching CPython) to `std::vector<std::pair<PyObject*, PyObject*>>` with
  linear-scan value-equality lookup (`PyObject_CompareBool` op==0). The
  lookup was already O(n) (the `unordered_map`'s hash was keyed by raw
  pointer, never used — the actual match was always a linear value scan),
  so this is the same algorithmic complexity with correct iteration order.
  `PyDict_SetItem` now updates an existing key's value in place (preserving
  the key's original insertion position) rather than erase-and-reappend.
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
- **Severe pre-existing use-after-free, found while adding
  `file.readlines()`: every `with open(...) as f:` corrupted memory.**
  `pyc_file_enter_adapter` (backs `__enter__` for pyc's synthetic file
  object) returned `self` without incrementing its refcount. Every other
  call result in generated code is treated as a fresh, owned reference —
  the with-statement binds `__enter__`'s return value to the target
  variable (`f`) as such, and emits a matching decref for it at `f`'s
  last use. Since the *only* refcount increment actually backing that
  "new" reference was the one already performed when `self` was stored
  into `__enter__`'s own temporary argument list (correctly balanced by
  that list's own decref immediately after the call), the object's true
  reference count was undercounted by exactly 1. Concretely: the
  with-block's own `__exit__`-argument-list cleanup (which also stores
  and then decrefs `self`) freed the object one decref too early, and
  the with-target variable's *own* later cleanup decref then ran against
  already-freed memory. Confirmed with `valgrind --tool=memcheck`
  (multiple "Invalid read/write of size 4" on a freed block before the
  fix; 0 errors after). This went undetected through every prior phase
  this session that used `with open(...)` (including the `.write()` fix
  itself, `cdbb702`) because the corruption is silent in the common
  single-open case — it only became an *observable* crash
  (`malloc(): unaligned tcache chunk detected`) once enough further heap
  activity in the same run gave glibc's allocator a chance to notice the
  damaged chunk metadata (reliably reproduced by repeating the
  open/read/close cycle 3+ times in one process, or by adding
  `readlines()`'s own extra allocations into the mix). Fixed by adding
  the missing `Py_INCREF` in `pyc_file_enter_adapter`, `Runtime.cpp`.
  **Lesson for this codebase specifically**: `valgrind`/a memory
  sanitizer would have caught this immediately — normal test-suite
  execution (matching output) is not sufficient to validate new
  `Runtime.cpp` refcounting logic, only to validate the *values*
  produced. Worth running new file/object-lifecycle-touching runtime
  code through `valgrind --tool=memcheck` before considering it verified,
  not just diffing stdout against CPython.
- **Newly discovered, pre-existing, general bug: string repr inside a
  container doesn't escape special characters.** `print(["a\nb", "c"])`
  prints a literal embedded newline instead of CPython's `['a\nb', 'c']`
  (with a visible backslash-n). Reproducible with a bare list literal —
  unrelated to `readlines()` (which is what surfaced it, since a line's
  trailing `\n` inside a `print()`ed list is an easy way to trigger it)
  or any other feature in this session. Not fixed (out of scope here);
  new permanent tests should avoid asserting on `print()` of
  newline-containing strings inside a container until this is addressed.

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
  match object, exception, function, exception class, complex, date/datetime,
  timedelta, pathlib.Path, bytes, bytearray
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
- `tests/runner.py`: inline test cases (`CASES`) + file-based regression tests (`FILE_CASES`)
- Each case compiled and compared against CPython output
- File cases: `tests/opt_*.py`, `tests/nbody.py`, `tests/fib*.py`, `tests/builtins*.py`, etc.

### Severe test-infrastructure bug found and fixed: ~20 "tests" were silently never running
Found while adding a new test case during the bytes/bytearray work (this and
`decimal.Decimal`/the `re.IGNORECASE` fix are otherwise unrelated to this
finding — it surfaced purely because a `base64`/`struct` test's output changed
and was expected to fail, but the *whole suite* kept reporting all-green
regardless). Root cause: `CASES = [...]` (Python source-string test cases)
closes with `]` at what is now line 990, immediately followed by
`FILE_CASES = [...]` (filename-based test cases) — but roughly 20 CASES-shaped
`(source_code, expected_output)` tuples had been pasted *after* that `]`,
landing inside `FILE_CASES`'s list literal instead. Python doesn't
type-check tuple shape at parse time, so this was a silent, no-error
structural bug: each stranded entry was interpreted as `(rel_path, args)`
by `main()`'s `FILE_CASES` loop — a huge multi-line Python source string
treated as a *file path*, and the expected-output string treated as
*command-line args*. `os.path.join(tests_dir, rel_path)` on a nonsense
multi-line "path" always resolves to a nonexistent file; running `python3`
and the compiled binary against a nonexistent file both fail identically
(empty stdout, non-zero exit) — so `actual == exp` compared `"" == ""`,
**always vacuously true**. Every one of these ~20 entries had been reporting
`PASS` unconditionally, regardless of what pyc actually did, for as long as
they'd been in this state — meaning none of them had provided real
regression coverage. This is a bug in the test harness, not something
introduced by any of this session's actual compiler changes.

Fixed by moving all ~20 stranded entries back into `CASES` (they now execute
for real). Re-running the suite with them genuinely active surfaced three
real, previously-hidden issues, one of which was expected and two of which
were pre-existing and unrelated to this session's work:
1. **Expected** (this session's own bytes/bytearray change): the
   hashlib/base64/struct case's hardcoded expected output was stale —
   `base64.b64encode`/`b64decode` now return real bytes (`b'...'`), not str.
   Updated.
2. **Real, pre-existing, unrelated bug — complex number arithmetic**:
   `a = 1j; b = 2j; print(a + b)` (and `-`/`*`/`/`) print `None` instead of
   a complex result — genuinely broken, not a formatting difference. Also,
   pyc's complex repr never suppresses a zero real part the way CPython's
   does (`1j` in real CPython vs. always `(0.0+1.0j)` in pyc). Since this
   source runs successfully under real CPython, the live-comparison this
   runner does always wins over any hardcoded fallback — meaning this test
   can never pass without actually fixing complex number support. **Now
   fixed**: complex dispatch with int/float promotion was added to the
   runtime `PyNumber_Add`/`Subtract`/`Multiply`/`TrueDivide` functions
   (previously complex arithmetic was only dispatched at compile time via
   the `complexVars` tracking set in `Compiler.cpp`, which only tracked
   complex literals and `complex()` calls — not plain variables holding
   complex values). The codegen native-float fast path in
   `emitNativeNumericBinary` was also fixed to not fire when
   `resultType == "boxed"` (which could be complex), preventing the
   imaginary part from being silently dropped (e.g. `-0.0 + 0j` producing
   `0.0` instead of `0j`). Complex repr was fixed to suppress zero real
   parts (`1j` not `(0.0+1.0j)`) and strip trailing `.0` from whole
   numbers in complex context (`format_double_complex` helper). Complex
   equality comparison was added to `PyObject_CompareBool` (needed for
   dict key lookup with complex keys). Test coverage re-added to `CASES`.
3. **Real, pre-existing, unrelated bug — function repr/naming**: a test
   directly compared a function object's raw `repr()` (which embeds a
   live process memory address) against live CPython's output — this can
   never match between two separate processes by construction, regardless
   of any pyc bug; not a real signal. Rewritten to check only reproducible
   properties (`repr(f).startswith("<function")`, not the literal string).
   While fixing this, also found: `callable(f)` returns `None` instead of
   `True` (the `callable()` builtin wasn't implemented at all — confirmed
   via grep, zero hits in `Compiler.cpp`). **Now fixed**: added
   `PyBuiltin_Callable` runtime function that checks for callable token
   strings (against the `g_callableRegistry`), function objects (type 11),
   exception classes (type 12), class dicts (has `__mro__`), class
   instances with `__call__`, and descriptor bundles. Wired through
   compiler dispatch and codegen extern declaration. pyc's nested-function
   repr showing its internal synthetic name (`<function __nesteddef_0 at
   ...>`) rather than CPython's qualified name (`<function
   outer.<locals>.inner at ...>`) remains unfixed (cosmetic, low-value).

**Lesson for this codebase specifically**: a passing test count is not
proof of correctness if the harness itself can silently misfile test
cases — worth periodically sanity-checking that `len(CASES)`/
`len(FILE_CASES)` match the number of list-literal entries actually
inside each list's own brackets (e.g. via `ast.parse` + inspecting
`Assign.value.elts`), not just trusting the file's visual structure.

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
