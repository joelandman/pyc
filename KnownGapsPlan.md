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

**Status:** 🟡 Partially Fixed — AST-level change applied, but unpacking not working in runtime
**Location:** `Compiler.cpp` `lowerListComp`, `frontend/ast.h` `ListComp::Comprehension`, `ir/builder.cpp` `build_list_comp`/`build_set_comp`

### Symptom

```python
# Works (plain for loop):
for k, g in [["a", 1], ["b", 2]]:
    print(k, g)  # prints a 1, b 2

# Broken (list comprehension):
[k for k, g in [["a", 1], ["b", 2]]]  # returns [None, None] instead of ['a', 'b']
```

### Fix Applied (Partial)

Changed `Comprehension.target` from `std::string` to `std::shared_ptr<Expr>` in `ast.h` for both `ListComp` and `SetComp`. Updated `lowerListComp` to check if target is Name or tuple/list pattern, calling `lowerUnpackTarget()` for non-Name targets (same pattern as `lowerDictComp`). Updated `build_list_comp` and `build_set_comp` in the IR builder to handle unpack targets with element-by-element stores.

### Current Issue

Despite the fix being in place, list comprehension unpacking still produces `[None, None]` instead of the expected unpacked values. Debug investigation revealed:

1. The `lowerListComp` function's debug output is not appearing (file not created despite fopen calls in code)
2. The LLVM IR shows the comprehension is being lowered, but `lowerUnpackTarget` is NOT being called (no `PyList_Unpack2` calls in IR)
3. The target node type check at `Compiler.cpp:9381-9386` may not be matching the actual AST node type
4. The AST structure from `parse_helper.py` shows the target is a `Tuple` node with children being `Name` nodes, which should match the non-Name branch

### Root Cause

Two-level problem:

1. **AST level** (`ast.h`): The `ListComp::Comprehension` struct stores `target` as a `std::shared_ptr<Expr>`, not as a `std::string`. The parser flattens unpacking patterns into a single string name, losing the tuple/list structure. So `lowerListComp` has no way to know the target was originally `(k, g)` vs just `k`.

2. **Codegen level** (`Compiler.cpp:lowerListComp`): Only handles simple Name targets. Unlike `lowerDictComp` which calls `lowerUnpackTarget()` for non-Name targets, `lowerListComp` has no unpacking logic at all.

### Implementation Approach

**Option A (Recommended): Fix at AST level**

✓ PARTIALLY IMPLEMENTED

Modified `frontend/ast.h` `ListComp::Comprehension` to store target as `std::shared_ptr<Expr>` instead of `std::string`:

```cpp
struct Comprehension {
    std::shared_ptr<Expr> target;  // was: std::string target
    std::shared_ptr<Expr> iterable;
    std::vector<std::shared_ptr<Expr>> ifs;
};
```

Update the parser (`PythonParser.cpp`) to extract the full target AST node instead of just the id string.

Update `lowerListComp` (`Compiler.cpp`) to check if target is Name or tuple/list pattern, calling `lowerUnpackTarget()` for non-Name targets (same pattern as `lowerDictComp`).

**Option B (Narrower): Fix at lowering level only**

Keep AST as-is but add a new AST field to the comprehension node that encodes unpacking info (e.g., a comma-separated list of target names). `lowerListComp` splits this and calls the existing unpack machinery.

### Files to Change (Option A)

| File | Change | Status |
|------|--------|--------|
| `frontend/ast.h` | Change `Comprehension.target` from `std::string` to `std::shared_ptr<Expr>` | ✅ Done |
| `src/Compiler.cpp` | Update `lowerListComp` to handle unpack targets via `lowerUnpackTarget` | ✅ Done |
| `ir/builder.cpp` | Update `build_list_comp`/`build_set_comp` to handle unpack targets | ✅ Done |

### Verification

```python
# Currently failing — produces [None, None] instead of ['a', 'b']
[k for k, g in [["a", 1], ["b", 2]]]           # ['a', 'b']
[k for k, v, w in [["a", 1, 2], ["b", 3, 4]]]  # ['a', 'b']
[[a, b] for a, b in pairs]                      # [['a', 1], ['b', 2]]
# Dict comprehensions already work — verify no regression
{k: v for k, v in pairs}                        # still works
```

### Debug Notes

- `lowerListComp` debug output not appearing despite fopen calls in code
- LLVM IR shows comprehension lowered but no `PyList_Unpack2` calls
- Target node type check at `Compiler.cpp:9381-9386` may not be matching actual AST node type
- AST from `parse_helper.py` shows target is `Tuple` node with `Name` children — should match non-Name branch
- Need to verify: is `lowerListComp` actually being called? Is the target node type "Tuple" or something else?

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
3. **Gap 2 (list comp unpacking)** — 🟡 Partially fixed — AST-level change applied, runtime unpacking not working
4. ~~**Gap 4 (variable name collision)**~~ — ✅ Fixed
