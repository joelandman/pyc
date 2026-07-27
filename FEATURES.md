# pyc — Features and Capabilities

Current test count: **300/300** (runner shows 300/300, file_case_failures=0).

## Types and Literals

| Type | Notes |
|------|-------|
| `int` | Full arithmetic, comparison, floor/true division, small int cache (-5..256) |
| `float` | `3.14`, `1e-3`, mixed int/float; shortest round-trip printing |
| `bool` | `True`/`False`; prints correctly; arithmetic with ints (`True+1=2`); singleton identity |
| `str` | Literals, `+`, `*`, f-strings, `%` formatting, all major methods, full slicing |
| `list` | Literals, subscript get/set, full slices (incl. step), comprehensions, append/sort/pop |
| `dict` | Literals, subscript get/set, keys/values/items, `get(key, default)` |
| `tuple` | Literals and unpacking (mapped to list internally) |
| `None` | Constant, comparison, printing; singleton identity |
| `complex` | Literals (`1j`, `3.5j`), arithmetic (`+ - * /`), pow, abs, `complex()` builtin |

## Operators

```
+  -  *  /  //  %  **          arithmetic (int, float, bool, str* for +/*, complex)
==  !=  <  >  <=  >=           comparison (numeric + string + chained 1<x<10)
is  is not                     identity (singleton-aware)
in  not in                     membership (list, str, dict)
and  or  not                   boolean (short-circuit, returns actual value)
-x  +x                         unary
+=  -=  *=  /=  //=  %=  **=  augmented (on names and on subscripts a[i]+=1)
```

## Control Flow

```python
if / elif / else
while ... break / continue
for x in iterable              (list, enumerate(), zip() results)
for x in range(...)            native loop shape and native i64 loop control;
                               the visible loop variable is still boxed
for i, v in enumerate(lst)     tuple-target for-loop
for (a, [b, c]) in iterable    recursive tuple/list destructuring
x if cond else y               ternary
```

## Functions

```python
def f(a, b=10, *args):         positional, default, *args, **kwargs
    return a, b                multi-value return (returned as list)

def f(a, b): ...
f(b=3, a=4)                    keyword call arguments
```

- Nested functions with `nonlocal` and cell capture (full closure support)
- `global x` declaration (shared module-level storage)
- `lambda` expressions (defaults, `*args`, as values in containers/args/returns)
- First-class functions: defs and lambdas as values with real function objects —
  `print(f)` gives `<function f at 0x...>`, identity-based `==`/`is`
- Decorators: `@deco`, `@deco(args)` factories, stacked (applied bottom-up)
- Class decorators with `__repr__` injection

## Classes

- `class` with `__init__`, instance attributes, method dispatch, class attributes
- Single and multiple inheritance with C3-linearized MRO
- `super()` following the runtime C3 MRO (full remaining-MRO method search)
- `__str__` / `__repr__` protocol (used by `print`, `str`, f-strings)

## Exceptions

- `try` / `except` / `except ... as e` / `else` / `finally`
- Typed handler dispatch with the builtin exception hierarchy
  (`ArithmeticError`, `LookupError`, `OSError` parents; `Exception` catch-all)
- Tuple clauses `except (A, B)`, bare re-raise, structured exception objects
- Builtins raise at the point of error (`ZeroDivisionError`, `IndexError`,
  `KeyError`, `ValueError` from `int()`)
- `finally` runs on every exit path: fall-through, exception, `return`,
  `break` / `continue`, raise inside a handler or `else`
- Uncaught exceptions print a CPython-style traceback line to stderr, exit 1
- Exception classes as first-class values (`exc = ValueError`, `raise exc("msg")`)

## Statements

- `with` (context managers via `__enter__` / `__exit__`)
- `match` / `case` (literals, wildcard, capture, singletons, guards)
- `assert`, `del`, walrus `:=`
- `import` / `from ... import` / `from ... import *` (file-based modules)

## Builtins

`print(*args, sep=, end=)`, `range(n)` / `range(s,e)` / `range(s,e,step)`,
`len(x)`, `str(x)`, `int(x)` / `int(x, base)`, `float(x)`, `complex(x)` / `complex(x, y)`,
`abs(x)`, `min(a,b,...)` / `min(list)`, `max(a,b,...)` / `max(list)`,
`list(x)`, `enumerate(iterable)`, `zip(a, b)`,
`sum(x)`, `sorted(x)` / `sorted(x, key=)`, `any(x)`, `all(x)`, `isinstance(obj, info)`,
`bool(x)`, `type(x)`, `id(x)`, `repr(x)`, `hex(x)`, `oct(x)`, `bin(x)`,
`ord(c)`, `chr(i)`, `round(x)`, `divmod(a, b)`, `pow(base, exp)`,
`reversed(x)`, `cmp_to_key(cmp)`

## Standard Library Stubs

- `os.path.exists()`, `os.path.isfile()`, `os.path.isdir()`, `os.path.join()`,
  `os.path.basename()`, `os.path.dirname()`, `os.path.splitext()` (returns a
  2-element list, not a tuple — see the tuple-type gap below),
  `os.path.abspath()` — real POSIX implementations
- `os.unlink()` / `os.remove()` — deletes files
- `os.rename()`, `os.getcwd()`, `os.listdir()` (entry order isn't guaranteed
  to match CPython's, matching real `readdir(3)` order), `os.makedirs()`
  (always behaves as `exist_ok=True`, i.e. `mkdir -p` — keyword arguments
  aren't read by synthetic-module functions, same limitation as
  `time.perf_counter`'s siblings)
- `os.environ` — a real dict populated from the process environment at
  import time (read-only snapshot; writes to it don't propagate back to
  the actual process environment)
- `subprocess.call()` — executes commands via fork/exec/pipe
- `subprocess.check_output()` — captures command output
- `sys` module (argv, stderr)
- `cmath` module: `sqrt`, `log`, `exp`, `sin`, `cos`, `tan`

## String Methods

`upper()`, `lower()`, `strip()`, `split(sep)`, `join(iterable)`,
`find()`, `count()`, `replace()`,
`str % value` (`%d`, `%s`, `%f`, `%.Nf`, `%x`, `%X`, `%o`, `%r`, `%%`, `%*d`)

## List/Dict Methods

`list.append(x)`, `list.sort()`, `list.pop()`  
`dict.keys()`, `dict.values()`, `dict.items()`, `dict.get(key, default)`

## Comprehensions

```python
[expr for x in iterable]
[expr for x in iterable if cond]
[[inner for ...] for ...]      nested
{ k: v for x in iterable if cond }
{ k: v for x in a for y in b }  product / nested generators
(genexpr for x in iterable)     generator expressions (eager materialization)
```

## Generator Expressions

- `(x*2 for x in range(5))` — eager materialization via thread-local buffer
- Works with `list()`, `for` loops, `join()`

## Complex Numbers

- Literals: `1j`, `3j`, `2.5j`
- Arithmetic: `+ - * /` (via `PyComplex_Add/Sub/Mul/Div`)
- Power: `**` (via `PyComplex_Pow`)
- Absolute value: `abs()` (via `PyComplex_Abs`)
- Builtin: `complex()`, `complex(3)`, `complex(3, 4)`, `complex("3+4j")`
- cmath module: `sqrt`, `log`, `exp`, `sin`, `cos`, `tan`

## Math

- `math` module: `sqrt`, `floor`, `ceil`, `trunc`, `pow`, `log` (natural or
  with a base), `log2`, `log10`, `exp`, `sin`/`cos`/`tan`,
  `asin`/`acos`/`atan`/`atan2`, `hypot`, `fabs`, `fmod`, `degrees`,
  `radians`, `isnan`/`isinf`/`isfinite`, `gcd`, `factorial`, and the
  constants `pi`, `e`, `tau`, `inf`, `nan` — wraps libm, works via
  `import math`, `from math import ...` (including `*`), and aliasing

## JSON

- `json` module: `dumps(obj)` / `loads(s)` — operates on the generic boxed
  value tree (dict/list/str/int/float/bool/None), no new types. Supports
  nested structures, string escaping, and JSON `null`/`true`/`false`. No
  `indent`/`sort_keys`/custom-encoder keyword arguments. Multi-key dict
  `dumps()` output is not guaranteed to match CPython's key order (see the
  dict-ordering limitation in IMPLEMENTATION.md)

## Random

- `random` module: `seed(n)`, `random()`, `randrange(stop)` /
  `randrange(start, stop)`, `randint(a, b)`, `uniform(a, b)`, `choice(seq)`,
  `shuffle(list)` — a from-scratch MT19937 generator replicating CPython's
  `_randommodule.c` bit-for-bit (same state, tempering, and integer-seeding
  algorithm), so `random.seed(n)` followed by any of these produces output
  identical to real CPython for the same `n`. One process-global generator
  instance, matching CPython's shared default `Random()` instance.

## Itertools / Collections (subset)

- `itertools` module: `chain(*iterables)`, `product(*iterables)`,
  `combinations(iterable, r)`, `permutations(iterable, r=None)`,
  `starmap(fn, iterable)`, `islice(iterable, stop)` (bounded 2-arg form
  only), `zip_longest(*iterables)` — all eager, returning a real new list.
  `count`/`cycle`/unbounded `repeat` are **not implemented**: pyc has no
  lazy iterator/`__next__`/`StopIteration` protocol (generator expressions
  are already eagerly materialized), so infinite iterators can't be
  represented at all — a hard architectural limit, not a scoping choice.
- `collections` module: `Counter(iterable)` returns a plain real dict
  pre-populated with counts (not a custom class instance — see
  IMPLEMENTATION.md for why). `most_common(counter, n=None)` is a plain
  function, **not** `counter.most_common(n)` method syntax. `defaultdict`,
  `namedtuple`, and `deque` are not implemented.
- Tuples aren't a distinct pyc type (pre-existing, unrelated limitation —
  `(1, 2, 3)` prints as `[1, 2, 3]` and `type()` reports `list`), so
  itertools results that would be tuples in real Python (`product`,
  `combinations`, `permutations`, `zip_longest` entries) are plain lists.

## Datetime

- `datetime` module: `date(year, month, day)`, `datetime(year, month, day,
  hour=0, minute=0, second=0)`, `timedelta(days=0, seconds=0, minutes=0,
  hours=0, weeks=0)` — two new runtime types (tags 14/15, see
  IMPLEMENTATION.md), not eagerly-materialized generic values like most
  other stdlib stubs. Works via `import datetime` (both
  `datetime.date(...)` and `datetime.date.today()` qualified forms, and
  `import datetime as dt`), and via `from datetime import date, datetime,
  timedelta` (bare names, including `as` aliasing) — both import styles
  construct through the same code path.
- Attributes: `.year`/`.month`/`.day` (date and datetime),
  `.hour`/`.minute`/`.second` (datetime only), `.days`/`.seconds` (note:
  **not** `.microseconds` — always 0, no sub-second precision) on
  timedelta.
- Arithmetic: `date/datetime + timedelta`, `date/datetime - timedelta` →
  same type; `date - date` / `datetime - datetime` → `timedelta`;
  `timedelta +/- timedelta`; `timedelta * int` (either operand order).
- Comparisons: `==`, `!=`, `<`, `>`, `<=`, `>=` between two values of the
  same type.
- `str()`/`print()`: `date` → `"YYYY-MM-DD"`; `datetime` →
  `"YYYY-MM-DD HH:MM:SS"` (space separator, matching CPython's `str()`);
  `timedelta` → CPython's own text form (`"H:MM:SS"` or `"N day(s),
  H:MM:SS"`).
- Methods: `.isoformat()` (datetime uses `"T"` as the date/time separator,
  differing from `str()`'s space), `.weekday()`, `.isoweekday()`,
  `.total_seconds()` (returns float; see the float-formatting caveat
  below).
- `datetime.date.today()` / `datetime.datetime.now()` — real wall-clock
  reads (`time()`/`localtime_r()`), not seeded/fixed like `random`, so not
  suitable for exact-match testing against a fixed CPython run.
- **Method calls require the compiler to statically see the value's type**
  (construction, a plain assignment, or a function's return value — all
  confirmed to propagate) — the same known limitation class as
  `Match.group()`. This does **not** hold for a value received as a plain
  function parameter: `def f(d): return d.isoformat()` returns `None`
  instead of raising or working, because pyc's `typeOf` type-tracking
  doesn't flow through parameters at all (see IMPLEMENTATION.md). By
  contrast, attribute reads (`.year`), arithmetic (`+`/`-`/`*`),
  comparisons, and `str()`/`print()` are all robust to this and work
  correctly even on an untyped parameter, because they route through
  runtime-tag dispatch (`Pyc_GetItem`, `PyNumber_Add`/etc.,
  `PyObject_CompareBool`, `PyObject_PrintBase`) rather than compile-time
  type inference.
- `type()` on a datetime value correctly reports `<class 'datetime.date'>`
  / `<class 'datetime.datetime'>` / `<class 'datetime.timedelta'>`.
- **Newly discovered, pre-existing, unrelated bug**: pyc's float formatter
  prints any whole-valued float whose integer part is evenly divisible by
  10 in scientific notation instead of matching CPython's fixed-point
  form — e.g. `print(20.0)` → `"2e+01"` instead of `"20.0"`; `print(100.0)`
  → `"1e+02"`. This is not specific to datetime (a bare `print(20.0)`
  reproduces it) but surfaces easily via `timedelta.total_seconds()`,
  since most human-chosen durations round evenly. See IMPLEMENTATION.md.

## Pathlib

- `pathlib.Path(path)` — a new runtime type (tag 16, see IMPLEMENTATION.md)
  that stores its text directly in the existing `PyObject.str` field
  (simpler than datetime's heap-allocated struct — a `Path` really is just
  a string with different dispatch). Works via `import pathlib`
  (`pathlib.Path(...)`, and `import pathlib as X`), and via
  `from pathlib import Path` (bare name, including `as` aliasing) — both
  forms construct through the same code path. Single-argument
  construction only (real `Path("a", "b")` multi-segment joining isn't
  supported — use `/` or `.joinpath()` instead).
- `/` (path joining): `Path / (str or Path)` → new `Path`, matching
  CPython's `PurePath.__truediv__`. Robust to untyped function parameters
  (routes through `PyNumber_TrueDivide`, gated on the runtime type tag).
- Attributes (robust — via `Pyc_GetItem`): `.name`, `.parent`, `.suffix`,
  `.stem`. Not implemented: `.parts`, `.anchor`, `.drive` (no tuple type,
  low value for an AOT-compiled subset).
- Comparisons (`==`, `!=`, `<`, etc.) and `str()`/`print()` compare/print
  the underlying path text — both robust to untyped parameters. Printing a
  `Path` nested inside a list/dict shows `PosixPath('...')` (matching
  CPython's repr); a top-level `print()`/`str()` shows the raw path text
  with no wrapper, also matching CPython.
- Methods (typeOf-gated — same param-passing limitation as datetime's
  methods, see below): `.exists()`, `.is_file()`, `.is_dir()`,
  `.joinpath(*parts)`, `.mkdir(parents=..., exist_ok=...)` (keyword
  arguments aren't read; always behaves as `parents=True, exist_ok=True`,
  same simplification as `os.makedirs`).
- `type()` on a `Path` value is not specialized (reports `<class
  'object'>` rather than `<class 'pathlib.PosixPath'>`) — lower priority
  than datetime's `type()` support since `isinstance`/`type()` checks on
  paths are rare in practice; not implemented.
- **Chained `/`/arithmetic results are now typeOf-tracked, fixing a gap
  found while building this**: `Compiler.cpp`'s `lowerBinOp` previously
  never tagged a binop's *result* with a special type string, so
  `(Path(x) / "y").is_dir()` — extremely common for `Path`, since `/`
  chaining is the primary way to build nested paths — fell through to the
  untyped-parameter-style `None` result despite the value being fully
  known at compile time (only a longer explicit-variable chain like
  `p = Path(x) / "y"; p.is_dir()` worked, since plain assignment already
  propagated typeOf). This same gap silently affected datetime too
  (`(d + delta).isoformat()`), just never surfaced there because the
  datetime test suite happened to always assign arithmetic results to a
  variable before calling a method. Fixed for both `pathlib` and
  `datetime`/`timedelta` arithmetic result temps in the same `lowerBinOp`
  change.

## Hashlib / Base64 / Struct

- `hashlib.md5(data)` / `.sha1(data)` / `.sha256(data)` — digests computed
  immediately at construction (no `.update()` streaming; data is always
  fully known upfront in practice). `.hexdigest()` returns the lowercase
  hex string. MD5/SHA-1/SHA-256 are implemented from scratch (standard
  algorithms, no OpenSSL/libcrypto dependency — matches `random`'s
  from-scratch MT19937 precedent), verified byte-for-byte against real
  `hashlib` output. Works via `import hashlib` and
  `from hashlib import md5, sha1, sha256` (including aliasing). No
  `.digest()` (raw bytes — see below), `.update()`, or `.copy()`.
- `base64.b64encode(s)` / `base64.b64decode(s)` — standard RFC 4648
  alphabet, implemented from scratch, operating directly on/returning
  `str` (no `.encode()`/`.decode()` step first, since pyc doesn't have
  those either — real CPython's `base64.b64encode()` requires an actual
  `bytes` object and raises `TypeError` on a plain `str`, so pyc's
  signature is deliberately more permissive here, not identical).
- `struct.pack(fmt, *values)` / `struct.unpack(fmt, data)` — common format
  codes (`b`/`B`/`h`/`H`/`i`/`I`/`l`/`L`/`q`/`Q`/`f`/`d`/`s`) and
  endianness prefixes (`<`/`>`/`!`/`=`, all treated explicitly —
  `=`/no-prefix defaults to little-endian, matching every platform pyc
  targets). Unsupported codes (`n`/`N`, native alignment padding) are
  silently skipped rather than erroring. `struct.unpack` returns a plain
  list, not a tuple (same documented gap as `os.path.splitext`/itertools).
- **No `bytes` type**: all three modules represent binary data as a plain
  `str` (type 3) whose characters hold byte values 0-255. Correct for
  ASCII/text content (the overwhelming common case — hashing strings,
  base64-encoding tokens, packing/unpacking numeric fields) but does
  **not** match CPython's `bytes` object identity, repr (`b'...'`), or
  encoding semantics for arbitrary non-ASCII/binary content. This is a
  deliberate, permanent scoping decision (user-confirmed), not a bug to
  fix quietly later — see IMPLEMENTATION.md.
- **Newly discovered, fixed while adding this**: `PyUnicode_FromString`
  (the runtime's primary string constructor, used almost everywhere)
  takes a `const char*` and relies on `strlen()`/implicit-length
  construction, so any content with an embedded `0x00` byte gets silently
  **truncated** — invisible for ordinary text, but `struct.pack`'s output
  routinely contains embedded NULs (e.g. any little-endian integer field
  with a zero high byte: `struct.pack("<i", 1000)` is `E8 03 00 00`).
  Fixed by adding a length-explicit `PyUnicode_FromStringAndSize`
  constructor and using it for `struct.pack`'s and `base64.b64decode`'s
  return values specifically (the two places in this session's new code
  that can produce arbitrary embedded-NUL byte content) — see
  IMPLEMENTATION.md.

## Heapq / Bisect / Statistics

- `heapq`: `heapify(list)`, `heappush(list, item)`, `heappop(list)`,
  `heappushpop(list, item)`, `heapreplace(list, item)`, `nlargest(n,
  iterable)`, `nsmallest(n, iterable)` — standard binary-heap operations,
  in-place on a plain list, verified against real `heapq` output.
- `bisect`: `bisect_left(list, x)`, `bisect_right(list, x)` (alias
  `bisect`), `insort_left(list, x)`, `insort_right(list, x)` (alias
  `insort`).
- `statistics`: `mean`, `median`, `median_low`, `median_high`, `mode`,
  `stdev`, `variance`, `pstdev`, `pvariance`. `mean`/`variance`/
  `pvariance` preserve CPython's exact-integer results for all-int input
  that divides evenly (`statistics.mean([2, 4]) == 3`, an `int`, not
  `3.0`; `statistics.pvariance([1,2,3,4,5]) == 2`, also `int`) — a
  targeted replication of CPython's Fraction-based arithmetic for the
  common case, not a full Fraction implementation, so an input whose
  *exact rational* result reduces to an integer despite a non-integer
  intermediate mean won't match (rare in practice). `stdev`/`pstdev`
  always return `float` (matches CPython — confirmed even a perfect-square
  variance still prints as `float` from `stdev`/`pstdev`, unlike
  `variance`/`pvariance` themselves).
- **Fixed while adding these**: real, previously-undiscovered pre-existing
  bugs — `h = [5, 1, 8, 3, 9, 2]; h.sort()` (and `.insert()`, `.remove()`,
  `.index()`, `.count()`, `.reverse()`, `.extend()`, `.copy()`) were all
  silent no-ops/wrong-results on a homogeneous int/float list literal.
  Homogeneous list literals use a native fast-path storage (not the
  generic boxed representation, for performance), which none of those
  eight methods accounted for (`.append()`/`.pop()`/`.clear()` were
  already correct). Fixed via one shared conversion helper, applied to
  all eight plus every new heapq/bisect/statistics function (which share
  the identical requirement) — see IMPLEMENTATION.md.

## String / Textwrap / Copy / Uuid

- `string`: pure constants matching CPython exactly — `ascii_lowercase`,
  `ascii_uppercase`, `ascii_letters`, `digits`, `hexdigits`, `octdigits`,
  `punctuation`, `whitespace`, `printable`.
- `textwrap.wrap(text, width=70)` / `textwrap.fill(text, width=70)` —
  standard greedy word-wrap (split on whitespace, pack words onto a line
  up to `width`), verified against real `textwrap` output for ordinary
  prose. Doesn't replicate CPython's long-word-breaking/hyphenation or
  `indent`/other keyword parameters.
- `copy.copy(x)` (shallow) / `copy.deepcopy(x)` (recursive) — works on
  `list`/`dict`/any other value (the latter returned as-is, matching
  Python's immutable-type optimization). No cycle detection — a
  self-referencing structure passed to `deepcopy` recurses until stack
  overflow (documented, same scoping precedent as itertools' unbounded
  iterators). Both forms robust to untyped function parameters (direct
  runtime-tag dispatch, no `typeOf` dependency — same "robust primitive"
  category as `str()`/attribute access elsewhere in this doc).
- `uuid.uuid4()` — real OS entropy (`std::random_device`), **not** the
  seeded `random` module generator: this is the *correct* match to real
  Python (`uuid4()` is unseedable in CPython too, unlike `random`'s
  functions), not a limitation. Excluded from exact-value testing the
  same way `datetime.now()`/`time.perf_counter()` are — verified
  structurally instead (length, dash placement, version nibble). Returns
  `str`; no `uuid.UUID` type (`.hex`/`.bytes`/`.int` etc. — out of scope,
  `str(uuid.uuid4())` covers the near-totality of real usage).
- **`copy.copy`/`copy.deepcopy` needed AST-structural recognition, not
  the usual token+registry dispatch**, because the `copy` module's own
  dict is itself `typeOf`-tagged `"dict"`, making it indistinguishable
  at the generic dispatch point from any real dict a user might call
  `.copy()` on — the same collision class as `os.path.join`/`os.remove`
  found in Phase 1, but not fixable the same way (`typeOf(obj)!="dict"`
  doesn't help when the receiver genuinely *is* a `"dict"`). See
  IMPLEMENTATION.md.

## Functools / Operator

- `functools.reduce(func, iterable, initializer=None)` — standard
  left-fold, calling `func` via the existing generic callable-apply
  primitive (the same one `sorted(key=...)` already uses internally).
- `functools.partial(func, *args)` — returns a real, robust partial
  application: `functools.partial(operator.add, 5)(10) == 15`. Works
  through function parameters, stored in variables/lists, etc. — it's
  implemented as a plain "descriptor bundle" (the mechanism closures
  already use internally: a list `[func, arg0, arg1, ...]`; calling it
  prepends the captured args to the caller's own), not a special type.
- `functools.wraps(original)` — a **true no-op** decorator: applying it
  to a wrapper function returns the wrapper unchanged. `@functools.wraps`
  compiles and runs correctly, but doesn't copy `__name__`/`__doc__`
  (pyc functions don't carry `__doc__` at all; low value for the
  implementation cost — documented simplification).
- `functools.lru_cache` — supports both the bare `@functools.lru_cache`
  and parenthesized `@functools.lru_cache(maxsize=...)` forms.
  **Unbounded cache only** — `maxsize` is accepted but not enforced
  (no eviction), documented gap matching `os.makedirs`'s ignored
  `exist_ok` for the same "keyword args aren't read by synthetic
  functions" reason.
- `operator.add/sub/mul/truediv/mod/eq/ne/lt/gt/le/ge/not_/neg` — thin
  wrappers over the existing arithmetic/comparison runtime primitives.
- `operator.itemgetter(key, ...)` / `operator.attrgetter(name, ...)` —
  also descriptor bundles, so they work directly as
  `sorted(items, key=operator.itemgetter("x"))` (the primary real-world
  use) and through function parameters/variables like `partial`. Support
  multiple keys/names (`itemgetter(0, 2)`), returning a list of results
  for the multi-key case (real `operator` returns a tuple — no tuple
  type in pyc, same documented gap as elsewhere).
- **Two real, previously-undiscovered compiler bugs found and fixed
  while building this** (both in `Compiler.cpp`, not specific to
  functools/operator — any code hitting the same shapes would have
  been affected): (1) a value returned from the generic
  dict-dispatch method-call path (used by every synthetic module
  function call like `os.path.exists(...)`) was never marked as
  "may hold a callable token", so assigning it to a variable and later
  calling that variable — `x = functools.partial(...); x(10)` — could
  miscompile into a direct call to an unrelated function instead of
  dispatching through `x`'s actual value. (2) A single shared
  "last lambda defined" compiler flag, used to let `f = lambda: ...; f()`
  resolve as a fast direct call, leaked across unrelated statements
  when a lambda was used as *another* call's argument
  (`functools.reduce(lambda a, b: a+b, ...)`) instead of a direct
  assignment RHS — the next, completely unrelated assignment
  (`add5 = functools.partial(...)`) could pick up the stale flag and
  alias itself directly to that earlier lambda, crashing at LLVM
  verification with an argument-count mismatch when later called with a
  different arity. See IMPLEMENTATION.md.

## Assignment Forms

```python
x = 1          # simple
a = b = 5      # multi-target
a, b = 1, 2    # tuple unpack
a[i] = v       # subscript
d[k] = v       # dict subscript
```

## Import System

- `import X` / `import X.Y.Z` — resolves same-directory modules and packages
  (recursively discovering every intermediate package level); binds the
  top-level name. `import X.Y.Z as w` binds `w` directly to the deepest
  submodule
- `from X import Y` / `from X.Y import Z` — loads the module or package,
  extracts `Y`/`Z` (an attribute or a submodule), binds to name
- `from X import *` — exports all non-underscore top-level names as module
  globals, for real compiled modules and for synthetic/built-in modules
  alike (each synthetic module declares its own exported-name list)
- `import X as Y` / `from X import Y as Z` — binds to the given alias
- Relative imports (`from . import x`, `from .. import y`, `from .rel import z`)
  — resolved against the importing module's own package; valid anywhere
  except a directly-executed main script (matches CPython, which also
  rejects relative imports in `__main__`)
- Namespace packages (PEP 420 — a directory with no `__init__.py`) are supported
- Packages: nested packages, transitive discovery of a package's own imports
  (absolute or relative), and submodules are wired onto their parent
  package as attributes on load (`import pkg.mod` makes `pkg.mod`
  attribute-accessible after importing just the leaf)
- A module's top-level code runs at most once per process — imports are
  cached via a `sys.modules`-backed registry
- External/stdlib modules pyc doesn't implement natively report a clear
  ImportError rather than attempting to compile real CPython stdlib
  source (see the synthetic modules listed above: `sys`, `re`, `os`,
  `subprocess`, `functools`, `cmath`, `time.perf_counter`)
- Cross-module globals via a `__module__<name>` function returning the
  module's dict

## Optimization Status

- `range(...)` for-loops lowered directly to loop blocks (no boxed range list)
- Hidden range loop counters use native i64 compare/increment
- Proven numeric `+`, `-`, `*`, `//`, `%` use native LLVM integer/double arithmetic
- Homogeneous numeric lists with native `int64`/`double` element storage
- Allocation sinking for numeric locals (native i64 alloca)
- Specialized function variants from proven call-site types
- Conservative type tracking with loop back-edge widening

## Runtime Architecture

`PyObject` is a flat struct: `{refcount, type, value(i64), dvalue(double),
list, dict, str}`. Type codes: 0=int, 1=list, 2=dict, 3=str, 4=float, 5=bool,
6=cell, 10=exception, 11=function, 12=exception class, 13=complex.

Most values flow as boxed `PyObject*` through LLVM IR. Arithmetic dispatches
at runtime via `PyNumber_Add` etc. Comparisons via `PyObject_CompareBool`.
Truthiness via `PyObject_TruthBoxed`. Global variables use LLVM `GlobalVariable`
with `InternalLinkage`.

IR instructions carry conservative result type metadata (`int`, `float`, `bool`,
`str`, `boxed`). Codegen uses this for native paths (range loop counters, numeric
arithmetic) with boxed fallback for uncertain cases.
