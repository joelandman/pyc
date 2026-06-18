# pyc — Memory Leak Remediation Plan

## Executive Summary

The pyc compiler generates LLVM IR that calls a C runtime with manual reference counting, but **99.8%+ of allocated PyObject\* objects are never freed**. For `fibn.py` with n=20: 98,522 allocs, 7 frees. For `nbody.py` with 100k steps: 58.6M allocs, 100K frees.

The root cause is a **fundamental architectural flaw** in how the code generator tracks value ownership.

---

## Root Cause

### The `valueMap` Blind Spot

```cpp
std::unordered_map<std::string, llvm::Value*> valueMap;
```

`valueMap` maps variable names to LLVM IR values **without tracking ownership**. When a value is created (e.g., `PyInt_FromLong(1)`), it returns a `PyObject*` with refcount=1 — the caller **owns** this reference. When that value is later consumed as an argument to another function (e.g., `PyNumber_Subtract`), it is treated as a **borrowed** reference. But the ownership flag is never cleared, so the reference is never DECREF'd.

### The INCREF Bug

```cpp
// Lines 979-988 in Codegen.cpp (assign handler):
builder.CreateStore(newVal, alloca);      // newVal has refcount=1
// ... later ...
builder.CreateCall(incref, {newVal});     // newVal now has refcount=2
```

Every newly-created value (already refcount=1) gets INCREF'd to refcount=2. Since the DECREF logic only fires on reassignment (bringing refcount back to 1), and never to 0, these values are **immortalized for the program's lifetime**.

### The Missing DECREFs

For a value to be freed, its refcount must reach 0. But:

1. **Constants** (`PyInt_FromLong(1)`) are created, stored in valueMap, consumed once, and abandoned
2. **Binary ops** (`PyNumber_Add`) return new refs that are stored and never DECREF'd
3. **Function call results** are stored and never DECREF'd
4. **Bool results** (`PyBool_New`) are stored and never DECREF'd
5. **Global reassignments** overwrite without releasing the old reference

---

## Evidence

```
fibn.py n=20: 98,522 allocs, 7 frees, 14.2 MB leaked (99.993% leak rate)
nbody.py 100k: 58.6M allocs, 100K frees, 8.4 GB leaked (99.83% leak rate)
```

Valgrind output confirms these are real leaks (not just "still reachable"):
```
==614635==     in use at exit: 14,185,864 bytes in 98,515 blocks
==614635==   total heap usage: 98,522 allocs, 7 frees, 14,264,280 bytes allocated
==614635== LEAK SUMMARY:
==614635== ERROR SUMMARY: 5627 errors from 5627 contexts (suppressed: 0 from 0)
```

---

## Reference Counting Contract

### Functions that return NEW references (caller must DECREF):

| Function | Returns |
|----------|---------|
| `PyInt_FromLong` | new ref (refcount=1) |
| `PyFloat_FromDouble` | new ref (refcount=1) |
| `PyBool_New` | new ref (refcount=1) |
| `PyUnicode_FromString` | new ref |
| `PyNumber_Add` | new ref |
| `PyNumber_Subtract` | new ref |
| `PyNumber_Multiply` | new ref |
| `PyNumber_Divide` | new ref |
| `PyNumber_TrueDivide` | new ref |
| `PyNumber_Remainder` | new ref |
| `PyNumber_Pow` | new ref |
| `PyDict_New` | new ref |
| `PyList_New` | new ref |
| `PyList_NewBoxed` | new ref |
| `PyList_GetItem` | **borrowed** ref |
| `PyList_GetItemObj` | **borrowed** ref |
| `Pyc_GetItem` | new ref |
| `PyDict_GetItem` | new ref (after fix) |
| `PyStr_FromAny` | new ref |
| `PyString_Concat` | new ref |
| `PyObject_Not` | new ref |
| `PyObject_TruthBoxed` | new ref |
| `PyCell_Get` | new ref |
| `PyCell_New` | new ref |
| `PyBuiltin_Len` | new ref |
| `PyBuiltin_PrintNewline` | new ref (should be void) |

### Functions that take BORROWED references (caller owns the object):

| Function | Notes |
|----------|-------|
| `PyDict_SetItem` | value param is borrowed |
| `PyList_SetItem` | item param is borrowed |
| `PyList_Append` | object param is borrowed |
| `PyNumber_Add` | both args are borrowed |
| `PyObject_CompareBool` | both args are borrowed |
| `PyObject_Print` | obj param is borrowed |

---

## Leak Sources (Detailed)

### L1: `const` instruction (HIGH)

**File:** `Codegen.cpp:707-742`

**Pattern:** Every `const` instruction creates a new boxed value but never DECREFs it.

```cpp
// Line 718-724: integer constants
long v = std::stol(val);
llvm::Value* boxed = builder.CreateCall(fromLong, {...}, inst.result);
valueMap[inst.result] = boxed;    // <-- NEVER DECREF'd

// Line 736-742: float constants
double v = std::stod(...);
llvm::Value* boxed = builder.CreateCall(fromDouble, {...}, inst.result);
valueMap[inst.result] = boxed;    // <-- NEVER DECREF'd

// Line 726-734: bool constants
llvm::Value* boxed = builder.CreateCall(boolNew, {...}, inst.result);
valueMap[inst.result] = boxed;    // <-- NEVER DECREF'd

// Line 709-716: string constants
llvm::Value* boxed = builder.CreateCall(fromStr, {...}, inst.result);
valueMap[inst.result] = boxed;    // <-- NEVER DECREF'd
```

**Evidence from fib.ll:**
```llvm
%c1 = call ptr @PyInt_FromLong(i64 1)          ; leaked
%c3 = call ptr @PyInt_FromLong(i64 1)          ; leaked
%c6 = call ptr @PyInt_FromLong(i64 2)          ; leaked
```

**Impact:** Every constant in every program leaks. In `fib(10)`: ~700 leaked objects from constants alone.

---

### L2: Binary operations (HIGH)

**File:** `Codegen.cpp:743-839`

**Pattern:** Every binary arithmetic operation creates a new boxed value but never DECREFs it.

```cpp
// Line 749: add
llvm::Value* sum = builder.CreateCall(numberAdd, {lhs, rhs}, inst.result);
valueMap[inst.result] = sum;   // <-- NEVER DECREF'd

// Line 760: sub
llvm::Value* diff = builder.CreateCall(numberSub, {lhs, rhs}, inst.result);
valueMap[inst.result] = diff;   // <-- NEVER DECREF'd

// Line 795: pow
valueMap[inst.result] = builder.CreateCall(fn, {lhs, rhs}, inst.result);   // <-- NEVER DECREF'd
```

**Evidence from fib.ll:**
```llvm
%t4 = call ptr @PyNumber_Subtract(ptr %n.load2, ptr %c3)   ; leaked
%t5 = call ptr @fib(ptr %t4)                                ; t4 passed as borrowed, never DECREF'd -> LEAK
```

**Impact:** Every arithmetic op leaks its result. In `fib(10)`: ~530 leaked objects from operations alone.

---

### L3: INCREF-after-store doubles refcount (HIGH)

**File:** `Codegen.cpp:979-988`

**Pattern:** After storing a value to an alloca, the code unconditionally INCREFs it, even if the value was already owned.

```cpp
builder.CreateStore(newVal, alloca);      // newVal has refcount=1 (e.g., from PyInt_FromLong)
// ... later ...
builder.CreateCall(incref, {newVal});     // newVal now has refcount=2
builder.CreateStore(newVal, valueMap[inst.result]);  // redundant store
```

**Impact:** Every variable assignment doubles refcount. Objects become immortal (refcount never reaches 0).

---

### L4: Global reassignment (HIGH)

**File:** `Codegen.cpp:944-958`

**Pattern:** When a module global is reassigned, the old value is never DECREF'd.

```cpp
if (llvm::GlobalVariable* gv = llvm::dyn_cast<...>(valueMap[inst.result])) {
    // Module global: never DECREF (the module owns this ref)
    builder.CreateStore(newVal, gv);   // <-- OLD VALUE LEAKED!
}
```

**Impact:** Every loop iteration that updates a global leaks one object. In `nbody.py`: one leak per step.

---

### L5: Call arguments and results (HIGH)

**File:** `Codegen.cpp:989-1013`

**Pattern:** Function call arguments passed as borrowed refs are never DECREF'd. Return values stored in valueMap are never DECREF'd.

```cpp
for (size_t i = 1; i < inst.operands.size(); ++i) {
    callArgs.push_back(getAsPyObject(inst.operands[i].name));  // borrowed, never DECREF'd
}
llvm::Value* callRes = builder.CreateCall(callee, callArgs, inst.result);
valueMap[inst.result] = callRes;   // <-- result never DECREF'd
```

**Impact:** Every function call leaks its arguments if those arguments were created with `const`, binary ops, etc.

---

### L6: `icmp` instruction (HIGH)

**File:** `Codegen.cpp:640-667`

**Pattern:** Comparison operations create a boxed bool result that is stored in valueMap but never DECREF'd.

```cpp
boxedCmp = builder.CreateCall(boolNew, {cmpResult}, inst.result);  // new ref
valueMap[inst.result] = boxedCmp;   // <-- NEVER DECREF'd
```

**Evidence from fib.ll:**
```llvm
%t2 = call ptr @PyBool_New(i32 %0)       ; leaked
```

**Impact:** Every comparison leaks a bool object. Used in every `if`, `while`, and `br` condition.

---

### L7: Default return value (MEDIUM)

**File:** `Codegen.cpp:1029-1038`

**Pattern:** Functions without explicit return paths create a new `PyInt_FromLong(0)` that is never DECREF'd.

```cpp
llvm::Value* zero = builder.CreateCall(fromLong, {0});  // new ref, never DECREF'd
builder.CreateRet(zero);
```

**Impact:** Every function exit leaks one boxed int.

---

### L8: `PyBuiltin_PrintNewline` (MEDIUM)

**File:** `Runtime.cpp:523-526`

**Pattern:** Returns a new ref that callers never DECREF.

```cpp
PyObject* PyBuiltin_PrintNewline(void) {
    printf("\n");
    return PyInt_FromLong(0);    // new ref, never DECREF'd by caller
}
```

**Impact:** Every print statement leaks a boxed int.

---

### L9: Multi-arg print (MEDIUM)

**File:** `Compiler.cpp:1499-1517`

**Pattern:** Intermediate `PyStr_FromAny` and `PyString_Concat` results in multi-arg print are never DECREF'd.

**Impact:** Only affects `print(a, b, c)` with 2+ arguments.

---

### L10: `PyObject_Not`/`TruthBoxed` (MEDIUM)

**File:** `Compiler.cpp:1965-1966`

**Pattern:** New ref from boolean negation/truth check is never DECREF'd.

**Impact:** Affects `not in` and `not` unary ops.

---

### L11: `PyCell_Get` (LOW)

**File:** `Compiler.cpp:545-551`

**Pattern:** New reference from cell read is never DECREF'd.

**Impact:** Only affects nonlocal/closure variables.

---

## Remediation Strategy

### Phase 1: Owned vs. Borrowed Tracking (3 days — highest impact)

Replace the blind `valueMap` approach with ownership tracking.

**Step 1: Add ownership tracking**

```cpp
std::unordered_map<std::string, llvm::Value*> valueMap;
std::unordered_set<std::string> ownedTemps;  // <-- NEW
```

**Step 2: Mark values as owned when created**

```cpp
// In const instruction (line 707):
if (!val.empty() && (val[0] == '"')) {
    llvm::Value* boxed = builder.CreateCall(fromStr, {...});
    valueMap[inst.result] = boxed;
    ownedTemps.insert(inst.result);    // <-- NEW
} else {
    llvm::Value* boxed = builder.CreateCall(fromLong, {...});
    valueMap[inst.result] = boxed;
    ownedTemps.insert(inst.result);    // <-- NEW
}

// In binary ops (line 743):
llvm::Value* sum = builder.CreateCall(numberAdd, {lhs, rhs});
valueMap[inst.result] = sum;
ownedTemps.insert(inst.result);        // <-- NEW

// In icmp (line 664):
boxedCmp = builder.CreateCall(boolNew, {cmpResult});
valueMap[inst.result] = boxedCmp;
ownedTemps.insert(inst.result);        // <-- NEW

// In call results (line 1010):
valueMap[inst.result] = callRes;
ownedTemps.insert(inst.result);        // <-- NEW
```

**Step 3: DECREF owned temps after borrowed use**

When an owned temp is consumed as a borrowed reference (passed to a function, used in an arithmetic op, used in a comparison), emit DECREF **after** the use:

```cpp
// Helper to emit DECREF if owned
auto decRefIfOwned = [&](const std::string& name) {
    if (ownedTemps.count(name)) {
        ownedTemps.erase(name);
        llvm::Function* decref = module->getFunction("Py_DECREF");
        if (decref) {
            llvm::Value* val = getOrLoad(name);
            builder.CreateCall(decref, {val});
        }
    }
};
```

**Step 4: Fix the assign handler**

```cpp
// In assign handler (line 938):
if (ownedTemps.find(inst.operands[0].name) != ownedTemps.end()) {
    // Source is an owned temp — use it as borrowed, then DECREF
    decRefIfOwned(inst.operands[0].name);
} else {
    // Source is borrowed (loaded from alloca/param/global) — INCREF it
    builder.CreateCall(incref, {getAsPyObject(inst.operands[0].name)});
}
```

### Phase 2: Global Reassignment Fix (0.5 days)

```cpp
// In assign handler (line 944):
if (llvm::GlobalVariable* gv = llvm::dyn_cast<...>(valueMap[inst.result])) {
    builder.CreateCall(decref, {oldVal});  // Release old ref
    builder.CreateStore(newVal, gv);
}
```

### Phase 3: Print/PrintNewline Fixes (0.5 days)

1. Change `PyBuiltin_PrintNewline` to return `void`
2. In multi-arg print, DECREF intermediate accumulators

### Phase 4: Minor Fixes (1 day)

1. `PyObject_Not`/`TruthBoxed`: DECREF after use
2. `PyCell_Get`: DECREF after borrowed use
3. Default return: use shared sentinel

---

## Success Criteria

| Metric | Before | Target |
|--------|--------|--------|
| `fibn.py` n=20 allocs | 98,522 | <1,000 |
| `fibn.py` n=20 frees | 7 | >97,000 |
| `nbody.py` 100k allocs | 58.6M | <100,000 |
| `nbody.py` 100k frees | 100K | >58M |
| Valgrind "definitely lost" blocks | 98,515 | <100 |

---

## Implementation Notes

### Key Principle

**Every new reference must be DECREF'd exactly once.** The challenge is determining **when** to DECREF. The answer depends on how the reference is consumed:

- **As an owned value** (stored in a variable): DECREF when the variable is reassigned or the function exits
- **As a borrowed argument**: DECREF after the call returns
- **As a condition** (branch target): DECREF after the branch decision
- **As a return value**: Keep ownership (caller must DECREF)

### Testing Strategy

1. Run `fibn.py` with valgrind — verify <100 leaked objects
2. Run `nbody.py` with valgrind — verify <100K leaked objects
3. Run full test suite — verify no output regressions
4. Run `make valgrind-test` — verify all tests pass memory check

### Risks

1. **Double DECREF**: If a temp is used multiple times as borrowed, we must only DECREF once. Solution: remove from `ownedTemps` after first DECREF.
2. **Return value ownership**: If a temp is used as a return value, we must NOT DECREF it before the return. Solution: track which temps are used as returns.
3. **Loop variables**: A temp used in a loop iteration may need to be DECREF'd after each iteration but not after the last. Solution: the `assign` handler's DECREF on reassignment covers this.
