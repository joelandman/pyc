# pyc — Implemented Features

Current test count: **300/300** (runner shows 300/300, file_case_failures=0).

## Types and literals

| Type | Notes |
|------|-------|
| `int` | Full arithmetic, comparison, floor/true division, small int cache (-5..256) |
| `float` | `3.14`, `1e-3`, mixed int/float; shortest round-trip printing |
| `bool` | `True`/`False`; prints correctly; arithmetic with ints (`True+1=2`); singleton identity |
| `str` | Literals, `+`, `*`, f-strings, `%` formatting, all major methods, full slicing |
| `list` | Literals, subscript get/set, full slices (incl. step), comprehensions, append/sort/pop |
| `dict` | Literals, subscript get/set, keys/values/items, `get()` with default |
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
**                             power (int*int, int*float, float*int, float*float, complex)
```

## Control flow

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

## Standard library stubs

- `os.path.exists()`, `os.path.isfile()`, `os.path.isdir()` — real POSIX implementations
- `os.unlink()` — deletes files
- `subprocess.call()` — executes commands via fork/exec/pipe
- `subprocess.check_output()` — captures command output
- `sys` module (argv, stderr)
- `cmath` module: `sqrt`, `log`, `exp`, `sin`, `cos`, `tan`

## String methods

`upper()`, `lower()`, `strip()`, `split(sep)`, `join(iterable)`,
`find()`, `count()`, `replace()`,
`str % value` (`%d`, `%s`, `%f`, `%.Nf`, `%x`, `%X`, `%o`, `%r`, `%%`, `%*d`)

## List/dict methods

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

## Generator expressions

- `(x*2 for x in range(5))` — eager materialization via thread-local buffer
- Works with `list()`, `for` loops, `join()`

## Complex numbers

- Literals: `1j`, `3j`, `2.5j`
- Arithmetic: `+ - * /` (via `PyComplex_Add/Sub/Mul/Div`)
- Power: `**` (via `PyComplex_Pow`)
- Absolute value: `abs()` (via `PyComplex_Abs`)
- Builtin: `complex()`, `complex(3)`, `complex(3, 4)`, `complex("3+4j")`
- cmath module: `sqrt`, `log`, `exp`, `sin`, `cos`, `tan`

## Assignment forms

```python
x = 1          # simple
a = b = 5      # multi-target
a, b = 1, 2    # tuple unpack
a[i] = v       # subscript
d[k] = v       # dict subscript
```

## Runtime architecture

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

## Optimization status

- `range(...)` for-loops lowered directly to loop blocks (no boxed range list)
- Hidden range loop counters use native i64 compare/increment
- Proven numeric `+`, `-`, `*`, `//`, `%` use native LLVM integer/double arithmetic
- Homogeneous numeric lists with native `int64`/`double` element storage
- Allocation sinking for numeric locals (native i64 alloca)
- Specialized function variants from proven call-site types
- Conservative type tracking with loop back-edge widening

## Import system

- `import X` — loads same-directory `.py` module, binds module dict to name
- `from X import Y` — loads module, extracts attribute, binds to name
- `from X import *` — loads module, exports all non-underscore names as module globals
- `import X as Y` — loads module, binds module dict to alias name
- External modules (`re`, `math`, etc.) report clear ImportError
- Cross-module globals via `__module__` function returning module dict pointer
