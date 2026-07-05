# pyc — Implemented Features

Current test count: **249 strict-pass / 251 total** (243 curated + 8 FILE_CASES; FILE_CASES are now strictly validated by the runner and 2 currently fail — see `CORRECTNESS.md` and `tests/runner.py`).

**Bug fixes:** `PyObject_Print` now flushes stdout after every print call (ensures output is visible when stdout is fully buffered). `pyc_setup_sys` now properly DECREFs all allocated index and string objects (fixes memory leaks). Subscript AugAssign (`a[i] += 1`) now carries result type metadata for native arithmetic optimization. Corrected `llvm::cast` → `llvm::dyn_cast` in class codegen. Fixed `PyDict_GetItem` to always return new references (caller responsible for DECREF). Added `ownedSlots` tracking in codegen assign path to DECREF old values on reassignment. LLVM verification failures are now fatal.

**Milestone update:** `sum`/`sorted`/`any`/`all`/`isinstance` builtins and `str.find`/`count`/`replace` methods are now wired and passing (B1).
Full slicing (get/set, step, negatives, str + list) implemented (B2).
Dict comprehensions (single/multi-generator, if conditions, nested) implemented (B3).
Type tracking foundation strengthened for unboxing (A1): i64 normalized to numeric int, valueTypes cleared at function boundaries, conservative loop back-edge widening in while/for/range (prevents incorrect narrow types across iterations).
Valgrind test target added to CI.
Class statements with `__init__`, instance attributes, method calls, and `__str__`/`__repr__` protocol implemented.
With statement support added for context managers with `__enter__`/`__exit__`.

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

Lambdas (B4 complete for the token-based model): `lambda` expressions produce a
synthetic nested IR function and a string "callable token" (the synthetic name)
as their runtime value. You can assign them, pass them as arguments, store them
in containers, return them from functions, unpack them, subscript them, and call
them directly or indirectly. Indirect calls use `Pyc_Apply(token, list)` where
the token is the runtime string value and the list is the flat argument list
(with dynamic * contents spliced in for indirect callees). Generated
`__apply__<name>` adapters (registered at module startup) perform the actual
dispatch and are shape-aware for targets that declare `*vararg`. Direct targets
use fast paths or `__va_*` wrappers. **kwargs and full first-class objects with
cells/closures remain future work.

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

## Classes

A minimal `class` that supports `__init__`, instance attributes via `self`, and method calls. No class inheritance, no descriptors, no property, no metaclasses.

```python
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def distance(self, other):
        return ((self.x - other.x)**2 + (self.y - other.y)**2)**0.5

p1 = Point(0, 0)
p2 = Point(3, 4)
print(p1.distance(p2))  # 5.0
```

## `__str__` and `__repr__` protocol

User-defined classes can override `__str__` and `__repr__`. The `print()` function calls `__str__` on objects that have it.

```python
class Name:
    def __init__(self, n):
        self.n = n
    def __str__(self):
        return self.n
    def __repr__(self):
        return f"Name({self.n})"

print(Name("hello"))   # hello
```

## `with` statements (partial)

`with context_manager:` for objects that implement `__enter__` and `__exit__`. The `as` clause binding needs further work.

```python
class Dummy:
    def __enter__(self):
        return "entered"
    def __exit__(self, exc_type, exc_val, exc_tb):
        return False

with Dummy():
    print("in with")
```

## Not yet implemented (or partial)

- `lambda` expressions (B4 complete for the common model): write `lambda`, assign/pass/store/return/unpack/subscript it, call directly or indirectly via string "callable tokens" + `Pyc_Apply(token, list)` + generated `__apply__<name>` adapters (registered at startup). Dynamic `*args` works at indirect call sites (spliced into the flat list for `Pyc_Apply`). Lambdas may declare `*args` in their signature; callee-side collection works. Adapters are shape-aware for targets that declare `*vararg`. Limitations still pending: full first-class function objects (identity/equality), cells/`nonlocal`/true closures, `**kwargs`.
- `nonlocal` statement (B5): single- and multi-level nesting (owner, assigning intermediates, forward-only scopes that only declare `nonlocal` to forward the cell); AugAssign to cell-backed names (`x += k`, etc. — explicit `PyCell_Get` of LHS + `PyCell_Set` of result); multi-target unpack into nonlocals (`a,b = b,a`). Cells allocated in the owning scope (with param capture), hidden leading `<name>_cell` parameters synthesized and passed on direct calls, uniform `<name>_cell` slot convention, `isCellBackedHere` predicate, and `PyCell_New/Get/Set/Check` (type=6). 4+ curated B5 cases (incl. augassign + unpack + multi-level) are green at --opt=0. Deeper aliasing, lambdas capturing cells, and class interactions remain future work.
- `import` / module system (partial): `import sys` registers `sys` as a module-level global, but `sys.version` and other attributes are not available. User module imports not yet supported.
- `*args` / `**kwargs` (call-site * works for literals (static) and dynamic cases (via __va wrappers for direct targets or flat-list splice for indirect `Pyc_Apply`); declared * collection on callee side works; adapters support *vararg targets; **kwargs pending)
- Walrus operator `:=`
