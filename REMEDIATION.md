# REMEDIATION.md

## Outstanding Issues Requiring Immediate Remediation

This document tracks correctness issues that must be fixed before the compiler can produce reliable binaries.

**Last updated:** All 18 original issues resolved. 1 intentionally unsupported.

---

### Issue 1: `for` Loop Iteration Is a Stub

**Severity:** Critical  
**Location:** `ir/builder.cpp:221-275`, `codegen/ir2ll.cpp:374-383`  
**Status: FIXED**

- `build_for_stmt()` rewritten to use index-based iteration over lists
- Added `pyc_range_list(start, stop, step)` runtime function

### Issue 2: `finalize()` Does Not Clean Up Registry Objects

**Severity:** Critical  
**Location:** `runtime/object.cpp:156-161`  
**Status: FIXED**

- `PyObjectFactory::finalize()` now calls `registry.cleanup()` to free all registered objects

### Issue 3: Small Integer Caching Returns Wrong Values

**Severity:** High  
**Location:** `runtime/object.cpp:57-72`  
**Status: FIXED**

- Separate singletons for -1, 0, 1 using `TYPE_INT + 1` and `TYPE_INT + 2` keys

### Issue 4: `raise` Statement Does Not Propagate Exceptions

**Severity:** High  
**Location:** `ir/builder.cpp:418-429`, `runtime/libpyc_runtime.cpp`  
**Status: FIXED**

- Added `pyc_raise_exception()`, `pyc_get_exception()`, `pyc_clear_exception()` runtime functions
- Thread-local `g_current_exception` stores current exception

### Issue 5: Objects Created in Runtime Library Are Not Registered

**Severity:** High  
**Location:** `runtime/libpyc_runtime.cpp:78-114`  
**Status: FIXED**

- Added `PyObjectFactory::register_object()` calls in `pyc_codegen_new_object()` and `pyc_new_type()`

### Issue 6: `INTRINSIC_RANGE` Returns Empty List

**Severity:** High  
**Location:** `codegen/ir2ll.cpp:407-430`  
**Status: FIXED**

- `INTRINSIC_RANGE` now calls `pyc_range_list()` with start, stop, step parameters

### Issue 7: `SETATTR` Attribute Name Is Null Pointer

**Severity:** High  
**Location:** `codegen/ir2ll.cpp:495-515`  
**Status: FIXED**

- `SETATTR` case creates a GlobalVariable for the attribute name string
- Same fix applied to `GETATTR`/`LOAD_ATTR` cases

### Issue 8: Interpreter `handle_call()` Does Not Handle Dynamic Function Objects

**Severity:** High  
**Location:** `ir/interpreter.cpp:516-560`  
**Status: FIXED**

- `handle_call()` now checks module functions, builtins, and global vars for dynamic calls

### Issue 9: Comprehension Support

**Severity:** Medium  
**Location:** `ir/builder.cpp`, `frontend/parser.cpp`  
**Status: FIXED**

- List, set, generator, and dict comprehensions all implemented

### Issue 10: Lambda Expression Support Missing

**Severity:** Medium  
**Location:** `ir/builder.cpp:805-851`  
**Status: FIXED**

- `build_lambda_expr()` creates new IR function with unique name

### Issue 11: `import` System Not Implemented

**Severity:** Medium  
**Location:** `ir/builder.cpp:409-468`, `runtime/import_system.cpp`  
**Status: FIXED**

- File-based module loading with packages, submodules, name binding, relative imports, namespace packages
- `sys.path` support with `set_sys_path()` / `get_sys_path()` API

### Issue 12: `format()` Builtin Is a Stub

**Severity:** Low  
**Location:** `runtime/builtins.cpp:928-1001`  
**Status: FIXED**

- Handles positional placeholders `{0}`, `{1}` and format specifiers (.2f, d, s, %)

### Issue 13: `dir()` and `globals()`/`locals()` Are Stubs

**Severity:** Low  
**Location:** `runtime/builtins.cpp:545-647`, `ir/interpreter.cpp:897-935`  
**Status: FIXED**

- `dir()` returns instance attributes, dict keys, and type methods
- `globals()`/`locals()` return interpreter state via thread-local `Interpreter::current()`

### Issue 14: `exec()` and `eval()` Are Unsupported

**Severity:** Low  
**Location:** `runtime/builtins.cpp:655-667`  
**Status: UNSUPPORTED**

**Decision:** Intentionally unsupported due to security implications (arbitrary code execution, code injection, sandbox escape). Will not be implemented. Use explicit function calls or pre-compiled IR modules instead.

### Issue 15: Missing `gc.h` Header File

**Severity:** Medium  
**Location:** `runtime/` directory  
**Status: FIXED**

- `gc.cpp` includes `runtime/object.h` (not `runtime/gc.h`). GC class is defined in `runtime/object.h`.

### Issue 16: Duplicate `ir/ir.h` Include in `main.cpp`

**Severity:** Low  
**Location:** `main.cpp:17`  
**Status: FIXED**

- Removed duplicate include

### Issue 17: `PyObjectFactory::finalize()` Memory Leak

**Severity:** Medium  
**Location:** `runtime/object.cpp:180-195`  
**Status: FIXED**

- `finalize()` now calls `registry.cleanup()` to free all registered non-singleton objects

### Issue 18: `wrap_numeric` Template Has Deduction Issue

**Severity:** Low  
**Location:** `ir/interpreter.h:174-181`  
**Status: FIXED**

- Changed `auto op` parameter to template parameter `typename Op`

---

## Summary of Open Issues

| # | Issue | Severity | Status |
|---|-------|----------|--------|
| 1 | `for` loop iteration is a stub | Critical | FIXED |
| 2 | `finalize()` does not clean up registry | Critical | FIXED |
| 3 | Small integer caching returns wrong values | High | FIXED |
| 4 | `raise` does not propagate exceptions | High | FIXED |
| 5 | Runtime lib objects not registered | High | FIXED |
| 6 | `INTRINSIC_RANGE` returns empty list | High | FIXED |
| 7 | `SETATTR` attribute name is null pointer | High | FIXED |
| 8 | `handle_call()` does not handle dynamic functions | High | FIXED |
| 9 | Comprehension support | Medium | FIXED |
| 10 | Lambda expression support missing | Medium | FIXED |
| 11 | `import` system not implemented | Medium | FIXED |
| 12 | `format()` is a stub | Low | FIXED |
| 13 | `dir()`/`globals()`/`locals()` are stubs | Low | FIXED |
| 14 | `exec()`/`eval()` are unsupported | Low | UNSUPPORTED (intentional) |
| 15 | Missing `gc.h` header | Medium | FIXED |
| 16 | Duplicate `ir/ir.h` include | Low | FIXED |
| 17 | `finalize()` memory leak | Medium | FIXED |
| 18 | `wrap_numeric` template issue | Low | FIXED |

---

## Additional Fixes Applied Since Original Issue List

### Exception Handling Improvements
- `build_try_stmt()` stores exception in `__current_exception__` local
- Exception passed to handlers, stored in `__exception_type__` when handler has a type
- `finally` block always executes regardless of exception state
- `else` clause after `except` blocks supported

### Interpreter AND/OR Value Propagation Fix
- `handle_and()` returns left if falsy, else right (not bool 0/1)
- `handle_or()` returns left if truthy, else right (not bool 0/1)
- Matches Python semantics

### LLVM Codegen BINOP/CMP Dispatch Fix
- Added explicit handling for `BINOP`/`CMP` instructions
- Dispatches based on instruction name (ADD, SUB, LT, etc.)

### Missing Builtin Functions Added
- `chr()`, `ord()`, `hex()`, `oct()`, `bin()` - type conversion builtins
- `update()`, `fromkeys()`, `popitem()` - dict methods
- `startswith()`, `endswith()` - string methods

### Core Language Features Added
- Dict literals, tuple literals, slice notation, tuple unpacking
- f-strings, decorators, match/case, async/await, yield
- `in` and `is` operators, short-circuit `and`/`or`
- Walrus operator: lexer + AST + IR builder done (parser handler pending)

---

## Priority Order for Remediation

All 18 original issues have been resolved. Only Issue 14 (`exec()`/`eval()`) remains open, intentionally unsupported.
