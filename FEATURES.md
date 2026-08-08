# pyc — Features and Capabilities

Current test count: **499/500** (runner shows 499/500, file_case_failures=0; 1 remaining CASES failure is a pre-existing nested-comp type-tracking quirk).

## Types and Literals

| Type | Notes |
|------|-------|
| `int` | Full arithmetic, comparison, floor/true division, small int cache (-5..256) |
| `float` | `3.14`, `1e-3`, mixed int/float; shortest round-trip printing |
| `bool` | `True`/`False`; prints correctly; arithmetic with ints (`True+1=2`); singleton identity |
| `str` | Literals, `+`, `*`, f-strings (incl. format specs and `!r`/`!s`/`!a`), `%` formatting, `.format()`, all major methods, full slicing |
| `list` | Literals, subscript get/set (incl. negative indices), full slices (incl. step), comprehensions, append/sort/pop |
| `dict` | Literals, subscript get/set, keys/values/items, `get(key, default)` |
| `tuple` | Literals and unpacking (mapped to list internally) |
| `None` | Constant, comparison, printing; singleton identity |
| `complex` | Literals (`1j`, `3.5j`), arithmetic (`+ - * /`), pow, abs, `complex()` builtin |
| `bytes` / `bytearray` | Literals (`b"..."`), indexing (→ int), slicing, concatenation, comparison, `.hex()`/`.decode()`/`str.encode()`; `bytearray` adds `.append()`/`.extend()`/item assignment |
| `decimal.Decimal` | Arbitrary-precision base-10 arithmetic (`+ - * / //`), comparisons, `.quantize()`, `int()`/`float()` conversion — backed by libmpdec |

## Operators

```
+  -  *  /  //  %  **          arithmetic (int, float, bool, str* for +/*, complex)
==  !=  <  >  <=  >=           comparison (numeric + string + chained 1<x<10)
is  is not                     identity (singleton-aware)
in  not in                     membership (list, str, dict)
and  or  not                   boolean (short-circuit, returns actual value)
-x  +x                         unary
+=  -=  *=  /=  //=  %=  **=  augmented (on names, subscripts a[i]+=1, and attributes obj.attr+=1)
```

- **Severe, common bug found and fixed**: augmented assignment on an
  instance attribute (`obj.attr += x`) crashed at runtime with an
  uncaught `KeyError` — the parser routed `Attribute` augmented-assign
  targets through the same code path as `Subscript` targets, which reads
  a bogus empty-string dict key for the (nonexistent) "index" of an
  attribute node. `obj.attr = obj.attr + x` was unaffected. See
  IMPLEMENTATION.md.

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

- **Severe, general, pre-existing bug found and fixed**: `if`/`while`/
  ternary conditions on a *boxed non-numeric* value (`if some_str:`, `if
  some_list:`, `if some_dict:`, `while s:`, `x if lst else y`, ...) were
  silently, unconditionally treated as **falsy**, regardless of the
  value's actual truthiness — `if "hello":` never ran its body.
  Conditions on boxed *numeric* values (int/float/bool, and anything
  producing a native `i1`/`i32` comparison result) were unaffected. Found
  while verifying `decimal.Decimal`'s truthiness (an unrelated, much
  narrower change on its own). See IMPLEMENTATION.md for the root cause
  and fix.
- **Severe, general bug found and fixed**: *assigning* a native
  comparison result to a variable (`x = 1 < 2`, chained `1 < 2 < 3`,
  comparisons of function parameters, `flag = i < 3` inside a loop —
  an entirely ordinary idiom) crashed LLVM module verification or
  silently miscompiled. Conditions used directly in `if`/`while`/ternary
  were unaffected (a different, already-fixed bug — see above). See
  IMPLEMENTATION.md for the root cause and fix.

## Functions

```python
def f(a, b=10, *args):         positional, default, *args
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
- `f(**some_dict)` at a call site — spreading a real dict into a
  callee's ordinary named parameters. **Found and fixed a segfault**
  (a missing null-terminator in what used to be a C varargs runtime
  helper). **Found and fixed two further bugs**: a key the dict omits
  now correctly falls back to that parameter's default instead of
  `None`, and mixing a positional argument with the spread dict no
  longer clobbers the positional value with `None` when the dict doesn't
  also happen to supply that same parameter's name.
- `def f(**kwargs): ...` — the `**kwargs` catch-all parameter.
  **Severe bug found and fixed**: this used to bind an empty, wrongly
  -typed list no matter what the caller passed (`f(a=1, b=2)` gave
  `kwargs == []`, not `{'a': 1, 'b': 2}`) — now correctly collects the
  caller's keyword arguments into a real dict for direct calls (a named
  function called directly), including combined with `*args` in any
  arg-count shape. **Also now fixed for indirect calls** (through a
  closure, decorator, or first-class function value, e.g. `g = f; g(a=1,
  b=2)`, or the standard `def wrapper(*args, **kwargs): return
  fn(*args, **kwargs)` decorator-forwarding pattern) — these used to
  always get an empty placeholder of the wrong type (a list, not even a
  dict) regardless of what was passed. See IMPLEMENTATION.md for all of
  the above.
- **Severe bug found and fixed**: a nested function combining `*args`
  and `**kwargs` in its signature (`def wrapper(*args, **kwargs): ...`)
  crashed compilation entirely — breaking the standard generic-decorator
  wrapper pattern, `def wrapper(*args, **kwargs): return f(*args,
  **kwargs)`, even when `kwargs` was never read. See IMPLEMENTATION.md.

## Classes

- `class` with `__init__`, instance attributes, method dispatch, class attributes
- Single and multiple inheritance with C3-linearized MRO
- `super()` following the runtime C3 MRO (full remaining-MRO method search)
- `__str__` / `__repr__` protocol (used by `print`, `str`, f-strings)
- Operator/protocol dunder methods: comparison (`__eq__`/`__ne__`/
  `__lt__`/`__le__`/`__gt__`/`__ge__`), arithmetic (`__add__`/`__sub__`/
  `__mul__`/`__floordiv__`/`__truediv__`/`__mod__`), `__neg__`,
  `__len__`, `__bool__`, the container protocol (`__getitem__`/
  `__setitem__`/`__contains__`), the iterator protocol (`__iter__`/
  `__next__`, eagerly materialized — see IMPLEMENTATION.md), and
  `__call__`. Only the left operand's dunder is consulted for binary
  operators (no `__radd__`/reflected-method fallback).
- `@classmethod`, `@property`, `@staticmethod` method decorators.
  **Severe bug found and fixed**: these used to be silently discarded
  entirely — every method was called identically regardless of
  decorator, so `@classmethod`'s `cls` was never correctly bound (only
  accidentally to the instance when called via `instance.method()`),
  `@staticmethod` with real parameters crashed/misbehaved when called
  via an instance, and `@property` getters were never invoked on plain
  attribute access at all (`a.name` returned the method's raw internal
  token instead of the computed value). Now works correctly for both
  `ClassName.method()` and `instance.method()` call shapes, including
  the "unbound method" idiom (`ClassName.method(instance, ...)`) and
  multi-level inheritance with `super()`.
  **Severe bug found and fixed**: class construction via a variable
  holding a class reference (`X = Foo; X()`; a class value pulled from a
  container, e.g. `registry["foo"](7)`; a class passed into a plain
  function, e.g. `def make(cls): return cls()`) always silently returned
  `None` instead of a new instance — class instantiation was only ever
  recognized structurally, by literal class name, at compile time. Now
  dispatches dynamically at the runtime level too, including resolving
  and calling an inherited `__init__` through the class's MRO. Surfaced
  and fixed two further, more severe pre-existing bugs along the way: a
  defaulted `__init__` parameter (`def __init__(self, n=5)`) silently
  clobbered across every class in the module sharing the same positional
  default index; and `__init__` defaults were entirely unreachable via
  any indirect call to `__init__` at all (a stored bound-method
  reference, or `super().__init__()`). See IMPLEMENTATION.md.
- **Related, more severe bug found and fixed**: `x ** N` for a small
  constant integer exponent (0–8) misrouted any function-parameter or
  other untyped operand through complex-number multiplication instead
  of ordinary multiplication — a silent wrong answer for int arguments,
  and an outright compiler crash for some float arguments. Not specific
  to methods (a plain top-level function hits it too). Fixed.
  **Also now fixed**: calling the same function with a float argument
  (`def f(y): return y ** 2; f(3.5)`) used to crash the compiler outright
  even as the only call site — see IMPLEMENTATION.md.
- **Severe bug found and fixed, much broader than first documented**:
  operator/protocol dunder methods weren't dispatched at all. Now works:
  comparison (`__eq__`/`__ne__`/`__lt__`/`__le__`/`__gt__`/`__ge__` —
  `__eq__` was the most deceptive case, since it *appeared* to work by
  sheer coincidence of a generic structural fallback, giving outright
  wrong answers for genuinely different instances), arithmetic
  (`__add__`, `__sub__`, `__mul__`, `__floordiv__`, `__truediv__`,
  `__mod__`), `__neg__`, `__len__`, `__bool__` (falling back to
  `__len__` per real Python precedence), the container protocol
  (`__getitem__`/`__setitem__`/`__contains__` — `__getitem__` didn't
  just misbehave but crashed with an uncaught `KeyError`), the iterator
  protocol (`for x in obj:` completely bypassed `__iter__`/`__next__`,
  silently iterating the instance's own raw attributes instead — now
  eagerly drains the iterator into a real list, matching pyc's existing
  "eager materialization" architecture), and `__call__` (calling an
  instance like a function silently returned `None`). Only the left
  operand's dunder is consulted for binary operators (no `__radd__`
  etc.); the bare `iter(x)` builtin itself is still unimplemented
  (`__iter__` returning something other than `self`, e.g. `return
  iter(self._data)`, silently yields nothing). See IMPLEMENTATION.md.
- **Related bug found and fixed**: `self.x, self.y = x, y` (unpacking
  where a target is an attribute or subscript, not a plain name) — the
  extremely common attribute-unpacking idiom in `__init__` — silently
  left every non-plain-name target unset, with no error. See
  IMPLEMENTATION.md.

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
- **Bug found and fixed**: `type(e).__name__` on a caught exception
  instance used to print `None` instead of the exception's class name —
  confirmed to actually be broader than originally documented,
  affecting `type(x).__name__` for *every* type, not just caught
  exceptions (`type(5).__name__` printed `None` too). `type()` itself
  also showed the wrong class for any user-defined class instance or
  builtin/structured exception (`<class 'dict'>` / `<class 'object'>`
  instead of the real name) — both now fixed. `<class 'ClassName'>`
  omits the CPython `__main__.` module qualifier for user-defined
  classes (pyc doesn't track modules the way CPython does); `.__name__`
  is unaffected by this since it strips any module prefix either way.
  See IMPLEMENTATION.md.
- **Severe bug found and fixed**: user-defined classes subclassing a
  builtin exception type (`class MyError(Exception): pass` — an
  ordinary, common idiom) didn't work at all. Raising one with an
  argument used to crash the *entire compilation* (LLVM verification
  failure); raising one with no arguments compiled but the resulting
  exception was uncatchable by name or by `except Exception:` alike.
  Now works: catch by exact name, by an ancestor class, or by generic
  `Exception`; message display; propagation out of a function call;
  multiple constructor arguments land in `e.args` (list-shaped, per
  pyc's existing list-vs-tuple choice). A class with its own explicit
  `__init__` that calls `super().__init__(...)` remains unsupported (a
  narrower, separate gap). See IMPLEMENTATION.md.

## Statements

- `with` (context managers via `__enter__` / `__exit__`)
- `match` / `case` (literals, wildcard, capture, singletons, guards)
- `assert`, `del`, walrus `:=`
- **Real bugs found and fixed**: `del list[i]` silently did nothing at
  all (for any list, not just a storage-representation edge case) —
  `Compiler.cpp` called a dict-only deletion function unconditionally,
  regardless of the target's actual type. `del dict[missing_key]` also
  silently succeeded instead of raising `KeyError`. Both fixed — see
  IMPLEMENTATION.md.
- `import` / `from ... import` / `from ... import *` (file-based modules)

## Builtins

`print(*args, sep=, end=)`, `range(n)` / `range(s,e)` / `range(s,e,step)`,
`len(x)`, `str(x)`, `int(x)` / `int(x, base)`, `float(x)`, `complex(x)` / `complex(x, y)`,
`abs(x)`, `min(a,b,...)` / `min(list)`, `max(a,b,...)` / `max(list)`,
`list(x)`, `enumerate(iterable)`, `zip(a, b)`,
`sum(x)`, `sorted(x)` / `sorted(x, key=)` / `sorted(x, reverse=)`, `any(x)`, `all(x)`, `isinstance(obj, info)`,
`bool(x)`, `type(x)`, `id(x)`, `repr(x)`, `hex(x)`, `oct(x)`, `bin(x)`,
`ord(c)`, `chr(i)`, `round(x)`, `divmod(a, b)`, `pow(base, exp)`,
`pow(base, exp, mod)` (modular exponentiation), `tuple(x)`,
`reversed(x)`, `cmp_to_key(cmp)`

- **Real bugs found and fixed**: `tuple(x)`, `divmod(a, b)`, and
  `pow(base, exp)` all unconditionally returned `None` — each had a
  correctly-implemented dispatch branch that was simply never reached
  (same root cause already found and fixed for `bytes`/`bytearray`
  earlier this session: missing from an internal whitelist that decides
  how bare-name calls are compiled). `tuple(x)` behaves like `list(x)`
  (no distinct tuple type — see the Types table above); `divmod()`
  likewise returns a 2-element list, not a tuple — both consistent with
  pyc's existing, pre-dating, documented "no tuple type" choice, not new
  gaps. `pow(base, exp, mod)` (3-arg modular exponentiation) was
  additionally found to have never been implemented at all — the
  modulus was silently ignored. See IMPLEMENTATION.md.
- **Real bugs found and fixed**: `sorted(x, reverse=True)`,
  `list.sort(reverse=True)`, `list.sort(key=...)`, and `min`/`max`'s
  `key=` argument were all silently ignored — `reverse=`/`key=`'s value
  was misread as an unrelated positional argument (a key function
  mistaken for the reverse flag, or vice versa) due to how builtins with
  no known parameter signature merge keyword arguments back into the
  positional-argument list. All fixed; `cmp_to_key`-based sorting still
  doesn't support `reverse=` (a narrower, documented remaining gap). See
  IMPLEMENTATION.md.

## File I/O

- `open(path, mode)` / `with open(path, mode) as f:` — a synthetic file
  object (`__enter__`/`__exit__`/`.write()`/`.readlines()`). `.write(s)`
  appends `s` to the file; `.readlines()` reads the remainder of the file
  from the current position to EOF, returning a list of lines with each
  line's trailing `\n` kept (matching CPython — only the final line lacks
  it if the file itself doesn't end with a newline). No `.read()` (whole
  content as one string) or `.readline()` (one line at a time) — only
  `.readlines()`, and only reachable via the `with`-statement form (no
  bare `f = open(...); ...; f.close()` — `.close()` isn't implemented).

## Shutil / Glob / Csv

- `shutil.copyfile(src, dst)` — direct C-level `fopen`/`fread`/`fwrite`,
  bypassing pyc's synthetic file object entirely (so it works
  independently of `open()`'s own limitations).
- `shutil.move(src, dst)` — `rename(2)` first (fast path, same
  filesystem); falls back to copy + delete on failure (e.g.
  cross-filesystem).
- `shutil.rmtree(path)` — recursive directory removal.
- `glob.glob(pattern)` — `*`, `?`, `[seq]` wildcard matching (standard
  glob/fnmatch semantics) against a single directory's listing. **No
  recursive `**` support** — a hard scoping choice (recursive directory
  walking + pattern matching is a materially bigger feature than a
  single-directory match), documented, matches the precedent set by
  itertools' unbounded-iterator gap.
- `csv.reader(lines)` — takes a plain list of line-strings (real
  `csv.reader`'s actual general contract — any iterable of strings, not
  specifically a file object), typically used as
  `csv.reader(f.readlines())`. Returns a real (eager) list of rows, not
  CPython's lazy iterator — same eager-materialization approach as every
  itertools function this session. Minimal quoted-field support
  (`"a,b",c` → `['a,b', 'c']`, `""` inside a quoted field is a literal
  `"`); does **not** handle embedded newlines inside a quoted field
  (each input line is always exactly one row) or custom
  dialects/delimiters.
- `csv.writer(f)` / `.writerow(row)` — quotes fields containing `,`,
  `"`, or a newline (doubling embedded `"`), writes through the same
  file-write path as `.write()`.

## Itertools Expansion

- `itertools.accumulate(iterable, func=None)` — `func=None` means
  running sum. No `initial=` keyword support.
- `itertools.takewhile(pred, iterable)` / `itertools.dropwhile(pred,
  iterable)` — call `pred` via the existing generic callable-apply
  primitive, same as `sorted(key=...)`/`functools.reduce`.
- `itertools.compress(data, selectors)` — parallel filter.
- `itertools.groupby(iterable, key=None)` — groups only *consecutive*
  equal keys (matches real `groupby` — **not** a full partition; verify
  this against real output if porting code that assumes otherwise).
  Returns a list of `[key, group_list]` 2-element lists (real `groupby`
  yields `(key, group_iterator)` tuples — no tuple type in pyc, and the
  group is eagerly materialized like every other itertools function
  here, not lazily).
- `itertools.chain.from_iterable(iterable_of_iterables)` — flattens one
  level.
- **Found and fixed two real bugs while adding these**: (1)
  `chain.from_iterable([[1,2],[3,4]])` (homogeneous int/float list
  literals as the inner lists) silently returned `[]` — the same
  `pyc_ensure_boxed_list()` class of bug found in the `heapq`/`bisect`/
  `statistics` phase, just not yet applied to this new function. (2)
  `groupby(iterable, key=...)`'s `key=` keyword argument was silently
  dropped (every keyed call grouped by the whole item instead), since it
  went through the same generic dict-dispatch as every other synthetic
  module function (which doesn't read keyword arguments). Fixed by
  giving `groupby` the same AST-structural construction as
  `csv.writer`/`pathlib.Path` so `key=` can be read directly from the
  call's AST. See IMPLEMENTATION.md.
- **Newly discovered, pre-existing, general bug (found while verifying
    this phase, unrelated to itertools itself): list comprehensions don't
    support multi-variable `for a, b in pairs` unpacking.**
    `[k for k, g in [["a", 1], ["b", 2]]]` returns `[None, None]` instead
    of `['a', 'b']` — even just `k`/`g` alone, not any deeper mistake. A
    **plain `for` loop** with the identical unpacking
    (`for k, g in pairs: ...`) works correctly; only the comprehension form
    is affected. **Fixed**: AST-level change (`Comprehension.target`
    changed from `std::string` to `std::shared_ptr<Expr>` in `ast.h`),
    `lowerListComp` updated to call `lowerUnpackTarget()` for non-Name
    targets. Now works for 2- and 3-element tuple unpacking. See
    IMPLEMENTATION.md and KnownGapsPlan.md. Set comprehensions
    (`{a+b for a, b in pairs}`) and dict insertion-order were also fixed
    in a follow-up — see the Sets section and the dict-order note in the
    Collections Expansion section below.

## Regex (re)

PCRE2-backed. `re.search`/`re.match`/`re.finditer`/`re.findall`/`re.sub`/
`re.split`/`re.compile`, plus `re.IGNORECASE`/`re.MULTILINE`/`re.DOTALL`
flag support (positional or `flags=` keyword), matching CPython's real
flag values (`IGNORECASE=2`, `MULTILINE=8`, `DOTALL=16`). `re.sub`'s
`count=`/`re.split`'s `maxsplit=` are supported. Match objects support
`.group(i)`.

- **Real bug fixed**: `re.search`/`re.match` used to hardcode
  `PCRE2_CASELESS` unconditionally, so **every** match was
  case-insensitive regardless of any flag — `re.search("Hello",
  "hello")` incorrectly matched, and `re.IGNORECASE` itself didn't exist
  as a real value (a bare reference silently resolved to `None`, and was
  discarded even when passed positionally). Confirmed against real
  CPython, fixed by compiling case-sensitive by default and threading a
  real flags argument through every `re.*` function via the existing
  `compileRegex()` helper.
- `re.split`'s `maxsplit` was previously accepted syntactically but
  silently ignored (a no-op parameter) — now actually implemented.
  `"split"` was also missing from the module's synthetic dict/export
  list entirely (`import re as x; x.split(...)` would have failed) —
  added.
- **Real bug fixed**: `re.match(...)` was routed to the same
  implementation as `re.search(...)` (unanchored). Fixed: `re.match`
  now compiles with `PCRE2_ANCHORED` so it only matches at the start
  of the string. `re.match("b", "abc")` correctly returns None.
- **Not implemented**: `re.VERBOSE`/`re.ASCII`/`re.UNICODE` and other
  less-common flags; `.groups()`/`.groupdict()`/named capture groups;
  compiled-pattern-object methods (`re.compile(p).search(...)` — the
  compiled object is currently inert, only the free `re.*` functions
  work); custom delimiters/dialects beyond PCRE2 syntax itself.

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

`upper()`, `lower()`, `strip()`, `split(sep)`, `rsplit(sep, maxsplit)`,
`partition(sep)`, `rpartition(sep)`, `join(iterable)`,
`find()`, `count()`, `replace()`, `format(*args, **kwargs)`,
`str % value` (`%d`, `%s`, `%f`, `%.Nf`, `%x`, `%X`, `%o`, `%r`, `%%`, `%*d`)

- **Severe bug found and fixed**: `.format()`, `.rsplit()`,
  `.partition()`, and `.rpartition()` used to have no implementation at
  all — calling any of them silently printed `None` instead of raising
  an error or producing the right result. All four now work; `.format()`
  supports positional/explicit-index/keyword fields, format specs
  (sharing the same formatter f-strings use),
  `!r` conversion, and literal-brace escaping. Nested field access
  (`"{0.attr}"`) isn't supported. See IMPLEMENTATION.md.
- **Related bug found and fixed**: `.split(None)` / `.rsplit(None, ...)`
  with an *explicit* `None` separator (as opposed to omitting the
  argument) silently produced spurious empty-string elements for any run
  of more than one whitespace character, instead of collapsing runs the
  way whitespace-mode splitting should. See IMPLEMENTATION.md.

## List/Dict Methods

`list.append(x)`, `list.sort()`, `list.pop()`  
`dict.keys()`, `dict.values()`, `dict.items()`, `dict.get(key, default)`

- `list + list` concatenation — **found missing entirely and
  implemented this session** (`[1,2,3] + [4,5]` used to return `None`
  unconditionally). See IMPLEMENTATION.md.
- **Severe, high-impact bug found and fixed**: negative list indexing
  (`lst[-1]`, one of the most common indexing idioms in Python) either
  raised a bogus `IndexError` or silently produced/wrote the wrong
  value, depending on the list's internal storage representation — a
  homogeneous int/float fast-path list crashed on read and silently
  no-op'd on write; a boxed/mixed-type list read back a wrong value.
  `lst[3]`-style non-negative indexing was unaffected. See
  IMPLEMENTATION.md.
- **Found, documented, not fixed (minor)**: a homogeneous list of
  `bool` values loses its bool-ness on read — `[True, False][0]` prints
  `1` instead of `True`. See IMPLEMENTATION.md.
- **Real bug found and fixed**: list `==`/`!=`/`<`/`>`/`<=`/`>=`
  comparison silently gave wrong answers for homogeneous int/float list
  literals — e.g. `[1,2,3] == [1,2,4]` and `[1,2,3] == [1,2]` both
  incorrectly evaluated `True` (same root cause as the `if`/`while`
  truthiness bug documented under Control Flow above: reading internal
  fast-path list storage incorrectly). See IMPLEMENTATION.md.
- **Real bug found and fixed**: `{**mapping}` dict-literal unpacking
  silently lost data — `{**d1, **d2}` printed as `{None: {'b': 2}}`,
  with `d1`'s entries dropped entirely, instead of merging both dicts.
  See IMPLEMENTATION.md.
- **Severe bug found and fixed**: f-string format specs (`f"{x:.2f}"`)
  used to be silently ignored — the value was substituted unformatted. A
  pre-existing, deliberate MVP-era scope cut that was previously
  undocumented, not a new regression. Now implements a practical subset
  of Python's Format Specification Mini-Language (fill/align/sign/#/
  0-pad/width/,/precision/type — see IMPLEMENTATION.md for exactly
  what's covered and what isn't), including dynamic width/precision
  (`f"{x:{width}.{prec}f}"`). The `!r`/`!s`/`!a` conversion flag,
  previously captured by the parser but never applied, now is too. See
  IMPLEMENTATION.md.

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
- **Known real bugs, found while restoring a stranded test case (see
  IMPLEMENTATION.md's test-infrastructure section), not fixed here (out
  of scope for the session that found them)**: arithmetic between two
  plain variables holding complex values (`a = 1j; b = 2j; a + b`) prints
  `None` instead of a complex result. Complex `repr`/`print` never
  suppresses a zero real part the way CPython's does — `print(1j)` shows
  `(0.0+1.0j)` in pyc vs. CPython's `1j`, always. `==`/`!=` and unary `-`
  on complex values are also unimplemented in the shared comparison/
  negation runtime functions.

## Sets

A real `set` type (type 20) — insertion-ordered, dedup-by-value. Backed by
a plain `std::vector<PyObject*>` with linear-scan equality
(`PyObject_CompareBool`), matching the dict container's O(n)-lookup
approach (pyc prioritizes simplicity over hash performance). CPython sets
don't guarantee insertion order, but pyc's do — a deliberate choice that
makes `{x for x in iter}` match list-comp-style output for single-iteration
test cases.

- `set` literals (`{1, 2, 3}`), `set()` / `set(iterable)` constructor
- Deduplication: `{1, 2, 2, 3, 1}` → `{1, 2, 3}`
- `in` / `not in` membership, `len()`, iteration (`for x in s:`)
- Operators: `|` (union), `&` (intersection), `-` (difference), `^`
  (symmetric difference)
- Comparison: `==`/`!=` (set equality), `<=`/`<` (subset/proper-subset),
  `>=`/`>` (superset/proper-superset)
- Methods: `.add()`/`.remove()`/`.discard()`/`.pop()`/`.clear()`/`.copy()`/
  `.update()`/`.union()`/`.intersection()`/`.difference()`/
  `.symmetric_difference()`/`.issubset()`/`.issuperset()`
- Set comprehensions: `{x for x in iter}`, `{x*2 for x in range(5) if x>1}`,
  including multi-variable unpacking (`{a+b for a, b in pairs}`)
- `print()`/`str()`: `{1, 2, 3}`; empty set prints as `set()` (a literal
  `{}` is a dict, matching CPython)
- `type()` → `<class 'set'>`, `isinstance(x, set)`, `sorted(set)`,
  `sum(set)`, `any(set)`/`all(set)`, `list(set)`, `reversed(set)`
- `set` as a `defaultdict` factory (`defaultdict(set)`)

## Bytes / Bytearray

A real `bytes`/`bytearray` type — previously an explicitly-declined scope
decision (see IMPLEMENTATION.md), reopened on request. Reuses
`pathlib.Path`'s "same storage, new type tag" pattern (types 17/18,
backed by the existing `str` field), not a new value shape like
`complex` needed.

- `b"..."` literals, including embedded non-printable/binary bytes
  (`b"\x00\x01\xff"`) — previously silently miscompiled to an empty str
  literal (a real bug, not just "unsupported"), now fixed.
- Construction: `bytes()`, `bytes(n)` (zero-filled), `bytes([ints])`,
  `bytes(str, encoding)`, `bytes(bytes_or_bytearray)` — and the same
  forms for `bytearray(...)`.
- Indexing (`b[i]` → **int** 0-255, not a length-1 bytes object — the one
  place this genuinely diverges from `str[i]`), slicing (`b[i:j]` →
  same type as the receiver), `len()`, iteration (each element an int),
  `in`/`not in` (accepts either a single int 0-255 or a bytes-like
  substring), `+` concatenation (result type follows the left operand:
  `bytearray + bytes -> bytearray`, `bytes + bytearray -> bytes`,
  matching CPython exactly), `==`/`!=`/ordering (lexicographic, and
  bytes/bytearray compare equal across the two types by content).
- `bytearray` mutability: `ba[i] = x`, `.append(int)`, `.extend(iterable)`.
- `.hex()` / `bytes.fromhex(s)`, `bytes.decode(encoding='utf-8')` /
  `str.encode(encoding='utf-8')`.
- `repr()`/`print()` show CPython's `b'...'` form with `\xHH` escaping
  for non-printable bytes and `\n`/`\t`/`\r`/`\\` shorthand — verified
  byte-for-byte against real CPython, with one simplification: always
  single-quotes with an escaped `\'`, rather than CPython's
  quote-switching to `"..."` when the content has a `'` but no `"`.
  `bytearray` wraps the same form in `bytearray(...)`.
- `isinstance(x, bytes)` / `isinstance(x, bytearray)`, `type()`.
- `hashlib.md5/sha1/sha256` now accept `str`/`bytes`/`bytearray`
  interchangeably; `hashlib.*().digest()` (raw bytes) is new alongside
  the existing `.hexdigest()`. `base64.b64encode`/`b64decode` and
  `struct.pack` now **return real bytes** (previously str) — see
  IMPLEMENTATION.md for the details of this behavior change.
- **Not implemented**: `bytes % formatting`, `memoryview`, `.join()`/
  full `.split()` parity, the buffer protocol, `open(path, "rb")`
  returning real bytes (no binary-mode file reading exists at all yet).

## Decimal

Real arbitrary-precision base-10 arithmetic via `libmpdec` (type 19) —
the same C library CPython's own `_decimal` module is built on (see
CMakeLists.txt; a real build dependency, `libmpdec-dev`, not a vendored
copy). One shared global context: 28 significant digits,
`ROUND_HALF_EVEN`, matching CPython's real defaults exactly (verified —
`Decimal('0.1') + Decimal('0.2') == Decimal('0.3')` exactly, `Decimal(1)
/ Decimal(3)` gives the same 28-digit result as real CPython).

- `Decimal(str)` / `Decimal(int)` / `Decimal(float)` / `Decimal(Decimal)`
  construction. `Decimal(float)` uses the float's exact-enough binary
  value (via `%.17g` formatting, an approximation of CPython's true
  exact-binary-value conversion — documented, not a bit-for-bit match).
- Arithmetic: `+ - * / //`, unary `-`. **Wired into the same generic
  runtime arithmetic functions every other numeric type uses**
  (`PyNumber_Add`/`Subtract`/`Multiply`/`Divide`/`TrueDivide`/`Negate`),
  not a compile-time-only tracking mechanism — this is a deliberate,
  better-quality choice than how `complex` numbers were built (see
  IMPLEMENTATION.md), meaning Decimal arithmetic works correctly even
  when a value crosses a function-parameter boundary untyped. `Decimal +
  int` auto-converts the int; `Decimal + float` raises (matches real
  CPython exactly — real `Decimal` doesn't implicitly coerce from
  `float` either). Division by zero raises `ZeroDivisionError`.
- Comparisons (`==`/`!=`/`<`/`>`/`<=`/`>=`): exact for Decimal-vs-Decimal
  and Decimal-vs-int; Decimal-vs-float goes through the same
  string-round-trip construction `Decimal(float)` uses (an
  approximation, not exact binary comparison — documented).
- `.quantize(Decimal('0.01'))` — the single most common real-world
  Decimal method (rounding money to N places).
- `int()`/`float()` conversion, `bool()`/truthiness (`Decimal('0')` is
  falsy), `str()`/`print()` (bare digit string, e.g. `3.14`), `repr()`
  (`Decimal('3.14')`, matching CPython exactly, including in nested
  containers — `print([Decimal('3.14')])` → `[Decimal('3.14')]`),
  `isinstance(x, Decimal)`, `type()`.
- **Not implemented**: `decimal.getcontext()`/`localcontext()` and any
  precision/rounding-mode mutation (the context is fixed at 28
  digits/`ROUND_HALF_EVEN` for the whole program), exception traps
  (`InvalidOperation`/`Overflow`/`Underflow` signals), `.sqrt()`/`.ln()`/
  `.exp()`/other `libmpdec`-backed math methods beyond `.quantize()`, `%`
  formatting.

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
  hex string; `.digest()` returns the raw digest as real bytes. MD5/SHA-1/
  SHA-256 are implemented from scratch (standard algorithms, no OpenSSL/
  libcrypto dependency — matches `random`'s from-scratch MT19937
  precedent), verified byte-for-byte against real `hashlib` output. Works
  via `import hashlib` and `from hashlib import md5, sha1, sha256`
  (including aliasing). Accepts `str`, `bytes`, or `bytearray`
  interchangeably as input (still more permissive than real CPython,
  which requires actual `bytes`). No `.update()` or `.copy()`.
- `base64.b64encode(s)` / `base64.b64decode(s)` — standard RFC 4648
  alphabet, implemented from scratch. Accepts `str`/`bytes`/`bytearray`
  input and **returns real bytes** (matching CPython exactly — this
  changed from returning `str` when the `bytes` type was added; see
  IMPLEMENTATION.md).
- `struct.pack(fmt, *values)` / `struct.unpack(fmt, data)` — common format
  codes (`b`/`B`/`h`/`H`/`i`/`I`/`l`/`L`/`q`/`Q`/`f`/`d`/`s`) and
  endianness prefixes (`<`/`>`/`!`/`=`, all treated explicitly —
  `=`/no-prefix defaults to little-endian, matching every platform pyc
  targets). Unsupported codes (`n`/`N`, native alignment padding) are
  silently skipped rather than erroring. `struct.pack` returns real bytes
  (see above); `struct.unpack` accepts `str`/`bytes`/`bytearray` input
  and still returns a plain list, not a tuple (same documented gap as
  `os.path.splitext`/itertools).
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

## Collections Expansion

- `collections.deque(iterable=[])` — a plain list (type 1) at runtime,
  **not** a new type: `.append`/`.pop`/`.copy`/`.clear` already work
  through the existing list machinery, extended (`.pop`/`.copy`/`.clear`)
  to also accept a `deque`-tagged receiver. `.appendleft(x)`,
  `.popleft()`, `.rotate(n=1)` (positive rotates right, negative rotates
  left — verified against real `collections.deque.rotate`) are new,
  typeOf-gated dispatch. **Documented gaps**: `print(d)` shows a plain
  list repr (`[1, 2, 3]`), not CPython's `deque([1, 2, 3])`; `isinstance(d,
  list)` is `True` (same runtime type tag, no dedicated `deque` type).
- `collections.namedtuple(typename, field_names)` — returns a callable
  "descriptor bundle" (same closure/`functools.partial` mechanism used
  elsewhere); calling it builds a **plain dict** pairing each field name
  to its positional argument. Field access (`p.x`) works for free via the
  existing generic attribute-read path (`Pyc_GetItem` on any dict with a
  matching string key already supports `.attr` syntax — no new runtime
  code needed for this part). **Documented gaps**: positional
  construction only (`Point(1, 2)`, not `Point(x=1, y=2)` — no
  keyword-argument channel through the callable-apply mechanism);
  `print(p)` shows a dict repr, not CPython's `Point(x=1, y=2)`; no real
  tuple behavior (`len()`, indexing by position) beyond attribute access.
- `collections.defaultdict(default_factory)` — a real dict whose factory
  is tracked **out-of-band**, in a `PyObject* -> PyObject*` map keyed by
  the dict's own pointer (`g_pycDefaultFactories`, same pattern as the
  open-file-object table `g_pycFiles`), *not* as a visible dict entry —
  an earlier version stashed the factory under a reserved dict key, which
  leaked into `print()`/`len()`/iteration on every defaultdict; moved
  out-of-band during verification. `Pyc_Subscript`'s dict-miss path
  checks this map before raising `KeyError`: if present, calls the
  factory with no arguments, stores the result under the missing key
  (mutate-on-access, matching real `defaultdict`), and returns it.
  `defaultdict(list)`/`defaultdict(int)`/`defaultdict(dict)`/
  `defaultdict(float)`/`defaultdict(str)` are supported. **Documented
  gap**: `print(dd)` shows a plain dict repr, not CPython's
  `defaultdict(<class 'list'>, {...})`.
- **Found and fixed one real, general gap while building this**: a bare
  (uncalled) reference to a builtin type name — `list` in
  `defaultdict(list)`, not `list(x)` — had **no runtime representation at
  all**; only actual calls like `list(x)` were recognized structurally.
  `defaultdict(list)` silently stored `None` as the factory, so every
  auto-populated key raised `KeyError` instead of getting an empty list.
  Fixed by adding zero-arg factory tokens (`PyBuiltin_ListFactory`,
  `PyBuiltin_DictFactory`, `PyBuiltin_IntFactory`, `PyBuiltin_FloatFactory`,
  `PyBuiltin_StrFactory`) for `list`/`dict`/`int`/`float`/`str`, using the
  same first-class-value mechanism (`B13`) already used for bare
  exception-class references like `exc = ValueError`. Scoped to these
  five names only — general first-class use of arbitrary builtins (e.g.
  `f = sorted`) is not supported.
- **Dict iteration order is now insertion-order-preserving** (matching
  CPython 3.7+). The dict payload was changed from `std::unordered_map`
  to `std::vector<std::pair<PyObject*, PyObject*>>` with linear-scan
  value-equality lookup — the lookup was already O(n) (the hash map's
  raw-pointer key was never used for the actual value comparison), so
  this is the same complexity with correct iteration order.

## Assignment Forms

```python
x = 1          # simple
a = b = 5      # multi-target
a, b = 1, 2    # tuple unpack
a[i] = v       # subscript
d[k] = v       # dict subscript
```

- **Severe, unfixed bug found and documented (not fixed — see
  IMPLEMENTATION.md)**: variable names of the form `t<N>`/`c<N>` (e.g.
  `t3`, `c0`, `c17`) can silently collide with the compiler's own
  internal temp-naming scheme, causing either silently wrong output
  (`c0 = "hello"; print(c0)` prints `0`) or a hard compile failure,
  depending on the exact sequence of internal temp allocation at that
  point in the function. Avoid naming variables this way (a short
  letter followed immediately by digits, nothing else) until this is
  fixed.

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
6=cell, 10=exception, 11=function, 12=exception class, 13=complex,
17=bytes, 18=bytearray, 19=decimal, 20=set.

Most values flow as boxed `PyObject*` through LLVM IR. Arithmetic dispatches
at runtime via `PyNumber_Add` etc. Comparisons via `PyObject_CompareBool`.
Truthiness via `PyObject_TruthBoxed`. Global variables use LLVM `GlobalVariable`
with `InternalLinkage`.

IR instructions carry conservative result type metadata (`int`, `float`, `bool`,
`str`, `boxed`). Codegen uses this for native paths (range loop counters, numeric
arithmetic) with boxed fallback for uncertain cases.
