# pyc — Implemented Features

Current test count: **143/143** (all compared against CPython output).

## Types and literals

| Type | Notes |
|------|-------|
| `int` | Full arithmetic, comparison, floor/true division |
| `float` | `3.14`, `1e-3`, mixed int/float; shortest round-trip printing |
| `bool` | `True`/`False`; prints correctly; arithmetic with ints (`True+1=2`) |
| `str` | Literals, `+`, `*`, f-strings, `%` formatting, all major methods |
| `list` | Literals, subscript get/set, simple slices, comprehensions, append/sort/pop |
| `dict` | Literals, subscript get/set, keys/values/items |
| `tuple` | Literals and unpacking (mapped to list internally) |
| `None` | Constant, comparison, printing |

## Operators

```
+  -  *  /  //  %  **          arithmetic (int, float, bool, str* for +/*)
==  !=  <  >  <=  >=           comparison (numeric + string + chained 1<x<10)
is  is not                     identity
in  not in                     membership (list, str, dict)
and  or  not                   boolean (short-circuit, returns actual value)
-x  +x                         unary
+=  -=  *=  /=  //=  %=  **=  augmented (on names and on subscripts a[i]+=1)
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
def f(a, b=10, *args):         positional, default, *args (collection NYI)
    return a, b                multi-value return (returned as list)

def f(a, b): ...
f(b=3, a=4)                    keyword call arguments
```

Nested functions work. `global x` shares module-level storage via LLVM
`GlobalVariable`. `try/except` basic form works.

## Assignment forms

```python
x = 1          # simple
a = b = 5      # multi-target
a, b = 1, 2    # tuple unpack
a[i] = v       # subscript
d[k] = v       # dict subscript
```

## Builtins

`print(*args)`, `range(n)` / `range(s,e)` / `range(s,e,step)`,
`len(x)`, `str(x)`, `int(x)`, `float(x)`, `abs(x)`,
`min(a,b,...)` / `min(list)`, `max(a,b,...)` / `max(list)`,
`list(x)`, `enumerate(iterable)`, `zip(a, b)`

## String methods

`upper()`, `lower()`, `strip()`, `split(sep)`, `join(iterable)`,
`str % value` (`%d`, `%s`, `%f`, `%.Nf`)

## List/dict methods

`list.append(x)`, `list.sort()`, `list.pop()`  
`dict.keys()`, `dict.values()`, `dict.items()`

## Comprehensions

```python
[expr for x in iterable]
[expr for x in iterable if cond]
[[inner for ...] for ...]      nested
```

## Runtime architecture

`PyObject` is a flat struct: `{refcount, type, value(long), dvalue(double),
list, dict, str}`. Type codes: 0=int, 1=list, 2=dict, 3=str, 4=float, 5=bool.

Most values still flow as boxed `PyObject*` through LLVM IR. Arithmetic dispatches
at runtime via `PyNumber_Add` etc. Comparisons via `PyObject_CompareBool`.
Truthiness via `PyObject_TruthBoxed`. Global variables use LLVM `GlobalVariable`
with `InternalLinkage`.

IR instructions now also carry conservative result type metadata. This is a
compiler-side analysis aid for native paths. Codegen only uses it when a
specific lowering has a boxed fallback for uncertain cases.

## Optimization status

- `range(...)` for-loops are lowered directly to loop blocks instead of
  allocating a boxed range list.
- Hidden `range(...)` loop counters use native i64 compare/increment operations.
- Constants and simple numeric arithmetic are annotated with conservative IR
  result types.
- Proven numeric `+`, `-`, and `*` operations use native LLVM integer/double
  arithmetic, then box their result for Python-visible storage or calls.
- Division, uncertain types, and list elements still use the boxed general path.
  Longer-lived unboxed numeric locals and homogeneous numeric-vector lists are
  planned next.

## Not yet implemented

- `sum()`, `sorted()`, `any()`, `all()`, `isinstance()` builtins
- `str.find()`, `str.replace()`, `str.count()` methods
- Full list/string slicing semantics including non-default step
- Dict comprehensions (list comps work; dict comp stubs not wired)
- `lambda` expressions
- `nonlocal` statement
- Classes and OOP
- `import` / module system
- `*args` collection in function body
- Walrus operator `:=`
