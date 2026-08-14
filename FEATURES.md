# pyc — Features and Capabilities

What compiles today. Open gaps live in [ISSUES.md](ISSUES.md). Design history lives in [IMPLEMENTATION.md](IMPLEMENTATION.md). When this file disagrees with `tests/runner.py` or the `pyc` binary, **trust the executable**.

Test inventory (see `tests/runner.py` and `test/import_tests/`): ~637 inline `CASES` + 29 `FILE_CASES` + 1 dispatch-chain check, compiled at `-O0` and compared to CPython; plus a 9-case import suite. `make check` runs the runner, the import suite, and a thin `-O2` smoke. Counts in older docs (300, 499, 557, 627, 632) are stale.

---

## Types and Literals

| Type | Tag | Notes |
|------|-----|-------|
| `int` | 0 | Arithmetic, comparison, floor/true division, small-int cache |
| `list` | 1 | Literals, subscript (incl. negative), slices, comprehensions, methods |
| `dict` | 2 | Insertion-ordered (CPython 3.7+). Subscript, keys/values/items, `get` |
| `str` | 3 | Literals, `+`/`*`, f-strings, `%`, `.format()`, major methods, slicing |
| `float` | 4 | Literals, mixed int/float. Whole values divisible by 10 print as `20.0`, not scientific |
| `bool` / `None` | 5 | Same tag. `True`/`False`/`None` singletons |
| `cell` | 6 | Closures / `nonlocal` |
| `tuple` | 7 | Distinct type (also used internally for `super` proxies — see ISSUES) |
| compiled regex / match | 8 / 9 | PCRE2. Boxes allocated with `new PyObject()` (I-004) |
| exception instance | 10 | |
| function | 11 | Identity + repr; `__name__` / `__doc__` / `__call__` |
| exception class | 12 | |
| `complex` | 13 | Literals, arithmetic, pow, abs, `complex()` |
| `date` / `datetime` | 14 | No microseconds, no tz, not a `date` subclass |
| `timedelta` | 15 | `.microseconds` always 0 |
| `pathlib.Path` | 16 | Single-arg construction |
| `bytes` | 17 | Literals, index → int, slice, `+`, `.hex()`/`.decode()` |
| `bytearray` | 18 | Mutable; `.append()`/`.extend()`/item assign |
| `decimal.Decimal` | 19 | libmpdec, 28 digits, `ROUND_HALF_EVEN`. No `getcontext()` |
| `set` | 20 | Insertion-ordered, dedup-by-value |

---

## Operators

```
+  -  *  /  //  %  **          arithmetic (int, float, bool, str, complex, tuple, list)
==  !=  <  >  <=  >=           comparison (numeric, string, sequence; chained)
is  is not                     identity
in  not in                     membership (list, str, dict, set, tuple)
and  or  not                   short-circuit, returns the actual value
-x  +x                         unary
+=  -=  *=  /=  //=  %=  **=  names, subscripts, and attributes
```

User-class dunders: `__eq__`/`__ne__`/`__lt__`/`__le__`/`__gt__`/`__ge__`,
`__add__`/`__sub__`/`__mul__`/`__floordiv__`/`__truediv__`/`__mod__`, `__neg__`,
`__len__`, `__bool__`, `__getitem__`/`__setitem__`/`__contains__`,
`__iter__`/`__next__` (eagerly materialized), `__call__`. Only the **left**
operand's dunder is consulted (no `__radd__`).

---

## Control Flow

```python
if / elif / else
while ... break / continue
for x in iterable              # list, tuple, set, dict, str, enumerate/zip results
for x in range(...)            # native i64 loop control; visible variable still boxed
for i, v in enumerate(lst)     # tuple-target for-loop
for (a, [b, c]) in iterable    # recursive destructuring
x if cond else y
```

Boxed non-numeric truthiness (`if some_str:`, `if lst:`) uses `PyObject_TruthValue`.

---

## Functions

```python
def f(a, b=10, *args, **kwargs):
    return a, b                # multi-value return is a real tuple
f(b=3, a=4)
f(**{"a": 1, "b": 2})          # unmatched keys go to **kwargs
```

- Nested functions, `nonlocal` (cells), `global`
- Lambdas (defaults, `*args`) as values; indirect calls via `Pyc_Apply`
- Decorators: `@deco`, `@deco(args)` factories, stacked
- `@classmethod` / `@property` / `@staticmethod` on methods
- First-class functions: identity, `print(f)` → `<function f at 0x...>`
- Missing required positional arguments raise `TypeError` with CPython's message (`f() missing 1 required positional argument: 'a'`), including dynamic `*args` ([I-023](ISSUES.md)) and aliased `g(**{})` ([I-024](ISSUES.md)). Nested/lambda names use CPython qualname ([I-025](ISSUES.md)). Method default slots use default-index, not param index ([I-033](ISSUES.md)).

---

## Classes

- `class` with `__init__`, instance/class attributes, methods
- Single and multiple inheritance, C3 MRO, `super()`
- `__str__` / `__repr__`
- Instantiation via a variable or container (`X = Foo; X()`, `registry["foo"](7)`)
- Unpacking onto attributes: `self.x, self.y = x, y`
- User methods named `call` / `exists` / `bit_length` / `fromkeys` / `unlink` / `isfile` / `isdir` / `check_output` are no longer stolen by name-only builtin/module arms ([I-006](ISSUES.md)). `C().get` / `os.get` are no longer dict `.get()` ([I-007](ISSUES.md)). Leftover boxed-accepting arms (`is_file`, `isoformat`, …): [I-030](ISSUES.md). Alias/`m = os` leftovers: [I-032](ISSUES.md).
- `super().__init__` into a builtin exception base stores `self.args` ([I-008](ISSUES.md)). Other builtin bases (`list`/`dict`) and `super().__str__` remain ([I-038](ISSUES.md)).

---

## Exceptions

- `try` / `except` / `except … as e` / `else` / `finally`
- Builtin hierarchy (`ArithmeticError`, `LookupError`, `OSError`, `Exception`)
- `except (A, B)`, bare re-raise, `raise ValueError("msg")`
- User subclasses: `class MyError(Exception): pass` and custom `super().__init__(m)` ([I-008](ISSUES.md))
- `finally` on fall-through, exception, `return`, `break`/`continue`
- Uncaught exceptions print a traceback to stderr and exit 1, including `File "…", line N, in <name>` frames ([I-009](ISSUES.md)). Reraise/nomatch drops callee frames ([I-036](ISSUES.md)); methods print IR names (`C__foo`, [I-037](ISSUES.md)).

---

## Statements

- `with` (`__enter__` / `__exit__`)
- `match` / `case` (literals, wildcard, capture, singletons, guards)
- `assert`, `del` (list index and dict key; missing dict key raises `KeyError`)
- Walrus `:=`
- `import` / `from … import` / `from … import *` / relative imports

---

## Builtins

`print(*args, sep=, end=)`, `range` (1/2/3-arg), `len`, `str`, `int`/`int(x, base)`,
`float`, `complex`, `abs`, `min`/`max` (multi-arg, iterable, `key=`, `default=`),
`list`, `tuple`, `set`, `enumerate` (`start=`), `zip`, `sum` (`start=`),
`sorted` / `list.sort` (`key=`, `reverse=`), `any`, `all`, `isinstance`,
`issubclass`, `bool`, `type`, `id`, `repr`, `hex`, `oct`, `bin`, `ord`, `chr`,
`round`, `divmod` (returns a tuple), `pow` / `pow(base, exp, mod)`, `reversed`,
`cmp_to_key`, `callable`, `map`, `filter`,
`getattr` / `hasattr` / `setattr` / `delattr`, `format`.

Many of these are first-class values (`sorted(words, key=len)`, `funcs = [abs, str]`).

`cmp_to_key` + `sorted(..., reverse=)` is supported.

`type(x)` returns a **display string** (`<class 'int'>`), not a type object.
`type(x).__name__` is parsed out of that string ([I-011](ISSUES.md)).

---

## Comprehensions and Generators

```python
[expr for x in iterable if cond]
[k for k, v in pairs]          # unpack targets work
{k: v for x in iterable}
{x*x for x in iterable}        # set comprehensions
(x*2 for x in range(5))        # generator expressions, eagerly materialized
```

List-comp `name[const]` is typed only when proven int/float; otherwise boxed (I-005). Nested listcomps are always lists, not scalars (I-029).

---

## String Methods

`upper`, `lower`, `strip`, `split`/`rsplit` (incl. `None` / whitespace mode and `maxsplit`),
`partition`/`rpartition` (3-tuples), `join`, `find`/`rfind`, `count`, `index`,
`replace`, `startswith`/`endswith` (incl. tuple of prefixes), `center`,
`format(*args, **kwargs)`, `str % value`.

`.format()` supports positional/index/keyword fields, format specs, `!r`/`!s`/`!a`,
and nested `{0.attr}` / `{0[k]}` ([I-010](ISSUES.md)). Nested misses raise
IndexError/KeyError/AttributeError ([I-041](ISSUES.md)). `:` / `!` inside `[…]`
are index characters ([I-042](ISSUES.md)). Base-field misses raise
IndexError/KeyError ([I-053](ISSUES.md)). `partition("")` / `rpartition("")` raise
`ValueError: empty separator`. Non-str sep is `TypeError` ([I-040](ISSUES.md)).

Container / `repr` of `str` escapes `\n`, `\t`, `\r`, `\\`, quotes, and other ASCII controls (`\xHH`), with CPython quote-switching. Bare `print("a\nb")` is still `str` (real newline). Embedded NUL in a str literal and `chr(0)` keep their length ([I-021](ISSUES.md)); bare `print` / some rebuilds still stop at the first NUL ([I-063](ISSUES.md)). `KeyError` / nested `Path` print use `repr` ([I-022](ISSUES.md)).

---

## List / Dict / Set / Tuple Methods

- **list**: `append`, `sort` (`key=`, `reverse=`), `pop`/`pop(i)`, `insert`, `remove`,
  `index`, `count`, `reverse`, `extend`, `copy`, `clear`, `+`, `del lst[i]`, `del lst[s:e]` / `del lst[::2]`.
  `del` of a tuple/str slice is `TypeError` ([I-026](ISSUES.md)); `del lst[::0]` is
  `ValueError` ([I-027](ISSUES.md)).
- **dict**: `keys`, `values` (lists), `items` (list of 2-tuples), `get`, `pop`, `update`, `copy`, `clear`
- **set**: `add`, `remove`, `discard`, `pop`, `clear`, `copy`, `update`,
  `union`/`intersection`/`difference`/`symmetric_difference`,
  `issubset`/`issuperset`; operators `|` `&` `-` `^`; subset comparisons
- **tuple**: indexing, slicing (→ tuple), `+`, `*`, `in`, `count`, `index`, unpacking

Homogeneous int/float list literals use unboxed `ilist`/`flist` storage. User-list
consumers either call `pyc_ensure_boxed_list()` or branch on `list_item_type`
(I-003 audit). `del` of a tuple/str/dict slice is still a no-op ([I-026](ISSUES.md)).

---

## File I/O

`open(path, mode)` / `with open(...) as f:` — `.write()`, `.readlines()`.
No `.read()`, `.readline()`, `.close()`, or `open(..., "rb")`.

---

## Import System

- `import X` / `import X.Y.Z` / `from X import Y` / `from X import *` / aliases
- Relative imports (`from . import x`, `from .. import y`) inside packages
- Nested packages, PEP 420 namespace packages, `sys.modules`-style cache
- Real CPython stdlib beyond the synthetic modules below → `ImportError`

Suite: `test/import_tests/run_import_tests.sh` (wired into `make check`).

---

## Synthetic Standard Library

Hand-maintained twice: `syntheticModuleExports()` in `Compiler.cpp` and the
runtime module dicts. Keep them in sync.

| Module | What works | Deliberate gaps |
|--------|------------|-----------------|
| `sys` | `argv`, `stderr`, `modules` | |
| `time` | `perf_counter` | rest of `time` |
| `re` | search/match/finditer/findall/sub/split/compile; `IGNORECASE`/`MULTILINE`/`DOTALL`; `count=`/`maxsplit=`; `.group(i)` | `VERBOSE`/`ASCII`; named groups; compiled-pattern methods |
| `os` / `os.path` | exists/isfile/isdir/join/basename/dirname/`split`/`splitext` (2-tuples)/abspath; unlink/remove/rename/getcwd/listdir/makedirs (`exist_ok` always true); `environ` (read-only snapshot) | writes to `environ` do not affect the process |
| `pathlib` | `Path`, `/` chaining, `.exists`/`.is_dir`/`.joinpath` | `PurePath`, `.parts`, `.resolve`, `.glob`, multi-arg ctor |
| `subprocess` | `call`, `check_output` | |
| `math` | ~25 libm functions + `pi`/`e`/`tau`/`inf`/`nan` | |
| `cmath` | `sqrt`, `log`, `exp`, `sin`, `cos`, `tan` | |
| `json` | `dumps`/`loads` | `indent`/`sort_keys`/custom encoder |
| `random` | MT19937 matching CPython: seed/random/randrange/randint/uniform/choice/shuffle | |
| `itertools` | chain, `chain.from_iterable`, product, combinations, permutations, starmap, islice (2-arg), zip_longest, accumulate, takewhile, dropwhile, compress, groupby | `count`/`cycle`/unbounded `repeat` (no lazy iterators) |
| `collections` | `Counter` (`.most_common`/`.elements`/`.subtract`/`.update`, `__missing__`); `deque`; `namedtuple`; `defaultdict` | `Counter` is a dict + side table, not a subclass; deque/namedtuple print as list/dict |
| `datetime` | `date`/`datetime`/`timedelta`, arithmetic, comparisons, `.isoformat`/`.weekday`/`.total_seconds` | µs, tz, `strptime`/`strftime`/`fromisoformat`; `datetime` is not a `date` subclass |
| `hashlib` | md5/sha1/sha256; `str`/`bytes`/`bytearray`; `.hexdigest()`/`.digest()` | no `.update()` (digest at construction) |
| `base64` | `b64encode`/`b64decode` → bytes | |
| `struct` | common pack/unpack codes; `unpack` → tuple | native align, `n`/`N` |
| `heapq` / `bisect` / `statistics` | standard helpers; statistics preserves exact int when all-int and division is even | full `Fraction` arithmetic |
| `string` / `textwrap` / `copy` / `uuid` | subset | |
| `functools` | `reduce`, `partial`, `wraps` (no-op on `__name__`/`__doc__`), `lru_cache` (unbounded; `maxsize` ignored) | |
| `operator` | arithmetic/comparison wrappers; `itemgetter`/`attrgetter` (multi-key → tuple) | |
| `shutil` | `copyfile`, `move`, `rmtree` | |
| `glob` | `*` `?` `[seq]` in one directory | no `**` |
| `csv` | `reader(lines)`, `writer(f).writerow` | dialects, embedded newlines in quoted fields |
| `decimal` | `Decimal` arithmetic, `.quantize`, conversions | `getcontext`/`localcontext` |

`itertools.product`/`combinations`/`permutations`/`zip_longest` entries,
`os.path.split`/`splitext`, `struct.unpack`, `enumerate`/`zip` pairs,
`dict.items`, `str.partition`/`rpartition`, `groupby` pairs, and
`Counter.most_common` entries are **real tuples**.

---

## Debug Info

`pyc hello.py -o hello -g -O0` emits DWARF line tables and `DbgDeclare` for
locals. `gdb`/`lldb` can `break`, `next`, `info locals`, `backtrace`.
A6 specialized variants are `DW_AT_artificial`. Load
[tools/pyc_gdb.py](tools/pyc_gdb.py) in gdb to pretty-print `PyObject*`
when the type is a real struct (null prints `None`; `-g -O0` locals
have a 4-field `PyObject` composite). See [DEBUGGING_PLAN.md](DEBUGGING_PLAN.md).
Optional: `-DPYC_RUNTIME_BC_DEBUG=ON` adds `-g` to `runtime.bc` ([I-044](ISSUES.md)).

---

## Optimization (landed)

A1 type tracking · A2 native `range` loops · A3 native arithmetic ·
A4 homogeneous numeric lists · A5 numeric-local sinking · A6 single-signature
specialization · A7 allocation counters · container typing · P0 structured
unpack · P1 scalar freelist · Phase 27 param/return inference.

A6 generates a native variant only when **all** call sites agree. Mixed
int/float sites stay boxed. Direct calls with a boxed `PyObject*` arg
speculate into `__specialized_*` behind a `type==0/4` tag check
([I-014](ISSUES.md) W5.8); miss stays on the boxed callee. `Pyc_Apply`
is still boxed. Native join of the variant result is leftover I-112.

Boxed-receiver methods (function parameters) use arity-specific
`Pyc_CallMethodOrBuiltin0/1/2` and a `(tag, name)` lookup
([I-015](ISSUES.md)).

`-O0` = no LTO, no LLVM passes (what the runner uses). `-O2` is the user default
(LTO of `runtime.bc` + LLVM O2). Verify both.

---

## Intentionally Unsupported

`exec()` / `eval()` · compiling real CPython stdlib · JIT · lazy compilation ·
tail-call optimization · `memoryview` / buffer protocol · `bytes %` formatting.

---

## Real Tuple Type (implementation note)

Type tag 7. Storage reuses `list`/`ilist`/`flist` with a different tag and
immutable API. Runtime: `PyTuple_New`/`SetItem`/`GetItem`/`Size`/`Concat`/`Repeat`,
`PyBuiltin_Tuple`. Compiler: `lowerList` Tuple branch, `tuple()` builtin,
`isinstance(..., tuple)` → 7.

Operations: literals (incl. `(1,)` and `()`), print/repr, index/slice, `len`,
unpack, `+`/`*`, comparisons (tuple ≠ list), `in`, `divmod` → tuple, `%` unpack.

A previous intermediate state emitted `PyTuple_*` with no runtime, breaking
any program with a tuple literal at link time. That is closed.

---

## Runtime Architecture

`PyObject` is a flat struct (`refcount`, `type`, `value`/`dvalue`/`list`/`dict`/`str`/…).
Most values flow as boxed `PyObject*`. Exceptions use setjmp/longjmp.
Indirect calls go through `Pyc_Apply` + generated `__apply__N` adapters.

Live sources are under `src/` plus `runtime/b7_import.cpp`. Root-level
`frontend/`, `ir/`, `codegen/`, and most of `test/` are legacy and not in the
main build.
