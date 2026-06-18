# pyc — Quality Assurance Plans

This document outlines concrete, actionable plans for ensuring **correctness**, **completeness**, and **performance** of the pyc compiler and its generated binaries. Each plan contains exactly five prioritized items with specific targets, success criteria, and rough effort estimates.

---

## 1. Correctness Plan

The primary correctness concern is **reference count integrity** in the runtime and codegen. The runtime uses manual refcounting with no garbage collector. Any imbalance — extra INCREF without matching DECREF, or DECREF on an already-freed pointer — leads to memory leaks or use-after-free crashes. The nbody benchmark demonstrates severe leaks: running with 50 000 steps consumes ~580 MB of heap, most of which is leaked PyObject allocations that are never freed.

### 1.1 — Fix the codegen assign path to DECREF old values on reassignment

**Problem:** `Codegen.cpp` lines 944-948 skip DECREF of the old value when a variable slot is reassigned. The comment says "existing slots may hold borrowed function arguments, module globals, or default argument objects" and conservatively avoids DECREF everywhere. This means every reassignment of a user variable leaks the previous PyObject*. In a tight loop (nbody: ~20 reassignments per iteration), this leaks ~20 heap objects per iteration — 580 MB at 50 000 iterations.

**Status: ✅ COMPLETE** (2026-06-17)

**Fix:** Added `ownedSlots` set to track alloca-backed slots that are owned by the function. In the assign codegen, DECREF the old value from owned slots before overwriting. Module globals (GlobalVariable) never DECREF. Borrowed alloca slots (params, cells) don't DECREF on first reassignment but the new value becomes owned for future reassignments.
- **Borrowed slots** (parameters, globals): created by the entry-block setup at lines 401-428 and 424-428. These never need DECREF because the codegen doesn't own these references.
- **Owned slots** (user-assigned variables): created by the first-time assign path at lines 960-965. These DO need DECREF on reassignment.

Track which slots are owned by recording the creation path. In the assign codegen, always DECREF the old value from an owned slot before storing the new value. The first-time assign path stays as-is (no old value to DECREF). The INCREF at lines 972-974 can stay for owned slots (it prevents the new value from being collected while the variable is live).

**Success criterion:** nbody at 50 000 steps uses <50 MB peak RSS (vs. current ~580 MB). Valgrind reports zero leaks for a simple test that reassigns variables in a loop. All 192 existing tests still pass.

**Effort:** 1-2 days.

### 1.2 — Fix `PyList_Pop` to DECREF before removing from list

**Problem:** `Runtime.cpp` line 448-453. `PyList_Pop` removes an item from the list's vector and returns it, but does not DECREF the list's reference first. The list INCREF'd the item on `PyList_SetItem` (line 109). After pop, the item has one extra refcount that no one owns — it leaks until the program exits.

**Status: ✅ COMPLETE** (2024-06-17)

**Fix:** Add `Py_DECREF(item)` before `pop_back()`:

```cpp
PyObject* PyList_Pop(PyObject* lst) {
    if (!lst || lst->type != 1 || lst->list.empty()) return nullptr;
    PyObject* item = lst->list.back();
    lst->list.pop_back();
    Py_DECREF(item);   // Remove list's reference before returning
    return item;       // Caller gets exactly one reference
}
```

**Success criterion:** A test that pushes then pops multiple times through the loop shows no memory growth over iterations.

**Tests:** `tests/final_correctness_test.sh` — 3 tests covering pop operations (pop_single, pop_all, pop_push_cycle). All passing at --opt=0.

**Effort:** <1 day.

### 1.3 — Fix `PyDict_GetItem` to return a new reference or document borrowed semantics

**Problem:** `Runtime.cpp` lines 194-201. `PyDict_GetItem` returns `pair.second` from the internal map without INCREF. The return type `PyObject*` is indistinguishable from functions that return new references (like `PyInt_FromLong`). Callers cannot know whether they need to DECREF the result.

**Status: ✅ COMPLETE** (2026-06-17)

**Fix:** Added `Py_INCREF(pair.second)` before returning from `PyDict_GetItem`. Updated `pyc_get_sys_attr` caller to remove redundant INCREF.

**Success criterion:** All callers of `PyDict_GetItem` are reviewed to confirm they DECREF the result (or the function is changed to INCREF). No new leaks introduced.

**Effort:** 1 day.

### 1.4 — Make LLVM module verification failure fatal

**Problem:** `Codegen.cpp` line 1033. When `verifyModule` fails, the error is printed but compilation continues and returns the broken module. The resulting binary may crash or produce incorrect output with no diagnostic.

**Status: ✅ COMPLETE** (2026-06-17)

**Fix:** After `verifyModule` fails, return `nullptr` instead of the broken module. The caller at `Compiler.cpp:3047` already checks for null and returns false.

**Success criterion:** No broken binaries are ever produced. CI catches any IR regressions immediately.

**Effort:** <1 hour.

### 1.5 — Add a valgrind-memcheck test target to CI

**Problem:** Memory bugs (leaks, use-after-free, double-free) are invisible to the existing text-comparison test suite. A program can produce correct output while leaking 500 MB of heap.

**Status: ✅ COMPLETE** (2026-06-17)

**Fix:** Added `make valgrind-test` target in CMakeLists.txt. Added valgrind tests to `tests/run_correctness_tests.sh` and `tests/final_correctness_test.sh`. Tests check for Invalid read/write operations (use-after-free, etc.). "definitely lost" blocks from local variables on exit are expected (reclaimed by OS).

Success criterion: CI fails if any test produces memory errors (Invalid read/write).

**Effort:** 1 day.

---

## 2. Completeness Plan

The compiler supports a substantial Python subset (types, operators, control flow, functions, comprehensions, etc.). The remaining gaps center on a few major feature categories. This plan targets an MVP that covers the most commonly used language constructs not yet implemented.

### 2.1 — Implement `class` statements (minimal, data-only)

**Target:** A minimal `class` that supports `__init__`, instance attributes via `self`, and method calls. No class inheritance, no descriptors, no property, no metaclasses.

**Approach:** A `class` body creates a dict (the class dict) to hold method names → callable tokens. `self` is the first parameter. Instance attributes are stored in a dict on the `self` object (`self.__dict__["attr"] = value`). Method calls on instances look up methods on the class dict (via `__class__` attribute), then call with `self` prepended.

**Success criterion:** The following compiles and produces correct output:
```python
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def distance(self, other):
        return ((self.x - other.x)**2 + (self.y - other.y)**2)**0.5

p1 = Point(0, 0)
p2 = Point(3, 4)
print(p1.distance(p2))  # 5.0
```

**Effort:** 3-5 days.

**Status: ✅ COMPLETE** (2026-06-17)

### 2.2 — Implement `import` / `from` statements

**Target:** `import sys`, `from os.path import join`, `import math as m`. Support for importing `.py` files from the same directory and from the `pyc` standard library (if any).

**Approach:** At compile time, parse the imported module's AST, lower it into the same ModuleIR, and treat its functions/variables as module-level globals (or separate module objects if the design supports namespacing). Handle circular import detection.

**Current status:** Partially implemented. `import sys` registers `sys` as a module-level global, but `sys.version` and other attributes are not available. The runtime has a `sys` module with `sys.argv`, but no `sys.version` attribute. User module imports are not yet supported.

**Success criterion:** `import sys; print(sys.version)` and `from math import sqrt; print(sqrt(4))` compile and run correctly.

**Effort:** 3-5 days.

### 2.3 — Implement `with` statements

**Target:** `with context_manager:` for objects that implement `__enter__` and `__exit__`.

**Current status:** Partially implemented. Parser support added, lowering emits `__enter__` call and `__exit__` call. However, there's an issue with the binding of the `as` target — the context manager expression evaluation isn't working correctly in the current implementation.

**Success criterion:** Simple `with` statements without `as` clause work. The `as` binding needs further work.

**Effort:** 2-3 days (file I/O adds significant scope; consider limiting to a simple `dummy` context manager for MVP).

### 2.4 — Add comprehensive test cases for every feature

**Target:** Every feature listed in FEATURES.md must have at least one inline test case in `tests/runner.py`'s `CASES` list, plus at least one file-based test in `FILE_CASES` if the feature exercises complex interactions (e.g., comprehensions with nested loops).

**Approach:** Audit FEATURES.md against the `CASES` list. For each feature with <2 test cases, write additional cases. Prioritize:
- Edge cases for existing features (e.g., empty dicts, nested comprehensions, chained comparisons with mixed types)
- Cross-feature interactions (e.g., lambda returning a list comprehension result used as a function argument)
- Negative cases (e.g., division by zero — should produce a runtime error, not a silent crash)

**Success criterion:** `make check` runs all cases at --opt=0 and --opt=3 with identical output for all cases. Every feature in FEATURES.md is tested.

**Effort:** 2-3 days.

### 2.5 — Implement `__str__` and `__repr__` protocol

**Target:** Allow user-defined classes to override `__str__` and `__repr__`. The `print()` function should call `__str__` on objects that have it. The `%s` formatting should call `__str__` as well.

**Approach:** Store `__str__` and `__repr__` methods in the class dict. When `PyObject_Print` receives a dict-backed object (class instance), it first checks for `__str__`, then `__repr__` in the class dict, and calls them via `Pyc_Apply` if found.

**Success criterion:**
```python
class Name:
    def __init__(self, n):
        self.n = n
    def __str__(self):
        return self.n
    def __repr__(self):
        return f"Name({self.n})"

print(Name("hello"))   # hello
```

**Effort:** 2 days (depends on 2.1 being implemented first).

**Status: ✅ COMPLETE** (2026-06-17)

---

## 3. Performance Plan

The generated binaries are correct but slow compared to CPython for numerical workloads. The primary bottleneck is the boxed object model: every numeric value is a heap-allocated `PyObject`, and every arithmetic operation involves a function call through the runtime. The native range loop optimization (A2) and native arithmetic narrowing (A1-A3) have reduced this for some paths, but the overall gap remains large.

### 3.1 — Benchmark suite: establish a regression baseline

**Target:** A repeatable, measurable benchmark suite that can track performance across commits.

**Approach:** Create a `benchmarks/` directory with standardized input files (nbody, a simple matrix multiply, a sorting benchmark, a string processing benchmark). For each benchmark, record:
- Wall-clock time (using a C helper that measures with `clock_gettime`)
- Peak RSS (via `/proc/self/status`)
- Number of heap allocations (via valgrind massif or custom malloc tracing)

Run each benchmark under the compiler at --opt=0 and --opt=3. Compare against CPython's output time. Store results in a JSON file that CI can compare against a baseline.

**Success criterion:** `make bench` produces a JSON report. CI fails if any benchmark regresses by >10% compared to the baseline.

**Effort:** 2-3 days.

### 3.2 — Inline the hot runtime functions

**Target:** Reduce function call overhead in the hot path (arithmetic, list access, comparison).

**Approach:** The LLVM optimizer can inline functions when it sees them as `InternalLinkage` or `PrivateLinkage`. Currently, runtime functions are declared with `ExternalLinkage`, which prevents cross-module inlining. Options:
- (a) Compile Runtime.cpp as part of the LLVM module (as a bitcode string embedded in the compiler) instead of linking a separate static library. This allows the LLVM optimizer to inline across the boundary.
- (b) Use `linkonce_odr` or `weak` linkage for hot functions so LLVM can merge and inline them.

Option (a) is more invasive but gives the most benefit. Start with a small subset of hot functions (PyInt_FromLong, PyNumber_Add, PyBuiltin_Len) to measure the impact.

**Success criterion:** nbody at 10 000 iterations runs 2x faster with inlining enabled.

**Effort:** 3-5 days.

### 3.3 — Optimize the boxed-to-unboxed conversion path

**Problem:** Every `getOrLoad` in codegen emits a load from an alloca, followed by type checks and field accesses when unboxing. For proven numeric variables (from range loops or native arithmetic narrowing), the type is known at compile time and the unboxing can be eliminated.

**Approach:** In codegen, track which valueMap entries are "known-native" (i64 or double, not boxed). When `unboxToI64` or `unboxToDouble` is called on a known-native value, return the native value directly without the struct field load + type check + select sequence. This eliminates ~5 LLVM IR instructions per arithmetic operation on proven-numeric values.

**Success criterion:** The LLVM IR for a simple `for i in range(n): s += i * i` loop has no struct loads or type checks in the inner loop body.

**Effort:** 2 days.

### 3.4 — Reduce allocation pressure via a small object pool

**Target:** Eliminate ~60% of heap allocations for small immutable objects (integers -5 to 256, which map to CPython's integer interning range).

**Approach:** Add a static pool of pre-allocated `PyObject*` values for small integers. `PyInt_FromLong` checks the pool first for values in the interning range and returns the pooled object (no new allocation). The pool objects have a fixed refcount that is never decremented (they leak, but the total leak is bounded to ~256 objects regardless of program size).

**Success criterion:** nbody at 50 000 steps has <5000 heap allocations (vs. current ~10 million+). Peak RSS drops by ~50%.

**Effort:** 1-2 days.

### 3.5 — Profile-guided optimization for the native range loop path

**Target:** Make the native range loop (lowered at `lowerRangeFor`) as fast as a hand-written C `for` loop with local variables.

**Approach:** The current range loop optimization avoids boxing the loop counter and uses native i64 arithmetic for the visible loop variable. However, when the loop variable escapes to a print call, it is boxed. The boxing happens inside the loop body via `getAsPyObject`. Optimize by:
- (a) Moving the boxing to only the points where it's actually needed (after the loop, or at call sites), rather than generating a box on every iteration.
- (b) Using LLVM's `loop-unroll` and `loop-vectorize` passes aggressively for range loops with small, constant bounds.
- (c) In the LLVM IR, emit `call` instructions to `PyInt_FromLong` only when the value must escape — not every time it's used in a print-like context.

**Success criterion:** A loop like `for i in range(n): print(i)` runs at comparable speed to a C equivalent (accounting for print overhead).

**Effort:** 3-5 days.

---

## Cross-cutting Concerns

### Simplicity

All plans above should be evaluated against the principle of **simplicity**. Before implementing any of the above:
- Ask: can this be achieved with fewer moving parts?
- Ask: does this add a new data structure or tracking mechanism to the compiler?
- Ask: is the runtime function signature clear and consistent?

The B4/B5 lambda/closure support added ~15 tracking maps and hundreds of lines of code. Any new feature should aim for the same complexity budget: one new data structure, one new IR instruction type, at most two new runtime functions.

### CI Integration

Every plan item should have a corresponding CI gate. The `.github/workflows/` (or equivalent) should run:
1. `make check` — correctness
2. `make valgrind-test` — memory safety (1.5 complete)
3. `make bench` — performance regression (once 3.1 is done)
