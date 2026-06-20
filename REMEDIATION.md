# REMEDIATION.md

## Outstanding Issues Requiring Immediate Remediation

This document tracks correctness issues that must be fixed before the compiler can produce reliable binaries.

**Last updated:** Full rewrite after Steps 5-9 completion

---

### Issue 1: `for` Loop Iteration Is a Stub

**Severity:** Critical  
**Location:** `ir/builder.cpp:221-275`, `codegen/ir2ll.cpp:374-383`  
**Status:** OPEN

**Problem:** `build_for_stmt()` creates loop structure but uses `CALL "next"` as a stub (line 248). The `INTRINSIC_RANGE` instruction returns an empty list instead of a proper range object. This means `for x in range(5):` produces no iterations.

**Impact:** No `for` loops work at all. Programs with any for loop produce wrong results or crash.

**Fix:**
- Implement iterator protocol: `__iter__` returns index tracker, `__next__` returns element
- Update `build_for_stmt()` to call `__iter__` on the iterable, then `__next__` in loop
- Implement `pyc_range_list(n)` in runtime to create list [0, 1, ..., n-1]
- Handle StopIteration to exit loop (branch to merge block)

---

### Issue 2: `finalize()` Does Not Clean Up Registry Objects

**Severity:** Critical  
**Location:** `runtime/object.cpp:156-161`  
**Status:** OPEN

**Problem:** `PyObjectFactory::finalize()` only deletes singleton objects. All objects registered with `PyObjectRegistry` (from `create_int()`, `create_str()`, `create_list()`, etc.) are leaked. Additionally, objects created in `libpyc_runtime.cpp` via direct `new PyObject()` bypass the factory entirely and are never freed.

**Impact:** Every run leaks all dynamically allocated objects. Memory grows unbounded for programs that create objects.

**Fix:**
- Call `registry.cleanup()` in `PyObjectFactory::finalize()` to free all registered objects
- Route `pyc_codegen_new_object()` and `pyc_new_type()` through `PyObjectFactory::create_*()` methods
- Add `pyc_destroy_object()` runtime function for explicit cleanup

---

### Issue 3: Small Integer Caching Returns Wrong Values

**Severity:** High  
**Location:** `runtime/object.cpp:57-72`  
**Status:** OPEN

**Problem:** All small integers (-1, 0, 1) return the same TYPE_INT singleton (which stores value 0). `create_int(42)` creates a new object every time with no caching. `create_int(-1)` returns the same object as `create_int(0)`.

**Impact:** `a = -1; b = 0; a == b` returns True (wrong). Any program using small integer equality is broken.

**Fix:**
- Create separate singletons for each cached value: `singletons_[TYPE_INT]` for 0, `singletons_[TYPE_INT_MINUS_1]` for -1, etc.
- Or use a map: `std::unordered_map<int64_t, PyObject*> small_int_cache_` for cached values
- Standard Python caches -5 to 256; start with -1 to 3 for minimal fix

---

### Issue 4: `raise` Statement Does Not Propagate Exceptions

**Severity:** High  
**Location:** `ir/builder.cpp:418-429`, `runtime/builtins.cpp`  
**Status:** OPEN

**Problem:** `build_raise_stmt()` just returns 0 instead of raising an exception. There is no runtime exception propagation mechanism. `try/except` blocks are created in IR but there is no way to detect exceptions at runtime.

**Impact:** `raise` silently does nothing. `try/except` always executes the try body and never the except body.

**Fix:**
- Add `pyc_raise_exception(obj)` and `pyc_get_exception()` to runtime
- Add thread-local exception storage in interpreter
- Update `handle_call()` to check for exception after each call
- Update LLVM codegen: `raise` emits CALL `pyc_raise_exception`, check exception in try/except blocks
- Implement `StopIteration` exception for `for` loop termination

---

### Issue 5: Objects Created in Runtime Library Are Not Registered

**Severity:** High  
**Location:** `runtime/libpyc_runtime.cpp:78-114`  
**Status:** OPEN

**Problem:** `pyc_codegen_new_object()`, `pyc_new_type()`, `pyc_type_name()`, and other runtime functions create `PyObject*` via direct `new PyObject()` without registering with `PyObjectRegistry`. These objects are never freed.

**Impact:** Every call to `type(x)`, `isinstance(x, int)`, or `NEWOBJ` instruction leaks memory. Programs that use type checking or object creation leak on every call.

**Fix:**
- Call `PyObjectFactory::register_object(obj)` after each `new PyObject()` in runtime functions
- Or better: route all object creation through `PyObjectFactory::create_*()` methods
- Add `pyc_ref_dec()` calls where appropriate (e.g., after `pyc_type_name()` returns)

---

### Issue 6: `INTRINSIC_RANGE` Returns Empty List

**Severity:** High  
**Location:** `codegen/ir2ll.cpp:374-383`  
**Status:** OPEN

**Problem:** `INTRINSIC_RANGE` creates an empty list instead of a range object with values [0, 1, ..., n-1]. The implementation just calls `pyc_new_list()` without populating it.

**Impact:** `range(5)` returns `[]`, so `for x in range(5):` never executes.

**Fix:**
- Implement `pyc_range_list(n)` that creates a list and fills it with 0..n-1
- Call it from `INTRINSIC_RANGE` case in ir2ll.cpp
- Alternative: implement iterator protocol with `__iter__`/`__next__` methods

---

### Issue 7: `SETATTR` Attribute Name Is Null Pointer

**Severity:** High  
**Location:** `codegen/ir2ll.cpp:442-455`  
**Status:** OPEN

**Problem:** `SETATTR` case loads the attribute name as a local variable but passes it as a raw pointer to `pyc_setattr()` without loading the actual string bytes. The `name_ptr` is a stack pointer, not a string pointer.

**Impact:** `setattr(obj, "x", val)` passes a garbage pointer as attribute name, causing crashes or silent failures.

**Fix:**
- Load the string pointer from `LOADCONST_STR` result (which returns a GlobalVariable pointer)
- Pass the GlobalVariable pointer directly to `pyc_setattr()`
- Same fix needed for `GETATTR`/`LOAD_ATTR` cases

---

### Issue 8: Interpreter `handle_call()` Does Not Handle Dynamic Function Objects

**Severity:** High  
**Location:** `ir/interpreter.cpp:516-533`  
**Status:** PARTIALLY FIXED

**Problem:** `handle_call()` looks up functions in `func_map_` by name. It does not handle calling function objects stored in variables (e.g., `f = lambda x: x; f(1)`).

**Impact:** Lambda calls, higher-order functions (map, filter), and function-as-value patterns don't work.

**Fix:**
- Check if the function operand is a function object (has `func_callable`)
- If yes, invoke `(*func_obj->func_callable)(self, args)` directly
- If no, look up in `func_map_` by name (existing path)

---

### Issue 9: No Comprehension Support

**Severity:** Medium  
**Location:** `ir/builder.cpp`, `frontend/lark_parser.cpp`  
**Status:** OPEN

**Problem:** `ListComp`, `SetComp`, `GenExpr`, `DictComp` AST nodes exist but have no IR builder handlers. The lark parser does not build these nodes from JSON. Comprehensions like `[x*2 for x in range(10)]` produce no output.

**Impact:** No list/set/dict comprehensions work. Programs using comprehensions silently drop them.

**Fix:**
- Add `build_list_comp()` to IR builder: translate to `result = []; [result.append(x) for x in iter]`
- Add comprehension node handling in `lark_parser.cpp::build_ast_node()`
- Implement in interpreter via existing loop infrastructure
- Implement in LLVM codegen via existing list operations

---

### Issue 10: Lambda Expression Support Missing

**Severity:** Medium  
**Location:** `ir/builder.cpp`, `frontend/lark_parser.cpp`  
**Status:** OPEN

**Problem:** `LambdaExpr` AST node exists but has no IR builder handler. The lark parser does not build lambda nodes from JSON. Expressions like `lambda x: x + 1` produce no output.

**Impact:** No lambda expressions work. Higher-order patterns like `map(lambda x: x*2, lst)` are broken.

**Fix:**
- Add `build_lambda_expr()` to IR builder: create function with unique name, store callable
- Add lambda node handling in `lark_parser.cpp::build_ast_node()`
- Capture free variables from enclosing scope into function globals
- Store function object via `create_function()` with `std::function` callable

---

### Issue 11: `import` System Not Implemented

**Severity:** Medium  
**Location:** `ir/builder.cpp:353-361`, `runtime/builtins.cpp:609-612`  
**Status:** OPEN

**Problem:** `build_import_stmt()` is a stub that does nothing. The `import` builtin is a stub returning empty dict. No file-based module loading exists.

**Impact:** `import os`, `import math`, or any import statement silently does nothing.

**Fix:**
- Implement file-based module loading: read .py file, parse, build IR, compile
- Create `PyModule` type with namespace dict for module globals
- Handle `import foo` → load `foo.py`, bind to global `foo`
- Handle `from foo import bar` → copy `bar` from module namespace
- Stub: raise `NotImplementedError` for non-builtin imports

---

### Issue 12: `format()` Builtin Is a Stub

**Severity:** Low  
**Location:** `runtime/builtins.cpp:928-931`  
**Status:** OPEN

**Problem:** `format()` is implemented as an alias to `str()`. It does not handle format specifiers like `"{:.2f}".format(3.14159)`.

**Impact:** f-string-like formatting doesn't work. Programs using `format()` with specifiers get wrong output.

**Fix:**
- Parse format specifier string (e.g., ".2f", "d", "s")
- For numeric types, use `std::stringstream` with appropriate precision/flags
- For string types, handle alignment and width specifiers
- Expected: 2-3x slower than CPython format (no pre-compiled format strings)

---

### Issue 13: `dir()` and `globals()`/`locals()` Are Stubs

**Severity:** Low  
**Location:** `runtime/builtins.cpp:545-599`  
**Status:** OPEN

**Problem:** `dir()` returns empty list. `globals()` and `locals()` return empty dict. These are introspection functions needed for debugging and dynamic code.

**Impact:** Debugging tools and introspection patterns don't work.

**Fix:**
- `dir(obj)`: iterate `instance_attrs` keys for instances, return type method names for types
- `globals()`: return reference to interpreter's global_vars_ map
- `locals()`: return reference to current frame's local_vars_ map

---

### Issue 14: `exec()` and `eval()` Are Stubs

**Severity:** Low  
**Location:** `runtime/builtins.cpp:601-607`  
**Status:** OPEN

**Problem:** `exec()` and `eval()` return 0 without executing code. These are needed for dynamic code execution.

**Impact:** Dynamic code patterns don't work.

**Fix:**
- `eval(code_string)`: parse code_string, build AST, execute in current scope, return result
- `exec(code_string)`: parse code_string, build AST, execute in current scope, return None
- Security: no sandboxing in initial implementation
- Expected: significant complexity increase, defer if time-constrained

---

### Issue 15: Missing `gc.h` Header File

**Severity:** Medium  
**Location:** `runtime/` directory  
**Status:** OPEN

**Problem:** `gc.cpp` includes `runtime/gc.h` but no such file exists. The GC class is defined in `runtime/object.h`. This compiles because `object.h` is included transitively, but it is a structural error.

**Fix:**
- Either create `runtime/gc.h` with `GarbageCollector` class declaration
- Or fix `gc.cpp` to include `runtime/object.h` instead of `runtime/gc.h`

---

### Issue 16: Duplicate `ir/ir.h` Include in `main.cpp`

**Severity:** Low  
**Location:** `main.cpp:16-17`  
**Status:** OPEN

**Problem:** `#include "ir/ir.h"` appears on both lines 16 and 17. Harmless due to include guards but indicates copy-paste error.

**Fix:**
- Remove duplicate include on line 17

---

### Issue 17: `PyObjectFactory::finalize()` Memory Leak

**Severity:** Medium  
**Location:** `runtime/object.cpp:156-161`  
**Status:** OPEN

**Problem:** `finalize()` only deletes singleton objects. All dynamically allocated objects (integers, strings, lists, dicts, functions, instances) are leaked. The `PyObjectRegistry` tracks them but is never cleaned up.

**Impact:** Every program run leaks all non-singleton objects. Memory grows unbounded.

**Fix:**
- Call `registry.cleanup()` at end of `finalize()`
- This deletes all non-singleton objects tracked by the registry

---

### Issue 18: `wrap_numeric` Template Has Deduction Issue

**Severity:** Low  
**Location:** `ir/interpreter.h:158-165`  
**Status:** OPEN

**Problem:** `wrap_numeric()` template takes `PyValue` parameters but the template may have deduction issues when called with mixed int/float/string types.

**Fix:**
- Review template signature and explicit template arguments
- Add explicit template instantiation for common type combinations
- Expected: minor fix, low impact

---

## Summary of Open Issues

| # | Issue | Severity | Status |
|---|-------|----------|--------|
| 1 | `for` loop iteration is a stub | Critical | OPEN |
| 2 | `finalize()` does not clean up registry | Critical | OPEN |
| 3 | Small integer caching returns wrong values | High | OPEN |
| 4 | `raise` does not propagate exceptions | High | OPEN |
| 5 | Runtime lib objects not registered | High | OPEN |
| 6 | `INTRINSIC_RANGE` returns empty list | High | OPEN |
| 7 | `SETATTR` attribute name is null pointer | High | OPEN |
| 8 | `handle_call()` does not handle dynamic functions | High | PARTIAL |
| 9 | No comprehension support | Medium | OPEN |
| 10 | Lambda expression support missing | Medium | OPEN |
| 11 | `import` system not implemented | Medium | OPEN |
| 12 | `format()` is a stub | Low | OPEN |
| 13 | `dir()`/`globals()`/`locals()` are stubs | Low | OPEN |
| 14 | `exec()`/`eval()` are stubs | Low | OPEN |
| 15 | Missing `gc.h` header | Medium | OPEN |
| 16 | Duplicate `ir/ir.h` include | Low | OPEN |
| 17 | `finalize()` memory leak | Medium | OPEN |
| 18 | `wrap_numeric` template issue | Low | OPEN |

## Fixed in Steps 5-9

### Step 5: Remaining Language Features
- Issue 4 (old): `create_str()` stores string value via `str_value` member
- Issue 5 (old): `create_function()` stores callable via `func_callable` member
- Issue 6 (old): Duplicate function definitions removed
- Issue 7 (old): Loop/if control flow fixed with proper block structure
- Issue 8 (old): Frame ownership fixed with `unique_ptr`
- Issue 9 (old, partial): POW fixed, GETATTR/SETATTR implemented, O2 optimization enabled
- Issue 10 (old): `argv[i++]()` call-to-function bug fixed
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

### Step 8: Runtime Library (Issue 9)
- Created `runtime/libpyc_runtime.{h,cpp}` with 18 runtime functions
- Implemented INTRINSIC_TYPE, INTRINSIC_LEN, INTRINSIC_INIT, ISINSTANCE, NEWTYPE
- Runtime functions: pyc_new_list, pyc_list_get/set, pyc_type_name, pyc_len, pyc_getattr/setattr, pyc_isinstance, pyc_object_init, pyc_pow, pyc_int_from_double, pyc_print, pyc_ref_inc/dec

### Step 9: Object Model Memory Management (Issue 2)
- Created `runtime/object_registry.{h,cpp}` with PyObjectRegistry class
- Added Py_INCREF/Py_DECREF helpers to object.h
- All create_* functions register with registry
- GC Issues 1/2/3 fixed (mark_object, del_ref, roots pruning)
- Small int caching: TYPE_INT singleton for value 0 created

---

## Priority Order for Remediation

1. **Issue 1 + Issue 6** (`for` loop + range) — blocks all iteration
2. **Issue 2 + Issue 17** (finalize cleanup) — blocks reliable long-running programs
3. **Issue 3** (small int caching) — blocks correct integer equality
4. **Issue 4** (exception propagation) — blocks error handling
5. **Issue 5 + Issue 7** (runtime objects + setattr) — blocks object operations
6. **Issue 9 + Issue 10** (comprehensions + lambda) — blocks common Python patterns
7. **Issue 11** (import system) — blocks module usage
8. **Issue 15** (missing gc.h) — structural fix
9. **Issue 16** (duplicate include) — trivial fix
10. **Issue 12-14, 18** (low severity stubs) — nice-to-have
