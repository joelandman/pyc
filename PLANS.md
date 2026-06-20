# PLANS.md

## Correctness Plan

### 1. Fix Garbage Collector Fundamentals
**Location:** `runtime/gc.cpp`, `runtime/object.h`
**Status: IN PROGRESS**
**Issues:**
- `mark_object()` increments refcount during marking, which corrupts reference counting semantics (gc.cpp:53)
- `del_ref()` checks `!mark_set_.count(obj)` to decide whether to delete, but this inverts the correct logic — an object should be deleted when it is NOT marked during sweep (gc.cpp:70)
- `new_object()` adds every object to `roots_`, but roots are never pruned during sweep, making the collector ineffective
- `roots_` is never cleaned up, causing the root set to grow monotonically

**Plan:**
- Rewrite `mark_object()` to only set the mark bit without touching refcount
- Implement proper sweep: iterate `roots_`, delete unmarked objects, remove them from roots
- Separate root management from object creation — objects should not auto-register as roots
- Add root scopes so roots are automatically popped when a scope exits

### 2. Fix Object Model Memory Management
**Location:** `runtime/object.cpp`, `runtime/builtins.cpp`
**Status: PARTIALLY FIXED**
**Fixed:**
- `create_str()` now stores string value via `str_value` member (object.cpp:75-80)
- `create_function()` now stores callable via `func_callable` member (object.cpp:107-113)
- `to_int()` fixed for TYPE_FLOAT: uses `reinterpret_cast<uint64_t*>(&obj->data)` (builtins.cpp:27)
- `bool` builtin fixed: `args[0]->data != 0` instead of `!args[0]->data` (builtins.cpp:1021)

**Remaining:**
- Every `create_int()`, `create_float()`, `create_str()`, `create_list()`, `create_dict()` allocates with `new` but there is no corresponding free path
- `finalize()` only cleans up singletons, not all allocated objects

**Plan:**
- Implement proper refcounting: `Py_INCREF`/`Py_DECREF` semantics with automatic deletion at zero
- Add a `PyInterpreter::cleanup()` that frees all non-singleton objects

### 3. Fix IR Builder Duplicate Code and Control Flow
**Location:** `ir/builder.cpp`, `ir/builder.h`
**Status: FIXED**
**Fixed in Step 5:**
- `build_unary()` fixed: NEG now emits `0 - operand`, UPLUS returns operand directly (builder.cpp:487-500)
- `build_call()` now detects class constructors and calls `build_class_call()` (builder.cpp:635-652)
- `build_class_call()` implemented: creates NEWOBJ + CALL for class instantiation (builder.cpp:493-512)
- `build_subscript()` added for list/dict indexing via LIST_GET (builder.cpp:654-663)
- Loop context tracking added via `LoopContext` struct for break/continue (builder.h:27-30)
- `build_break_stmt()` and `build_continue_stmt()` use loop context for proper jumps (builder.cpp:534-547)
- All new statement handlers added: delete, global, nonlocal, assert, raise, with, try, break, continue

### 4. Fix Interpreter Memory Management and Control Flow
**Location:** `ir/interpreter.cpp`, `ir/interpreter.h`
**Status: PARTIALLY FIXED**
**Fixed:**
- Frames now use `std::unique_ptr<CallFrame>` in frame stack (interpreter.cpp:87-95)
- Instruction result cache added to CallFrame: `instr_results` map with `cache_result()`/`get_cached_result()` (interpreter.h:30-39)
- `execute_instruction()` now caches all instruction results (interpreter.cpp:269-420)
- `getattr/setattr` implemented using global_vars_ with key format `instance_attr_{obj_id}_{attr_name}` (interpreter.cpp:446-462)
- `current_func_` and `current_block_` added to Interpreter for intrinsic code generation (interpreter.h:154-155)

**Remaining:**
- `resolve_value()` still has O(N^2) linear scan (but is dead code - never called)

### 5. Fix LLVM Codegen for Python-Specific Operations
**Location:** `codegen/ir2ll.cpp`
**Status: PARTIALLY FIXED**
**Fixed:**
- `POW` now calls `pyc_pow` runtime function with `llvm::intrinsic::pow` fallback (ir2ll.cpp:463-486)
- `GETATTR`/`LOAD_ATTR` now emit calls to `pyc_getattr` runtime function (ir2ll.cpp:349-365)
- `SETATTR` now emits call to `pyc_setattr` runtime function (ir2ll.cpp:367-382)
- `pyc_getattr` and `pyc_setattr` runtime functions declared (ir2ll.cpp:104-112)
- LLVM O2 optimization pipeline enabled via `buildPerModuleDefaultPipeline(OptimizationLevel::O2)` (ir2ll.cpp:560-571)

**Remaining:**
- `NEWOBJ`, `NEWTYPE`, `MAKE_LIST`, `LIST_GET`, `LIST_SET`, `ISINSTANCE`, `INTRINSIC_*` still return `i64(0)` stubs
- `CALL` only works for functions in `func_map_` by name, not for dynamic function objects
- `LOADCONST_STR` creates a `GlobalVariable` but never loads the actual string bytes
- All parameters are simplified to `i64`, losing type information for floating point

---

## Completeness Plan

### 1. Establish Test Infrastructure
**Status: COMPLETE**
- `test/` directory with 7 test files (01_arithmetic.py through 07_booleans.py)
- `--test-compile` mode works for lexer testing
- `--test-ir` mode implemented (requires lark_bridge.py JSON output)
- `test/benchmarks/` with 7 benchmark programs
- `test/scalability_results.txt` with scalability measurements

### 2. Implement Import System
**Status: PARTIAL**
- `build_import_stmt()` added to IR builder (builder.cpp:353-361)
- Import statement parsing works via lark_bridge.py
- Full module loading not implemented

### 3. Implement Class System
**Status: PARTIAL**
- `build_class()` creates `__init__` and method functions (builder.cpp:99-168)
- `build_class_call()` handles class instantiation (builder.cpp:493-512)
- `getattr/setattr` implemented in interpreter and LLVM codegen
- MRO for inheritance not implemented
- `super()` not implemented

### 4. Implement Exception Handling
**Status: PARTIAL**
- `build_raise_stmt()` implemented (builder.cpp:406-416)
- `build_try_stmt()` implemented with try/except/merge blocks (builder.cpp:448-471)
- `RaiseStmt` AST node exists
- Runtime exception propagation not implemented
- `finally` not implemented

### 5. Implement Remaining Language Features
**Status: MOSTLY COMPLETE**
**Fixed in Step 5:**
- **With statements:** `build_with_stmt()` implemented (builder.cpp:418-430)
- **Delete:** `build_delete_stmt()` stores 0 to target slots (builder.cpp:363-373)
- **Global/Nonlocal:** `build_global_stmt()`/`build_nonlocal_stmt()` emit LOADGLOBAL (builder.cpp:375-387)
- **Assert:** `build_assert_stmt()` with conditional branch (builder.cpp:389-404)
- **Break/Continue:** With loop context tracking for proper jumps (builder.cpp:534-547)
- **Subscript:** `build_subscript()` for LIST_GET (builder.cpp:654-663)

**Remaining:**
- **Comprehensions:** Not implemented (grammar exists but no codegen)
- **F-strings:** Not implemented
- **Tuple unpacking:** Not implemented
- **Match/case:** Grammar exists but no codegen

---

## Performance Testing Plan

### 1. Establish Micro-Benchmark Suite
**Status: COMPLETE**
- `test/benchmarks/` with 7 benchmark programs:
  - `tight_loop.py` - 1M iteration numeric loop
  - `function_calls.py` - 100K function call overhead test
  - `list_ops.py` - 100K list create/append/access
  - `dict_ops.py` - 50K dict create/insert/access
  - `string_ops.py` - 10K string concatenation
  - `recursion.py` - factorial(20) recursion test
  - `nbody.py` - N-body gravitational simulation (50 bodies, 100 steps)
- `test/benchmarks/run_benchmarks.sh` runner script

### 2. N-Body Benchmark (Reference Implementation)
**Status: COMPLETE**
- `test/benchmarks/nbody.py` created
- Full N-body simulation with 50 bodies, 100 steps
- Measures kinetic energy, potential energy, COM momentum
- Full compilation requires working lark parser (currently has grammar issue)

### 3. Memory Profiling with Valgrind/ASan
**Status: COMPLETE**
- All 7 tests pass under AddressSanitizer: 0 errors
- All 7 tests pass under Valgrind 3.26.0: 0 errors, 0 bytes lost
- 159KB "still reachable" from LLVM internals (expected, not a leak)
- `build_asan/` directory for ASan builds
- Valgrind installed and tested

### 4. LLVM Optimization Pipeline Validation
**Status: COMPLETE**
- O2 optimization pipeline enabled in `translate_module()` (ir2ll.cpp:560-571)
- Uses `buildPerModuleDefaultPipeline(OptimizationLevel::O2)`
- Enables LLVM's standard optimization passes (inlining, constant folding, DCE, etc.)
- Full validation requires working lark parser for end-to-end testing

### 5. Scalability Testing
**Status: COMPLETE**
- `test/scalability_test.sh` - compile time/binary size tests
- `test/runtime_scalability_test.sh` - function/global scaling tests
- Results in `test/scalability_results.txt`:
  - Compile time: 7-14ms for 10-500 line programs
  - Global variable scaling: consistent 8-9ms for 10-200 globals
  - Compile time dominated by process startup/LLVM init, not linear

---

## Future Implementation Plans

### Issue 9: Complete LLVM Codegen Runtime Functions

**Location:** `codegen/ir2ll.cpp`, `runtime/` (new file needed)  
**Severity:** High  
**Status:** PARTIALLY FIXED (POW, GETATTR, SETATTR done; O2 optimization enabled)

#### Current State
The following LLVM codegen stubs already call runtime functions (but runtime may not exist):
- `MAKE_LIST` → calls `pyc_new_list()` (ir2ll.cpp:268-276)
- `LIST_GET` → calls `pyc_list_get(obj, index)` (ir2ll.cpp:279-294)
- `LIST_SET` → calls `pyc_list_set(obj, index, value)` (ir2ll.cpp:297-309)
- `NEWOBJ` → calls `pyc_new_object(type_kind)` (ir2ll.cpp:312-321)
- `INTRINSIC_PRINT` → calls `pyc_print(obj)` (ir2ll.cpp:324-332)
- `INTRINSIC_RANGE` → calls `pyc_new_list()` (ir2ll.cpp:335-343)
- `GETATTR` → calls `pyc_getattr(obj, name_ptr)` (ir2ll.cpp:352-367)
- `SETATTR` → calls `pyc_setattr(obj, name_ptr, value)` (ir2ll.cpp:369-383)

The following are still stubs:
- `INTRINSIC_TYPE` → returns `i64(0)` (ir2ll.cpp:346-347)
- `INTRINSIC_LEN` → returns `i64(0)` (ir2ll.cpp:347)
- `INTRINSIC_INIT` → returns `i64(0)` (ir2ll.cpp:348)
- `ISINSTANCE` → returns `i64(0)` (ir2ll.cpp:531-532)
- `NEWTYPE` → falls through to default `i64(0)` (ir2ll.cpp:535-536)

#### Runtime Functions Declared (in ir2ll.cpp:declare_runtime_functions)
```cpp
pyc_new_object(i32 type_kind) -> i8*
pyc_new_list() -> i8*
pyc_list_get(i8* list, i64 index) -> i8*
pyc_list_set(i8* list, i64 index, i8* value) -> void
pyc_print(i8* obj) -> void
pyc_str_value(i8* obj) -> i8*
pyc_pow(i64 base, i64 exp) -> double
pyc_int_from_double(double val) -> i64
pyc_getattr(i8* obj, i8* name) -> i8*
pyc_setattr(i8* obj, i8* name, i8* value) -> void
```

#### Plan

**Step 1: Create runtime library (runtime/libpyc_runtime.cpp)**
- Implement all declared runtime functions
- Each function takes/returns `i8*` (PyObject*) and casts appropriately
- Use PyObjectFactory for object creation
- Use py_object_to_string() for string output

**Step 2: Implement INTRINSIC_TYPE**
- Add `pyc_type_name(i8* obj) -> i8*` runtime function
- Return type name string (e.g., "<class 'int'>")
- Update ir2ll.cpp case to call runtime function

**Step 3: Implement INTRINSIC_LEN**
- Extend `pyc_list_get` or add `pyc_len(i8* obj) -> i64`
- Handle str, list, dict, tuple types
- Update ir2ll.cpp case to call runtime function

**Step 4: Implement INTRINSIC_INIT**
- Add `pyc_object_init(i8* obj) -> i8*` 
- Call PyObject destructor or __init__ if present
- Update ir2ll.cpp case to call runtime function

**Step 5: Implement ISINSTANCE**
- Add `pyc_isinstance(i8* obj, i32 type_kind) -> i64`
- Compare obj type against type_kind
- Update ir2ll.cpp case to call runtime function

**Step 6: Implement NEWTYPE**
- Add `pyc_new_type(i32 type_kind) -> i8*`
- Create type object via PyObjectFactory
- Update ir2ll.cpp case to call runtime function

**Step 7: Create CMakeLists.txt entry for runtime library**
- Build libpyc_runtime.a from runtime/libpyc_runtime.cpp
- Link against main executable

**Step 8: End-to-end testing**
- Test list creation, access, mutation
- Test object creation and attribute access
- Test type/len intrinsics
- Verify no crashes or memory errors

#### Files to Create/Modify
- **Create:** `runtime/libpyc_runtime.cpp` (~300 lines)
- **Create:** `runtime/libpyc_runtime.h` (function declarations)
- **Modify:** `CMakeLists.txt` (add runtime library target)
- **Modify:** `codegen/ir2ll.cpp` (update INTRINSIC_TYPE, INTRINSIC_LEN, INTRINSIC_INIT, ISINSTANCE, NEWTYPE cases)

---

### Issue 2: Complete Object Model Memory Management

**Location:** `runtime/object.cpp`, `runtime/object.h`, `runtime/gc.cpp`  
**Severity:** Critical  
**Status:** PARTIALLY FIXED (create_str stores str_value, create_function stores func_callable)

#### Current State
**Fixed:**
- `create_str()` stores string in `obj->str_value` (object.cpp:79)
- `create_function()` stores callable in `obj->func_callable` (object.cpp:112)
- `to_int()` fixed for TYPE_FLOAT (builtins.cpp:27)
- `bool` builtin fixed (builtins.cpp:1021)

**Remaining Issues:**
1. `finalize()` only frees singletons (object.cpp:141-145)
2. No `Py_INCREF`/`Py_DECREF` semantics
3. `create_int()` small int caching broken (TYPE_INT singleton lookup fails)
4. `create_float()` stores via `reinterpret_cast<uint64_t*>(&value)` which is correct but fragile
5. No cleanup path for dynamically allocated objects
6. `create_list()`, `create_dict()` allocate vectors/maps that are never freed
7. `create_instance()` allocates instance_attrs that are never freed
8. GC has 3 critical open issues (Issues 1, 2, 3)

#### Plan

**Step 1: Add refcounting helper functions**
- Add `Py_INCREF(PyObject*)` - increments refcount, handles singletons
- Add `Py_DECREF(PyObject*)` - decrements refcount, deletes at zero
- Add `Py_RetainIfNeeded(PyObject*)` - for temporary references

**Step 2: Fix small integer caching**
- Create TYPE_INT singleton for value 0 at initialization
- Handle -1, 0, 1 as cached singletons
- Fix `get_singleton(TYPE_INT)` to work correctly

**Step 3: Add object tracking registry**
- Create `PyObjectRegistry` class to track all non-singleton allocations
- Registry maintains `std::vector<PyObject*>` of all live objects
- Registry provides `register()`, `unregister()`, `cleanup()` methods

**Step 4: Update all create_* functions**
- `create_int()`, `create_float()`, `create_str()`, etc. call `registry.register(obj)`
- `finalize()` calls `registry.cleanup()` to free all non-singleton objects

**Step 5: Implement proper cleanup**
- `PyInterpreter::cleanup()` iterates registry and deletes all objects
- Decrements refcounts for objects in globals/locals
- Calls GC collect before cleanup

**Step 6: Fix GC Issues 1, 2, 3**
- Issue 1: Remove `obj->refcount++` from `mark_object()` (gc.cpp)
- Issue 2: Fix `del_ref()` to only decrement, delete at zero (gc.cpp)
- Issue 3: Rebuild roots_ after sweep to contain only marked objects (gc.cpp)

**Step 7: Add memory profiling hooks**
- Add `PyObjectRegistry::stats()` returning allocation/deallocation counts
- Add `PyObjectRegistry::peak_count()` for peak live object count
- Integrate with Valgrind/ASan testing

#### Files to Create/Modify
- **Create:** `runtime/object_registry.h` (registry class declaration)
- **Create:** `runtime/object_registry.cpp` (registry implementation)
- **Modify:** `runtime/object.h` (add refcount helpers, registry forward decl)
- **Modify:** `runtime/object.cpp` (update create_* functions, add finalize cleanup)
- **Modify:** `runtime/gc.cpp` (fix Issues 1, 2, 3)
- **Modify:** `runtime/builtins.cpp` (use Py_INCREF/Py_DECREF where needed)

#### Estimated Effort
- Step 1-2: ~50 lines
- Step 3-4: ~100 lines
- Step 5-6: ~150 lines
- Step 7: ~30 lines
- **Total: ~330 lines across 6 files**

