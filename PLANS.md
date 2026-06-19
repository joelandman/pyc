# PLANS.md

## Correctness Plan

### 1. Fix Garbage Collector Fundamentals
**Location:** `runtime/gc.cpp`, `runtime/object.h`
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
**Issues:**
- `create_str()` does not store the string value anywhere (object.cpp:64 — `data = 0`, comment says "stored separately" but never is)
- `create_function()` does not store the `std::function` callable at all
- Every `create_int()`, `create_float()`, `create_str()`, `create_list()`, `create_dict()` allocates with `new` but there is no corresponding free path
- `finalize()` only cleans up singletons, not all allocated objects
- `to_int()` and `to_float()` in builtins.cpp cast `obj->data` as `char*` for TYPE_STR conversion, which is incorrect since data is never set for strings

**Plan:**
- Add a `std::string` member to PyObject for string storage, or use a separate string table
- Store the `std::function` callable in the function object
- Implement proper refcounting: `Py_INCREF`/`Py_DECREF` semantics with automatic deletion at zero
- Add a `PyInterpreter::cleanup()` that frees all non-singleton objects
- Fix `to_int()`/`to_float()` to access stored strings correctly

### 3. Fix IR Builder Duplicate Code and Control Flow
**Location:** `ir/builder.cpp`, `ir/builder.h`
**Issues:**
- `build_function()` is defined twice (lines 64-98 and 100-123) with slightly different implementations
- `build_expr()` is defined twice (lines 233-270 and 272-274) — the second is a no-op identity call
- `build_for_stmt()` does not set up proper loop control flow (no back-edge, no condition check)
- `build_while_stmt()` does not evaluate the test expression or emit a branch
- `build_if_stmt()` does not emit an unconditional jump after each branch, causing fallthrough

**Plan:**
- Remove duplicate function definitions and keep the correct implementation
- Fix `build_for_stmt()` to emit: iter creation → loop label → next() call → branch on truthiness → body → jump back
- Fix `build_while_stmt()` to emit: loop label → test branch → body → jump back
- Fix `build_if_stmt()` to emit unconditional jumps after if/else bodies to a merge block

### 4. Fix Interpreter Memory Management and Control Flow
**Location:** `ir/interpreter.cpp`, `ir/interpreter.h`
**Issues:**
- `call_function_impl()` allocates `CallFrame` with `new` (line 202) but the frame pointer is compared against `frame_stack_.back()` — if the frame is popped during execution, the `delete frame` at line 231 double-frees
- `resolve_value()` does a linear scan through ALL blocks and ALL instructions for every value resolution — both correct and performance issues
- `handle_jump()` and `handle_branch()` do not update the block/instruction indices to actually jump
- `execute_block()` receives `inst_idx` by reference but never advances it across blocks

**Plan:**
- Use stack-allocated or arena-allocated CallFrames instead of raw `new`
- Fix the frame ownership model: either the frame stack owns frames (via unique_ptr) or the caller does, not both
- Implement proper control flow in `handle_jump()` and `handle_branch()` to update `block_idx` and `inst_idx`
- Add a `name_to_slot` cache in CallFrame to avoid linear scans

### 5. Fix LLVM Codegen for Python-Specific Operations
**Location:** `codegen/ir2ll.cpp`
**Issues:**
- `NEWOBJ`, `NEWTYPE`, `MAKE_LIST`, `LIST_GET`, `LIST_SET`, `ISINSTANCE`, `INTRINSIC_*`, `SETATTR` all return `i64(0)` stubs (lines 350-363)
- `CALL` only works for functions in `func_map_` by name, not for dynamic function objects
- `LOADCONST_STR` creates a `GlobalVariable` but never loads the actual string bytes
- All parameters are simplified to `i64`, losing type information for floating point
- `POW` uses `CreateFMul` (multiplication) instead of actual power

**Plan:**
- Implement `MAKE_LIST` to call runtime `pyc_new_list` and `LIST_GET/SET` to call runtime list accessors
- Implement `NEWOBJ` to call runtime object constructor
- Fix `POW` to call `llvm::Intrinsic::pow` or emit a `call @pyc_pow`
- Implement `CALL` to handle both direct function calls and dynamic call through PyObject
- Fix `LOADCONST_STR` to emit a proper pointer load from the global string

---

## Completeness Plan

### 1. Establish Test Infrastructure
**Goal:** Every feature has at least one test case that can be run via `./pyc --test <name>`
**Plan:**
- Create `test/` directory with test input files (`.py`) and expected outputs (`.txt`)
- Add `--test-compile` mode that compiles a test file and compares output against expected
- Add `--test-ir` mode that validates IR generation for known inputs
- Add `--test-llvm` mode that validates LLVM IR output
- Start with 10-15 core tests: arithmetic, conditionals, loops, functions, classes, lists, dicts, comprehensions

### 2. Implement Import System
**Goal:** Support `import math` and `from math import pi`
**Plan:**
- Add `IMPORT` and `FROM_IMPORT` handling in the IR builder
- Implement a module registry in `PyInterpreter` that caches loaded modules
- For C++ built-in modules (math, sys), register them at interpreter initialization
- For `.py` files, implement file-based module loading with path resolution
- Handle `__name__ == "__main__"` by checking the entry module name

### 3. Implement Class System
**Goal:** Full class inheritance, attributes, and method calls
**Plan:**
- Add `NEWOBJ` instruction that creates an instance with a `__dict__` for attributes
- Implement `GETATTR`/`SETATTR` to look up attributes in instance dict, then class dict, then bases
- Implement method resolution order (MRO) for inheritance
- Handle `super()` by looking up the next class in the MRO
- Store method functions in the class type object, not instances

### 4. Implement Exception Handling
**Goal:** `try/except/else/finally` and `raise` statements
**Plan:**
- Add `RAISE` instruction with exception type and value
- Add `TRY` instruction with handler block addresses
- Implement exception propagation through the call stack
- Store exception info in the frame (type, value, traceback)
- Implement `finally` as code that always executes regardless of exception

### 5. Implement Remaining Language Features
**Goal:** Support comprehensions, f-strings, tuple unpacking, with statements, match/case
**Plan:**
- **Comprehensions:** Generate a hidden function with a loop, or emit `MAKE_LIST` + `LIST_APPEND` IR instructions
- **F-strings:** Expand at AST level into `str()` calls + `+` concatenation, or emit a `FORMAT` IR instruction
- **Tuple unpacking:** Handle in `AssignStmt` by generating multiple STORE instructions from list indices
- **With statements:** Generate `__enter__` call, bind result, then `__exit__` call in a try/finally
- **Match/case:** Lower to nested if/elif chains or emit a `MATCH` instruction with pattern cases

---

## Performance Testing Plan

### 1. Establish Micro-Benchmark Suite
**Goal:** Quantitative performance baselines for compiler-generated code
**Plan:**
- Create `test/benchmarks/` with performance-critical patterns:
  - Tight numeric loops (1M iterations)
  - Function call overhead (100K calls)
  - List/dict operations (100K inserts + reads)
  - String concatenation (10K iterations)
  - Recursion depth tests
- Each benchmark measures: compile time, binary size, execution time, peak memory
- Compare against CPython execution time for the same code

### 2. N-Body Benchmark (Reference Implementation)
**Goal:** Validate correctness and performance on a realistic numerical workload
**Plan:**
- Create `test/nbody.py` implementing the parallel N-body simulation from the Python benchmark suite
- Run through the compiler and compare output against CPython
- Measure: execution time ratio (pyc binary vs CPython), memory usage via valgrind
- Target: within 5x CPython for this workload (given current boxing overhead)

### 3. Memory Profiling with Valgrind/ASan
**Goal:** Ensure no memory leaks, no use-after-free, no buffer overflows
**Plan:**
- Run all tests under `valgrind --leak-check=full --show-leak-kinds=all`
- Run all tests under AddressSanitizer (`-fsanitize=address`)
- Track object allocation/deallocation counts for known workloads
- Verify that object count returns to baseline after test completion
- Fix all reported issues before declaring correctness

### 4. LLVM Optimization Pipeline Validation
**Goal:** Verify that LLVM applies optimizations (inlining, constant folding, loop unrolling)
**Plan:**
- Emit LLVM IR and inspect for optimization opportunities:
  - Constant propagation: literal expressions should be folded
  - Dead code elimination: unreachable branches should be removed
  - Function inlining: small called functions should be inlined
- Test with `-O0`, `-O1`, `-O2`, `-O3` LLVM optimization levels
- Verify that optimized builds produce correct results
- Measure speedup from LLVM optimizations vs unoptimized

### 5. Scalability Testing
**Goal:** Verify compiler and runtime scale with program size
**Plan:**
- **Compile time:** Measure compilation time for programs of increasing size (100, 500, 1000, 5000 lines)
- **Binary size:** Measure output binary size growth
- **Runtime:** Measure execution time for programs with increasing numbers of:
  - Functions (10, 50, 100, 500 functions)
  - Global variables (10, 100, 1000 globals)
  - GC pressure (create and discard 10K, 100K, 1M objects)
- Identify scaling bottlenecks and plan optimizations
