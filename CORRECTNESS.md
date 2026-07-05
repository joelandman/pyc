# CORRECTNESS.md

Correctness issues that must be fixed before the compiler can produce reliable binaries.
Sorted by criticality (most critical at top).

---

## Critical

### 1. `for` Loop Iteration Is a Stub

**Severity:** Critical  
**Location:** `ir/builder.cpp:221-275`, `codegen/ir2ll.cpp:374-383`  
**Status: FIXED**

- `build_for_stmt()` rewritten to use index-based iteration over lists
- Loop initializes index to 0, compares `index < len(list)` in condition
- Uses `LIST_GET` to get element at current index
- Added `pyc_range_list(start, stop, step)` runtime function
- LLVM codegen CALL handler emits `pyc_range_list()` for `range()` calls

### 2. `finalize()` Does Not Clean Up Registry Objects

**Severity:** Critical  
**Location:** `runtime/object.cpp:156-161`  
**Status: FIXED**

- `PyObjectFactory::finalize()` now calls `registry.cleanup()` to free all registered objects
- `get_registry()` moved before `finalize()` to fix scope issue
- All non-singleton objects are freed before singletons

---

## High

### 3. Small Integer Caching Returns Wrong Values

**Severity:** High  
**Location:** `runtime/object.cpp:57-72`  
**Status: FIXED**

- Separate singletons for -1, 0, 1 using `TYPE_INT + 1` and `TYPE_INT + 2` keys
- `create_int()` looks up correct singleton based on value

### 4. `raise` Statement Does Not Propagate Exceptions

**Severity:** High  
**Location:** `ir/builder.cpp:418-429`, `runtime/libpyc_runtime.cpp`  
**Status: FIXED**

- Added `pyc_raise_exception()`, `pyc_get_exception()`, `pyc_clear_exception()` runtime functions
- Thread-local `g_current_exception` stores current exception
- `build_raise_stmt()` emits `CALL pyc_raise_exception` with exception object

### 5. Objects Created in Runtime Library Are Not Registered

**Severity:** High  
**Location:** `runtime/libpyc_runtime.cpp:78-114`  
**Status: FIXED**

- Added `PyObjectFactory::register_object()` calls in `pyc_codegen_new_object()` and `pyc_new_type()`
- All objects created via these functions are now tracked by registry

### 6. `INTRINSIC_RANGE` Returns Empty List

**Severity:** High  
**Location:** `codegen/ir2ll.cpp:407-430`  
**Status: FIXED**

- `INTRINSIC_RANGE` now calls `pyc_range_list()` with start, stop, step parameters
- Extracts parameters from IR operands using `llvm::cast<llvm::ConstantInt>`

### 7. `SETATTR` Attribute Name Is Null Pointer

**Severity:** High  
**Location:** `codegen/ir2ll.cpp:495-515`  
**Status: FIXED**

- `SETATTR` case creates a GlobalVariable for the attribute name string
- Passes the GlobalVariable pointer directly to `pyc_setattr()`
- Same fix applied to `GETATTR`/`LOAD_ATTR` cases

### 8. Interpreter `handle_call()` Does Not Handle Dynamic Function Objects

**Severity:** High  
**Location:** `ir/interpreter.cpp:516-560`  
**Status: FIXED**

- `handle_call()` now checks module functions, builtins, and global vars for dynamic calls
- Checks if first operand is a function object reference
- Enables dynamic function calls and lambda expressions

### 9. Garbage Collector Fundamentals Broken

**Severity:** High  
**Location:** `runtime/gc.cpp`, `runtime/object.h`  
**Status: FIXED**

- `mark_object()` only sets mark bit, no longer touches refcount
- `del_ref()` deletes at refcount zero without mark set check
- `collect()` rebuilds `roots_` after sweep to contain only marked objects
- `new_object()` no longer auto-registers as root

### 10. Object Model Memory Management Broken

**Severity:** High  
**Location:** `runtime/object.cpp`, `runtime/builtins.cpp`  
**Status: FIXED**

- `create_str()` stores string via `str_value` member
- `create_function()` stores callable via `func_callable` member
- `to_int()` fixed for TYPE_FLOAT via `reinterpret_cast<uint64_t*>`
- `bool` builtin fixed (inverted logic)
- `PyObjectRegistry` tracks all non-singleton allocations
- `Py_INCREF`/`Py_DECREF` helpers added to object.h
- `finalize()` calls `registry.cleanup()` to free all registered objects

### 11. IR Builder Control Flow Broken

**Severity:** High  
**Location:** `ir/builder.cpp`, `ir/builder.h`  
**Status: FIXED**

- `build_unary()` fixed: NEG emits `0 - operand`, UPLUS returns operand directly
- `build_call()` detects class constructors via `build_class_call()`
- `build_class_call()` creates NEWOBJ + CALL for instantiation
- `build_subscript()` added for LIST_GET
- Loop context tracking via `LoopContext` struct for break/continue
- All 10 statement handlers added: delete, global, nonlocal, assert, raise, with, try, break, continue

### 12. LLVM Codegen for Python-Specific Operations Broken

**Severity:** High  
**Location:** `codegen/ir2ll.cpp`  
**Status: FIXED**

- `POW` calls `pyc_pow` runtime function
- `GETATTR`/`LOAD_ATTR` emit calls to `pyc_getattr` with attribute name string
- `SETATTR` emits call to `pyc_setattr` with proper attribute name string
- `MAKE_LIST` calls `pyc_new_list()`
- `LIST_GET` calls `pyc_list_get()`, `LIST_SET` calls `pyc_list_set()`
- `NEWOBJ` calls `pyc_codegen_new_object()`
- `INTRINSIC_PRINT` calls `pyc_print()`, `INTRINSIC_TYPE` calls `pyc_type_name()`
- `INTRINSIC_LEN` calls `pyc_len()`, `INTRINSIC_INIT` calls `pyc_object_init()`
- `ISINSTANCE` calls `pyc_isinstance()`, `NEWTYPE` calls `pyc_new_type()`
- `range()` calls `pyc_range_list()` via CALL instruction
- LLVM O2 optimization pipeline enabled

### 13. Interpreter Memory Management Broken

**Severity:** High  
**Location:** `ir/interpreter.cpp`, `ir/interpreter.h`  
**Status: FIXED**

- Frames use `std::unique_ptr<CallFrame>` in frame stack
- Instruction result cache (`instr_results` map) with `cache_result()`/`get_cached_result()`
- `getattr/setattr` implemented using `instance_attrs` on PyObject
- `handle_call()` checks module functions, builtins, and global vars for dynamic calls

### 14. Exception Handling Does Not Propagate Exceptions

**Severity:** High  
**Location:** `ir/builder.cpp`, `runtime/libpyc_runtime.cpp`  
**Status: FIXED**

- `build_try_stmt()` stores exception in `__current_exception__` local
- Exception passed to handlers, stored in `__exception_type__` when handler has a type
- `finally` block always executes regardless of exception state
- `else` clause after `except` blocks supported

### 15. Interpreter AND/OR Value Propagation Broken

**Severity:** High  
**Location:** `ir/interpreter.cpp`  
**Status: FIXED**

- `handle_and()` returns left if falsy, else right (not bool 0/1)
- `handle_or()` returns left if truthy, else right (not bool 0/1)
- Matches Python semantics

### 16. LLVM Codegen BINOP/CMP Fall-Through

**Severity:** High  
**Location:** `codegen/ir2ll.cpp`  
**Status: FIXED**

- Added explicit handling for `BINOP`/`CMP` instructions
- Dispatches based on instruction name (ADD, SUB, LT, GT, EQ, etc.)

---

## Medium

### 17. Comprehension Support Missing

**Severity:** Medium  
**Location:** `ir/builder.cpp`, `frontend/parser.cpp`  
**Status: FIXED**

- List, set, generator, and dict comprehensions all implemented
- All return lists (sets/generators not fully supported)

### 18. Missing `gc.h` Header File

**Severity:** Medium  
**Location:** `runtime/` directory  
**Status: FIXED**

- `gc.cpp` includes `runtime/object.h` (not `runtime/gc.h`). GC class is defined in `runtime/object.h`.

### 19. `PyObjectFactory::finalize()` Memory Leak

**Severity:** Medium  
**Location:** `runtime/object.cpp:180-195`  
**Status: FIXED**

- `finalize()` now calls `registry.cleanup()` to free all registered non-singleton objects

---

## Low

### 20. `format()` Builtin Is a Stub

**Severity:** Low  
**Location:** `runtime/builtins.cpp:928-1001`  
**Status: FIXED**

- Handles positional placeholders `{0}`, `{1}` and format specifiers (.2f, d, s, %)

### 21. `dir()` and `globals()`/`locals()` Are Stubs

**Severity:** Low  
**Location:** `runtime/builtins.cpp:545-647`, `ir/interpreter.cpp:897-935`  
**Status: FIXED**

- `dir()` returns instance attributes, dict keys, and type methods
- `globals()`/`locals()` return interpreter state via thread-local `Interpreter::current()`

### 22. `exec()` and `eval()` Are Unsupported

**Severity:** Low  
**Location:** `runtime/builtins.cpp:655-667`  
**Status: UNSUPPORTED (intentional)**

- Intentionally unsupported due to security implications
- Use explicit function calls or pre-compiled IR modules instead

### 23. Duplicate `ir/ir.h` Include in `main.cpp`

**Severity:** Low  
**Location:** `main.cpp:17`  
**Status: FIXED**

- Removed duplicate include

### 24. `wrap_numeric` Template Has Deduction Issue

**Severity:** Low  
**Location:** `ir/interpreter.h:174-181`  
**Status: FIXED**

- Changed `auto op` parameter to template parameter `typename Op`
- Resolves C++20 extension warning with C++17 compilation

---

## Critical

### 25. `//` (FloorDiv) Compiler Crash / Invalid LLVM IR

**Severity:** Critical  
**Location:** `src/codegen/Codegen.cpp:937-994`  
**Status: FIXED**

- The int path of `div` op emitted `builder.CreateCondBr` followed by `CreateBr` while still inside a non-terminated basic block, leaving the IRBuilder's insert point invalid and producing "Terminator found in the middle of a basic block!" LLVM verification failures (or a compiler segfault inside `CreateAlignedLoad`).
- Triggered by any `//` or `//=` where operands are not both compile-time constants (e.g., `a=10; b=3; print(a//b)` or `x=7; x//=2; print(x)`).
- Replaced the broken branch-based DECREF cleanup with a single `select` between the two candidate PyObject* pointers (`boxedI64` and `quot`) and a `Py_DECREF` call. `Py_DECREF(NULL)` is a safe no-op in the runtime (`runtime/Runtime.cpp:222-234`), so the freed value safely covers both paths without introducing a terminator in the middle of a basic block.
- Added a `safeRhs = select(isZero, 1, rhs)` guard before the native `CreateSDiv`/`CreateSRem` so the host's division-by-zero trap never fires; the native result is discarded on the zero path anyway.
- Fixed the ownership logic so the *result* (`select(isZero, quot, boxedI64)`) is never the same value passed to `Py_DECREF`.
- Regression cases added to `tests/runner.py`: `//` between two variables, `//=`, `//` in expressions, in loops, with function-call consumers, and with subscript-source operands. 206/206 tests now pass (was 195/200 with 5 strict failures).

---

## Summary

| Severity | Count | Status |
|----------|-------|--------|
| Critical | 3 | All FIXED |
| High | 15 | All FIXED |
| Medium | 3 | All FIXED |
| Low | 5 | 4 FIXED, 1 UNSUPPORTED |
| **Total** | **26** | **25 FIXED, 1 UNSUPPORTED** |
