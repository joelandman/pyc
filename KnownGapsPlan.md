# Known Gaps Plan

All four documented gaps in the pyc compiler have been fixed.

**Fixed:** Gap 1 (`**dict` → `**kwargs`), Gap 2 (list comp unpacking), Gap 3 (`re.match` anchoring), Gap 4 (variable name collision)

---

## Gap 1: `**dict` Spread Unmatched Keys Not Routed to `**kwargs`

**Status:** ✅ Fixed
**Location:** `Compiler.cpp` lines 4724-4776 (dict spread path), `Runtime.cpp` `Pyc_RouteSpreadKwargs`

### Symptom

```python
def f(**kwargs):
    print(kwargs)

f(**{"p": 1, "q": 2})  # prints {} instead of {'p': 1, 'q': 2}
```

### Root Cause

The dict-spread path uses `Pyc_DictGetOrDefault(dict, key, fallback)` for each named parameter individually. When a key in the spread dict doesn't match any regular parameter name, it's silently dropped. The unmatched keys are never collected into the `**kwargs` catch-all parameter.

The comment at `Compiler.cpp:4741-4748` explicitly acknowledges this gap:
> "Routing a **dict spread's own unmatched entries into a **kwargs catch-all is a further, separate, still-open gap — not attempted here."

### Implementation Approach

Two-part fix:

**A. Runtime helper** — Add `Pyc_RouteSpreadKwargs(PyObject* spread_dict, PyObject* param_names_list, PyObject* kwargs_dict)` to Runtime.cpp:
- Iterates over the spread dict's keys
- For each key, checks if it's in the callee's named parameters
- If NOT a named parameter, sets it in the `**kwargs` dict
- Called at the end of the dict-spread parameter resolution

**B. Compiler codegen** — In the dict-spread lowering path (`Compiler.cpp` ~line 4741):
- After the per-parameter `Pyc_DictGetOrDefault` loop, call `Pyc_RouteSpreadKwargs` with:
  - The spread dict (runtime value)
  - A list of the callee's parameter names (compile-time constant)
  - The kwargs dict (already created if the function has `**kwargs`)
- Only emit this call when the callee has a `**kwargs` parameter

This is a runtime set-difference: spread_dict_keys - param_names = kwargs_entries.

### Files to Change

| File | Change |
|------|--------|
| `src/runtime/Runtime.cpp` | Add `Pyc_RouteSpreadKwargs` helper |
| `include/pyc/runtime.h` | Declare `Pyc_RouteSpreadKwargs` |
| `src/codegen/Codegen.cpp` | Declare `Pyc_RouteSpreadKwargs` LLVM extern |
| `src/Compiler.cpp` | Call `Pyc_RouteSpreadKwargs` in dict-spread path when `**kwargs` exists |

### Verification

```python
def f(**kwargs): print(kwargs)
f(**{"p": 1, "q": 2})       # {'p': 1, 'q': 2}
f(**{})                       # {}
f(**{"a": 1}, b=2)           # {} (a matches param, b is direct kwarg)
def g(a, **kwargs): print(kwargs)
g(1, **{"x": 10, "y": 20})   # {'x': 10, 'y': 20}
```

---

## Gap 2: List Comprehensions Don't Support Multi-Variable `for a, b in pairs` Unpacking

**Status:** ✅ Fixed
**Location:** `Compiler.cpp` `lowerListComp`, `frontend/ast.h` `ListComp::Comprehension`, `ir/builder.cpp` `build_list_comp`/`build_set_comp`

### Symptom

```python
# Works (plain for loop):
for k, g in [["a", 1], ["b", 2]]:
    print(k, g)  # prints a 1, b 2

# Broken (list comprehension):
[k for k, g in [["a", 1], ["b", 2]]]  # returns [None, None] instead of ['a', 'b']
```

### Fix Applied

Changed `Comprehension.target` from `std::string` to `std::shared_ptr<Expr>` in `ast.h` for both `ListComp` and `SetComp`. Updated `lowerListComp` to check if target is Name or tuple/list pattern, calling `lowerUnpackTarget()` for non-Name targets (same pattern as `lowerDictComp`). Updated `build_list_comp` and `build_set_comp` in the IR builder to handle unpack targets with element-by-element stores.

### Verification (all passing)

```python
[k for k, g in [["a", 1], ["b", 2]]]           # ['a', 'b']
[k for k, v, w in [["a", 1, 2], ["b", 3, 4]]]  # ['a', 'b']
[[a, b] for a, b in pairs]                      # [['a', 1], ['b', 2]]
# Dict comprehensions already work — verify no regression
{k: v for k, v in pairs}                        # still works
```

### Note on the stale-build episode

The fix in commit `86e9fd5` was correct but appeared broken for an extended
period because the `build/pyc` binary was stale (last built before the fix)
and could not be rebuilt: `libmpdec-dev` (a CMake hard dependency for
`decimal.Decimal`) had silently gone missing from the build environment,
causing CMake reconfigure to fail and `make` to exit before compiling. The
test runner passed (`make check` swallows CASES failures) so the regression
went unnoticed. Re-installing `libmpdec-dev` and rebuilding produced a binary
in which Gap 2 works as designed. Lesson: a green `make check` does not
prove the binary is fresh — verify the build actually recompiled the
changed source before trusting runner output.

### Related remaining gaps (not Gap 2)

These surface in the same test cases but are distinct, pre-existing
limitations:

- ~~**Set comprehensions are not implemented at all.**~~ **Now working**
  — `{x*x for x in [1,2,3,2]}` correctly produces `{1, 4, 9}`. The
  `lowerSetComp` handler in `Compiler.cpp` is implemented and functional.
  Also fixed: nested set comprehension iterators are now evaluated lazily
  inside the parent body (same fix as `lowerListComp`), so nested set
  comprehensions with outer-variable-dependent iterables work correctly.
- ~~**Dict iteration order is not insertion-preserved**~~ **Now insertion-ordered**
  (CPython 3.7+). Dict payload is a `std::vector` of pairs, not
  `unordered_map`. See IMPLEMENTATION.md and ISSUES I-106.
- **Nested-comp subscript-on-iteration-variable** produces float-promoted
  / tuple-collapsed values — a compound of pre-existing type-tracking
  quirks, not the unpack-target mechanism. Tracked as ISSUES I-005.

---

## Gap 3: `re.match` Not Anchored

**Status:** ✅ Fixed
**Location:** `Compiler.cpp` `lowerMethodCall` (~line 7746), `Runtime.cpp` `PyBuiltin_ReMatch`

### Symptom

```python
import re
re.match("b", "abc")   # returns match object (wrong!)
# CPython: returns None (match must be at start of string)
```

### Root Cause

`re.match` is routed to the same `PyBuiltin_ReSearch` function as `re.search`. The `pcre2_match` call passes `0` as the start offset and `0` as flags — no anchoring. Real `re.match` requires the pattern to match at the very start of the string.

### Implementation Approach

Add a separate `PyBuiltin_ReMatch` runtime function that compiles with `PCRE2_ANCHORED` flag:

**A. Runtime.cpp** — Add `PyBuiltin_ReMatch`:
- Same as `PyBuiltin_ReSearch` but passes `PCRE2_ANCHORED` to `pcre2_compile`
- Or: add an `anchored` boolean parameter to `PyBuiltin_ReSearch` (cleaner, less code duplication)

**B. Compiler.cpp** — Route `re.match` to the anchored variant:
- In `lowerMethodCall`, separate `match` from `search`/`finditer`/etc.
- Call `PyBuiltin_ReMatch` (or `PyBuiltin_ReSearch` with `anchored=1`)

### Files to Change

| File | Change |
|------|--------|
| `src/runtime/Runtime.cpp` | Add `PyBuiltin_ReMatch` or modify `PyBuiltin_ReSearch` to accept `anchored` param |
| `include/pyc/runtime.h` | Declare new/modified function |
| `src/codegen/Codegen.cpp` | Declare `PyBuiltin_ReMatch` LLVM extern |
| `src/Compiler.cpp` | Route `re.match` to `PyBuiltin_ReMatch` instead of `PyBuiltin_ReSearch` |

### Verification

```python
import re
re.match("b", "abc")       # None (correct — not at start)
re.match("a", "abc")       # match object (correct — at start)
re.search("b", "abc")      # match object (correct — anywhere)
re.match("b", "bc")        # match object (correct — at start)
```

---

## Gap 4: Variable Name Collision `t<N>`/`c<N>` with Compiler Temp Namespace

**Status:** ✅ Fixed (by `$` prefix on all compiler temps)
**Location:** `Compiler.cpp` temp naming scheme

### Fix Applied

All compiler internal temps are prefixed with `$` (e.g., `$t0`, `$c0`, `$t123`). Since `$` is not a valid character in Python identifiers, user variable names like `t0`, `c0`, `c17` can never collide with compiler temps. The `valueMap` uses exact string key lookup, so `"t0"` never equals `"$t0"`.

### Verification (all passing)

```python
c0 = 'hello'           # 'hello'
t0 = 100; c0 = 5; i0 = [1,2,3]  # 100 200 5 [1, 2, 3] 3.14
def f(t2, c3): return t2 + c3   # f(10, 20) → 30
for i in range(3): t3 = i*2     # 0 2 4
class Foo: self.c5 = 'attr'     # attr 99
```

### Remaining Temp Naming Patterns (no collision risk)

| Pattern | Risk | Reason |
|---------|------|--------|
| `$t<N>`, `$c<N>` | None | `$` prefix, invalid in Python |
| `__sl_<N>` | None | Double underscore prefix |
| `__yfrom_subgen_<N>` | None | Unique prefix |
| `<name>_cell` | None | Suffix, not full name |

---

## Priority Order

1. ~~**Gap 3 (`re.match` anchoring)**~~ — ✅ Fixed
2. ~~**Gap 1 (`**dict` → `**kwargs` routing)**~~ — ✅ Fixed
3. ~~**Gap 2 (list comp unpacking)**~~ — ✅ Fixed
4. ~~**Gap 4 (variable name collision)**~~ — ✅ Fixed
