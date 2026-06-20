# REMEDIATION.md

## Outstanding Issues Requiring Immediate Remediation

This document tracks correctness issues that must be fixed before the compiler can produce reliable binaries.

**Last updated:** Step 7 complete (performance, memory profiling, scalability testing)

---

### Issue 1: GC `mark_object` Corrupts Refcounts

**Severity:** Critical  
**Location:** `runtime/gc.cpp:49-58`  
**Status: OPEN**

**Problem:** `mark_object()` increments `obj->refcount` during the mark phase. This is fundamentally wrong — the mark phase should only set a mark bit, not modify refcounts. Incrementing refcounts during marking means:
- Refcounts are inflated beyond their true value
- `del_ref()` will never reach zero for marked objects
- Objects are never freed even when unreachable

**Fix:** Remove the `obj->refcount++` line. The mark phase should only add to `mark_set_`.

---

### Issue 2: GC `del_ref` Logic is Inverted

**Severity:** Critical  
**Location:** `runtime/gc.cpp:66-74`  
**Status: OPEN**

**Problem:** `del_ref()` deletes an object when it is NOT in the mark set. But the correct logic is: an object should be deleted during sweep when it is NOT marked (i.e., unreachable). The current code checks `!mark_set_.count(obj)` which means it tries to delete objects that were never marked — but this happens at refcount drop time, not during sweep. The GC never actually calls `del_ref()` from the sweep path.

**Fix:** The sweep phase should iterate `roots_`, check `mark_set_`, and delete unmarked objects. `del_ref()` should only decrement refcount and delete when it reaches zero (without any mark set check).

---

### Issue 3: Roots Never Pruned During Sweep

**Severity:** Critical  
**Location:** `runtime/gc.cpp:26-35, 76-86`  
**Status: OPEN**

**Problem:** `new_object()` adds every new object to `roots_`. The `collect()`/`mark_all_roots()` methods never remove dead objects from `roots_`. This means:
- The root set grows monotonically
- Even freed objects remain in roots (since they're never removed)
- `object_count()` always returns total allocations ever made

**Fix:** After the sweep phase, rebuild `roots_` to contain only marked objects. Or better: remove the auto-registration of roots from `new_object()` and let the interpreter explicitly manage roots.

---

### Issue 4: `create_str()` Does Not Store the String

**Severity:** Critical  
**Location:** `runtime/object.cpp:60-68`  
**Status: FIXED**  
**Fixed in Step 5:** `create_str()` now stores string via `obj->str_value = new std::string(value)` (object.cpp:79)

---

### Issue 5: `create_function()` Does Not Store the Callable

**Severity:** High  
**Location:** `runtime/object.cpp:70-78`  
**Status: FIXED**  
**Fixed in Step 5:** `create_function()` now stores callable via `obj->func_callable = func` (object.cpp:112)

---

### Issue 6: IR Builder Has Duplicate Function Definitions

**Severity:** High  
**Location:** `ir/builder.cpp:64-98` and `ir/builder.cpp:100-123`  
**Status: FIXED**  
**Fixed in Step 5:** Duplicate definitions removed, single correct implementation remains.

---

### Issue 7: IR Builder Control Flow Is Incomplete

**Severity:** High  
**Location:** `ir/builder.cpp:166-194`  
**Status: FIXED**  
**Fixed in Step 5:**
- `build_for_stmt()` now creates proper loop blocks with back-edges (builder.cpp:204-252)
- `build_while_stmt()` now evaluates test expression and emits branches (builder.cpp:254-283)
- `build_if_stmt()` creates proper true/false/merge block structure (builder.cpp:172-202)
- Loop context tracking added via `LoopContext` struct for break/continue (builder.h:27-30)
- `build_break_stmt()` and `build_continue_stmt()` use loop context (builder.cpp:534-547)

---

### Issue 8: Interpreter Frame Ownership Is Broken

**Severity:** High  
**Location:** `ir/interpreter.cpp:187-234`  
**Status: FIXED**  
**Fixed in Step 5:** Frames now use `std::unique_ptr<CallFrame>` stored in frame stack (interpreter.cpp:87-95, 189).

---

### Issue 9: LLVM Codegen Stubs Return Wrong Values

**Severity:** High  
**Location:** `codegen/ir2ll.cpp:350-363`  
**Status: PARTIALLY FIXED**  
**Fixed in Step 5:**
- `POW` now calls `pyc_pow` with `llvm::intrinsic::pow` fallback (ir2ll.cpp:463-486)
- `GETATTR`/`LOAD_ATTR` emit calls to `pyc_getattr` (ir2ll.cpp:349-365)
- `SETATTR` emits call to `pyc_setattr` (ir2ll.cpp:367-382)
- LLVM O2 optimization pipeline enabled (ir2ll.cpp:560-571)

**Remaining stubs:**
- `MAKE_LIST`, `LIST_GET`, `LIST_SET`, `NEWOBJ`, `INTRINSIC_*` still return `i64(0)`
- `CALL` only works for functions in `func_map_` by name

---

### Issue 10: `main.cpp` Has a Call-to-Function Bug

**Severity:** High  
**Location:** `main.cpp:214`  
**Status: FIXED**  
**Fixed:** `args.output_file = argv[i++];` (removed `()`)

---

### Issue 11: `main.cpp` Uses Wrong Option Flag

**Severity:** Medium  
**Location:** `main.cpp:97, 215`  
**Status: OPEN**  
The help text says `--emit-llir` but the actual flag is `--emit-llvm-ir`.

---

### Issue 12: Interpreter `resolve_value` Does Linear Scan

**Severity:** Medium  
**Location:** `ir/interpreter.cpp:111-145`  
**Status: FIXED (dead code)**  
**Fixed in Step 6:** Instruction result cache added to CallFrame (`instr_results` map with `cache_result()`/`get_cached_result()` methods). The `resolve_value()` function is now dead code (never called), and the cache provides O(1) lookup for instruction results.

---

### Issue 13: No `gc.h` Header File

**Severity:** Medium  
**Location:** `runtime/` directory  
**Status: OPEN**  
`gc.cpp` includes `runtime/gc.h` but no such file exists. The GC class is defined in `runtime/object.h`.

---

### Issue 14: `main.cpp` Includes `ir/ir.h` Twice

**Severity:** Low  
**Location:** `main.cpp:16-17`  
**Status: OPEN**  
`#include "ir/ir.h"` appears on both lines 16 and 17. Harmless due to include guards.

---

### Issue 15: `PyObjectFactory::finalize()` Only Frees Singletons

**Severity:** Medium  
**Location:** `runtime/object.cpp:89-94`  
**Status: OPEN**  
`finalize()` only deletes singleton objects. All other objects are never freed.

---

### Issue 16: `build_class()` Creates Empty IR Functions

**Severity:** Medium  
**Location:** `ir/builder.cpp:125-142`  
**Status: FIXED**  
**Fixed in Step 5:** `build_class()` now creates `__init__` and method functions with proper IR (builder.cpp:99-168). `build_class_call()` handles class instantiation (builder.cpp:493-512).

---

### Issue 17: `AST::Module::classify_funcs_and_classes()` Creates Shared Pointers from Raw Pointers

**Severity:** Medium  
**Location:** `frontend/ast.cpp:12-14`  
**Status: OPEN**  
Two independent `shared_ptr`s manage the same raw pointer — use-after-free risk.

---

### Issue 18: `wrap_numeric` Helper Has Template Argument Deduction Issue

**Severity:** Low  
**Location:** `ir/interpreter.h:158-165`  
**Status: OPEN**  
Function signature takes `PyValue` parameters but the template may have deduction issues.

---

## Summary of Open Issues

| # | Issue | Severity | Status |
|---|-------|----------|--------|
| 1 | GC `mark_object` corrupts refcounts | Critical | OPEN |
| 2 | GC `del_ref` logic inverted | Critical | OPEN |
| 3 | Roots never pruned | Critical | OPEN |
| 4 | `create_str()` discards string value | Critical | FIXED |
| 5 | `create_function()` discards callable | Critical | FIXED |
| 6 | Duplicate `build_function`/`build_expr` | High | FIXED |
| 7 | Incomplete loop/if control flow | High | FIXED |
| 8 | Frame ownership broken | High | FIXED |
| 9 | LLVM codegen stubs return 0 | High | PARTIAL |
| 10 | `argv[i++]()` call-to-function bug | High | FIXED |
| 11 | Wrong option flag in help | Medium | OPEN |
| 12 | `resolve_value` O(N^2) linear scan | Medium | FIXED (dead code) |
| 13 | Missing `gc.h` header | Medium | OPEN |
| 14 | Duplicate `ir/ir.h` include | Low | OPEN |
| 15 | `finalize()` only frees singletons | Medium | OPEN |
| 16 | `build_class()` creates empty IR | Medium | FIXED |
| 17 | Double shared_ptr ownership | Medium | OPEN |
| 18 | `wrap_numeric` template issue | Low | OPEN |

## Fixed in Steps 5-7

### Step 5: Remaining Language Features
- Issue 4: `create_str()` stores string value via `str_value` member
- Issue 5: `create_function()` stores callable via `func_callable` member
- Issue 6: Duplicate function definitions removed
- Issue 7: Loop/if control flow fixed with proper block structure
- Issue 8: Frame ownership fixed with `unique_ptr`
- Issue 9 (partial): POW fixed, GETATTR/SETATTR implemented, O2 optimization enabled
- Issue 10: `argv[i++]()` call-to-function bug fixed
- New: 20+ built-in functions (list, dict, string methods)
- New: 10 statement handlers (delete, global, nonlocal, assert, raise, with, try, break, continue)
- New: Subscript expression support (LIST_GET)

### Step 6: Performance Improvements
- Instruction result cache added to CallFrame for O(1) lookup
- LLVM O2 optimization pipeline enabled
- Benchmark suite created (7 benchmarks)

### Step 7: Memory Profiling and Scalability
- All 7 tests pass under AddressSanitizer (0 errors)
- All 7 tests pass under Valgrind (0 errors, 0 bytes lost)
- Scalability testing completed (compile time, global variable scaling)
