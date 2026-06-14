# pyc — Implemented Features

Current test count: **180/180** (all compared against CPython output).

**Milestone update:** `sum`/`sorted`/`any`/`all`/`isinstance` builtins and `str.find`/`count`/`replace` methods are now wired and passing (B1).
Full slicing (get/set, step, negatives, str + list) implemented (B2).
Dict comprehensions (single/multi-generator, if conditions, nested) implemented (B3).
Type tracking foundation strengthened for unboxing (A1): i64 normalized to numeric int, valueTypes cleared at function boundaries, conservative loop back-edge widening in while/for/range (prevents incorrect narrow types across iterations).

## Types and literals

| Type | Notes |
|------|-------|
| `int` | Full arithmetic, comparison, floor/true division |
| `float` | `3.14`, `1e-3`, mixed int/float; shortest round-trip printing |
| `bool` | `True`/`False`; prints correctly; arithmetic with ints (`True+1=2`) |
| `str` | Literals, `+`, `*`, f-strings, `%` formatting, all major methods, full slicing |
| `list` | Literals, subscript get/set, full slices (incl. step), comprehensions, append/sort/pop |
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
def f(a, b=10, *args):         positional, default, *args (call-site unpacking + callee collection supported; **kwargs NYI)
    return a, b                multi-value return (returned as list)

def f(a, b): ...
f(b=3, a=4)                    keyword call arguments
```

Nested functions work. `global x` shares module-level storage via LLVM
`GlobalVariable`. `try/except` basic form works.

Lambdas are supported as values (B4): you can assign them, pass them as arguments,
store them in lists, and call them indirectly. The implementation uses string
"callable tokens" (synthetic names) resolved at call time via a small runtime
registry + `Pyc_Apply` + generated `__apply__<name>` adapters. Dynamic `*args`
at call sites works for both direct targets (via `__va_*` wrappers) and indirect
callables (spliced into the flat arg list passed to `Pyc_Apply`). Adapters are
shape-aware and correctly handle targets that declare `*vararg`. **kwargs and
full first-class function objects (with cells/closures) are still pending.

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
`list(x)`, `enumerate(iterable)`, `zip(a, b)`,
`sum(x)`, `sorted(x)`, `any(x)`, `all(x)`, `isinstance(obj, info)`

## String methods

`upper()`, `lower()`, `strip()`, `split(sep)`, `join(iterable)`,
`find()`, `count()`, `replace()`,
`str % value` (`%d`, `%s`, `%f`, `%.Nf`)

## List/dict methods

`list.append(x)`, `list.sort()`, `list.pop()`  
`dict.keys()`, `dict.values()`, `dict.items()`

## Comprehensions

```python
[expr for x in iterable]
[expr for x in iterable if cond]
[[inner for ...] for ...]      nested
{ k: v for x in iterable if cond }
{ k: v for x in a for y in b }  product / nested generators
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

## Not yet implemented (or partial)

- `lambda` expressions (as values: assign, pass as argument, store in containers, and call indirectly via callable tokens + `Pyc_Apply` + `__apply__` adapters; direct/assigned calls, *args in lambda signatures, and dynamic * at indirect call sites are supported; full first-class callable objects and **kwargs forwarding pending)
- `nonlocal` statement
- Classes and OOP
- `import` / module system
- `*args` / `**kwargs` (call-site * unpacking works for literals (static) and dynamic lists (via __va wrappers or indirect `Pyc_Apply` arg lists); declared *args collection on the callee side works; adapters support *vararg targets for indirect dispatch; **kwargs pending)
- Walrus operator `:=`
