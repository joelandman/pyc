# Real Tuple Type (Type 7)

## Overview

pyc now has a **real, distinct tuple type** (type tag 7), matching CPython
semantics. Previously, tuple literals were mapped to lists (type 1) — an
intermediate state left the compiler emitting calls to `PyTuple_New`/
`PyTuple_SetItem` with no runtime implementation, breaking any program
containing a tuple literal at link time. Both the runtime and compiler
halves are now complete.

## Implementation

- **Runtime (`src/runtime/Runtime.cpp`)**: `PyTuple_New`, `PyTuple_SetItem`,
  `PyTuple_SetItemBoxed`, `PyTuple_GetItem`, `PyTuple_Size`, `PyTuple_Concat`,
  `PyTuple_Repeat`, `PyBuiltin_Tuple` — reusing the `list`/`ilist`/`flist`/
  `list_item_type` storage fields (a tuple is structurally a list with a
  different tag and immutable semantics). Tuple branches added to:
  `PyObject_PrintBase`/`PyObject_PrintElement`/`PyBuiltin_Repr` (paren
  format `(1, 2, 3)`, `(1,)`, `()`), `PyObject_CompareBool` (tuple-vs-tuple
  structural; tuple-vs-list → False), `PyBuiltin_Type` (`<class 'tuple'>`),
  `Pyc_IsInstance`, `PyBuiltin_Len`, `Pyc_GetItem`/`Pyc_Subscript`,
  `Pyc_Contains`, `PyList_Unpack2`/`Unpack3`, `Pyc_GetSlice` (tuple slice →
  tuple), `PyBuiltin_List` (tuple → list), `PyString_Format` (`%` unpacking),
  `PyObject_TruthValue`, `PyNumber_Add` (tuple+tuple), `PyNumber_Multiply`
  (tuple*int), `pyc_flattenRecursive`. `PyBuiltin_Divmod` now returns a real
  tuple.
- **Compiler (`src/Compiler.cpp`)**: `lowerList` emits `PyTuple_NewBoxed`/
  `PyTuple_SetItemBoxed` for Tuple AST nodes; `tuple()` builtin calls
  `PyBuiltin_Tuple`; `isinstance` table maps `tuple` → typecode 7.
- **Codegen (`src/codegen/Codegen.cpp`)**: LLVM extern declarations for all
  `PyTuple_*` functions.
- **Headers**: `object_struct.h` (type tag 7), `runtime.h` (declarations).

## Supported Operations

- Tuple literals: `(1, 2, 3)`, `(1,)`, `()`, `((1, 2), (3, 4))`
- Printing/repr: `(1, 2, 3)`, `(1,)`, `()` (matching CPython exactly)
- Indexing: `t[0]`, `t[-1]` (with IndexError on out-of-range)
- Slicing: `t[1:]` → tuple
- `len(t)`
- Unpacking: `a, b = t`, `for x, y in pairs`, `return a, b`
- Concatenation: `(1, 2) + (3, 4)` → `(1, 2, 3, 4)`
- Repetition: `(1, 2) * 2` → `(1, 2, 1, 2)`, `2 * (1, 2)`
- Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=` (structural; tuple ≠ list)
- Membership: `x in t`, `x not in t`
- `tuple(iterable)` builtin (accepts list/str/dict/set/tuple/iterable)
- `divmod(a, b)` returns a real tuple
- `type(t)` → `<class 'tuple'>`, `isinstance(t, tuple)` → True/False
- Truthiness: non-empty tuple is truthy, empty tuple is falsy
- `%` formatting: `"%s %d" % (1, 2)` unpacks the tuple

## Functions Now Returning Tuples (Follow-up)

These functions previously returned lists but now return real tuples,
matching CPython:

- `itertools.product`/`combinations`/`permutations`/`zip_longest` —
  inner combo entries are tuples (outer container stays a list)
- `os.path.splitext(p)` → `(root, ext)` 2-tuple
- `os.path.split(p)` → `(head, tail)` 2-tuple (newly implemented; was
  entirely missing before)
- `operator.itemgetter(1, 2)(obj)` / `operator.attrgetter(...)` multi-key
  → tuple
- `struct.unpack(fmt, data)` → tuple
- `str.partition(sep)` / `str.rpartition(sep)` → 3-tuple
- `enumerate(iterable)` → list of 2-tuples `(index, value)`
- `zip(a, b)` → list of 2-tuples
- `dict.items()` → list of 2-tuples `(key, value)`

Also fixed during this work:
- `complex.real` / `complex.imag` attribute reads (previously returned
  `None` — `Pyc_GetItem` now handles type 13)
- `str.split`/`rsplit` method dispatch was catching `os.path.split(...)`
  — now gated on `typeOf(obj) != "dict"` so `os.path.split` reaches the
  runtime

Still returning lists (narrower remaining gaps): `itertools.groupby`
key/group pairs, `collections.Counter.most_common` entries.

## Verification

All 557 runner tests pass (0 failures), 9/9 import tests pass, valgrind
shows 0 errors. Verified at both -O0 (runner) and -O2 (default).