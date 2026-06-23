# PERFORMANCE.md

Performance optimization plans to reduce the gap between pyc and CPython.
Sorted by criticality (highest impact at top).

---

## High Impact

### 1. Introduce Type-Specialized IR Instructions

**Current bottleneck:** Every arithmetic operation boxes values into `PyObject*`, calls runtime functions, checks types, and unboxes. A simple `a + b` compiles to: `LOADGLOBAL → LOADGLOBAL → CALL pyc_runtime_add → STORELOCAL`.

**Plan:**
- Add type metadata to IR: `TypeKind` field to track whether a value is int/float/object
- In LLVM codegen, emit native `add`/`mul`/`sub` for int/float operations instead of calling runtime
- Only call runtime functions for object operations (getattr, len, type, isinstance)
- Add `IS_INT`/`IS_FLOAT` type check instructions to branch between specialized and generic paths

**Expected speedup:** 5-20x for numeric computation benchmarks

### 2. Inline Small Built-in Functions

**Current bottleneck:** Every builtin call (print, len, type, range, etc.) goes through `std::function` indirection and PyObject allocation. `len(x)` compiles to: `LOADGLOBAL len → LOADLOCAL x → CALL builtin_len → result`.

**Plan:**
- In LLVM codegen, detect calls to known builtins by name and emit inline code
- `len(x)` → call `pyc_len(x)` directly (already done) but eliminate the LOADGLOBAL step
- `print(x)` → call `pyc_print(x)` directly without function lookup
- `type(x)` → call `pyc_type_name(x)` directly
- `range(n)` → create list with `pyc_new_list()` and loop with `pyc_list_set()`
- Add a builtin dispatch table in ir2ll.cpp that maps builtin names to inline LLVM IR

**Expected speedup:** 30-50% for programs heavy on builtin calls

### 3. Eliminate PyObject Overhead in Hot Paths

**Current bottleneck:** Per-operation overhead is ~100ns (PyObject alloc + type check) vs ~1ns (native CPU instruction).

**Plan:**
- Profile hot paths with `perf record` and `perf report`
- Identify functions where 80% of time is spent in PyObject allocation/deallocation
- Replace with unboxed representation in IR: `i64` for ints, `f64` for floats
- Only use PyObject* when object semantics are required (strings, lists, dicts, functions)

**Expected speedup:** 5-20x for numeric computation benchmarks

### 4. Optimize Object Allocation with Arena Allocator

**Current bottleneck:** Every `new PyObject()` goes through `malloc`/`new` with per-object overhead. Hot loops create and destroy thousands of small objects.

**Plan:**
- Create `PyObjectArena` class that pre-allocates 4KB blocks and hands out objects from them
- Track allocations per arena; free entire arena at once instead of individual objects
- Add `arena_alloc()` to PyObjectFactory as primary allocation path
- Singletons still use direct `new`; arena used for all runtime objects
- Add arena per function scope; reclaim at function return

**Expected speedup:** 15-30% for programs with heavy object creation (list append, dict insert)

---

## Medium Impact

### 5. Add IR-Level Constant Folding

**Current bottleneck:** No compile-time evaluation of constant expressions. `1 + 2 * 3` becomes runtime operations.

**Plan:**
- After building each IR function, run a constant folding pass over linear blocks
- For each instruction, check if all operands are constants (`is_const == true`)
- If yes, evaluate the operation and replace with a `LOADCONST` instruction
- Handle arithmetic ops (ADD, SUB, MUL, DIV, MOD, POW), comparisons (LT, LE, GT, GE, EQ, NE), and boolean ops (AND, OR, NOT)
- This eliminates entire expression trees at compile time

**Expected speedup:** 20-40% for programs with many constant expressions (math, benchmarks)

### 6. Optimize String Operations

**Current bottleneck:** String concatenation and manipulation is 10-50x slower than CPython due to repeated heap allocation.

**Plan:**
- Implement string interning: cache frequently-used string literals in a global map
- Add `pyc_string_concat` that pre-allocates result buffer (avoid O(n²) reallocation)
- Implement `join` as a single-pass operation with pre-computed total length
- Add `reserve` capacity to `std::string` before repeated concatenation

**Expected speedup:** 3-10x for string-heavy programs

### 7. Optimize List Operations

**Current bottleneck:** List append and access is slow due to PyObject wrapper around each element.

**Plan:**
- Implement `std::vector<int64_t>` as a specialized "int list" type alongside PyObject list
- Implement `std::vector<double>` as a specialized "float list" type
- Auto-detect homogeneous lists at compile time and use specialized operations
- Keep generic PyObject list for heterogeneous collections

**Expected speedup:** 3-8x for list-heavy programs

### 8. Enable LLVM Inlining and Profile-Guided Optimization

**Current bottleneck:** LLVM O2 pipeline is enabled but aggressive inlining is not configured. Runtime functions like `pyc_list_get`, `pyc_getattr` are not inlined across call boundaries.

**Plan:**
- Configure LLVM passes for aggressive inlining: `PassBuilderOptions.setInliningThreshold(InliningMode::Standard)`
- Add `alwaysinline` attribute to small runtime functions: `pyc_int_from_double`, `pyc_isinstance`, `pyc_len`
- Add `noinline` to large functions: `py_object_to_string`, `pyc_print`
- Use `opt` command-line tool to inspect generated LLVM IR and verify inlining
- Add `llvm::Attribute::InlineHint` to hot call sites (builtins, list operations)

**Expected speedup:** 10-25% for programs with many small function calls

---

## Lower Impact

### 9. Optimize Global Variable Access

**Plan:**
- Track which globals are read/written in each function
- Promote frequently-accessed globals to local variables (SSA form)
- Use LLVM `alias analysis` to optimize global variable loads/stores

**Expected speedup:** 5-15% for programs with many global variable accesses

### 10. Add Dead Code Elimination at IR Level

**Plan:**
- After building IR, identify instructions whose results are never used
- Remove unused instructions (not referenced by any other instruction or return)
- Remove unreachable basic blocks (after dead branches)
- Remove unused function definitions (not in call graph from __main__)

**Expected speedup:** 10-20% reduction in generated LLVM IR size

### 11. Implement Tail Call Optimization

**Plan:**
- Detect tail calls: call is the last instruction in a function
- In LLVM codegen, emit `tail` call attribute for tail calls
- Enables recursive functions to use O(1) stack space

**Expected:** enables infinite recursion patterns without stack overflow

### 12. Add Type-Based Specialization for Common Patterns

**Plan:**
- Analyze IR to detect pure-integer or pure-float functions
- Generate specialized LLVM IR that uses `i64`/`f64` instead of `i8*` (PyObject*)
- Generate generic fallback for mixed-type calls

**Expected speedup:** 3-10x for numeric computation functions

### 13. Integrate LLVM `opt` with Aggressive Flags

**Plan:**
- Run `opt -O3` on generated LLVM IR before codegen
- Enable `inliner`, `instcombine`, `licm`, `loop-unroll`, `slp-vectorize`
- Add custom LLVM passes for Python-specific optimizations

**Expected speedup:** 10-30% across all programs

### 14. Add Lazy Compilation for Functions

**Plan:**
- Skip LLVM codegen for functions that are defined but never called
- Add call graph analysis in IR module to detect reachable functions
- Only compile functions reachable from `__main__` or called functions

**Expected speedup:** 20-40% faster compile time for large programs with many unused functions

---

## Benchmark Plans

### 1. Benchmark Against CPython for Same Programs

**Goal:** Establish baseline performance gap and identify hotspots.

**Plan:**
- Run all benchmark programs (`test/benchmarks/`) through CPython and pyc
- Measure wall-clock time for each: `time python3 bench.py` vs `time ./pyc --compile bench.py && ./bench`
- Compare compile time + execution time for pyc vs pure CPython
- Expected: pyc will be slower for small programs (JIT overhead), faster for large loops (native code)
- Target: 2-5x slower than CPython for tight loops, 1-2x for compute-heavy programs

---

## Summary

| Priority | Count | Description |
|----------|-------|-------------|
| High | 4 | Core optimization paths with 5-50x potential speedup |
| Medium | 4 | Language-specific optimizations with 3-30% speedup |
| Lower | 6 | Incremental improvements with 5-40% speedup |
| **Total** | **14** | **All planned** |
