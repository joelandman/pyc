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

- `os.path.exists()`, `os.path.isfile()`, `os.path.isdir()` — real POSIX implementations
- `os.unlink()` — deletes files
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
