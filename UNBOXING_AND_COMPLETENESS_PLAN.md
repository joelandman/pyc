# Unboxing and Completeness Plan for pyc

**Priorities (per project goals):** Correctness first, then completeness of implementation, then performance.

This document outlines the concrete next steps for:
1. Continuing unboxing work (integer loop counters → unboxed numeric locals → related).
2. Wiring up and implementing currently missing functionality.

Current state (as of 2026-06):
- `for x in range(...)` lowers to native loop blocks; hidden counters are i64 (see `Compiler.cpp:790 lowerRangeFor`, `Codegen.cpp:452 i64* ops`).
- Visible Python loop variable is still boxed each iteration (`box_i64` + `assign`).
- Proven `+`, `-`, `*` on `resultType=int|float` use native LLVM arithmetic then re-box (`Codegen.cpp:346 emitNativeNumericBinary`).
- Division (`//` and `/`), `**`, `%`, mixed/unknown types, and anything involving lists/dicts stay on the boxed `PyNumber_*` / runtime path.
- IR instructions carry conservative `resultType` (`IR.h:19`; set via `numericResultType` + `noteType` in `Compiler.cpp`).
- `valueTypes` map + `mergeBranchTypes` provide limited control-flow awareness (if/else); loops have weak tracking.
- Runtime.cpp already implements many "missing" builtins and methods (sum/sorted/any/all/isinstance, str find/count/replace, etc.), but lowering does not emit calls for most of them.
- Slicing: parser produces `Slice` with up to 3 children (start/stop/step); lowering and `Pyc_GetSlice` only handle start/stop.
- Dict comprehensions: `lowerDictComp` is a non-functional stub using undefined runtime helpers.
- No support yet for: `lambda`, `nonlocal`, `class`, general `import`, `*args` collection in defs, walrus `:=`.

Files of interest:
- Lowering: `src/Compiler.cpp` (LoweringVisitor, lowerRangeFor, numericResultType, noteType, lowerCall/lowerMethodCall/lowerSubscriptGet/lowerListComp/lowerDictComp).
- Codegen: `src/codegen/Codegen.cpp` (unboxToI64/unboxToDouble, box*, emitNativeNumericBinary, i64* instructions, valueMap slots).
- IR: `include/pyc/IR.h`, `src/ir/IR.cpp`.
- Runtime: `src/runtime/Runtime.cpp`, `include/pyc/runtime.h`.
- Tests: `tests/runner.py`, `tests/opt_*.py`, `tests/nbody.py`.

---

## Part A: Unboxing Roadmap (Performance, after correctness)

Goal: Reduce PyObject* allocations and refcount traffic for hot numeric code while preserving full Python semantics via conservative analysis + boxed fallback.

### A1. Strengthen Type Tracking (Foundation)
- Extend `valueTypes` (and the IR `resultType` annotations) to track locals across loops and repeated assignments.
  - Improve `mergeBranchTypes` to handle `while` and `for` (currently only if/else merge at `Compiler.cpp:614`).
  - On loop back-edges, conservatively widen types to "boxed" unless we can prove the variable stays numeric (or stays i64 for range counters).
  - Teach `numericResultType` to consider more sources: `int(x)`, `float(x)`, `abs(x)`, unary `-`, and range-loop induction variables.
- Add a tiny "proven numeric" lattice: `i64 | double | int_boxed | float_boxed | bool_boxed | unknown_boxed`.
  - "i64" means the *storage* can be a native i64 (no boxing inside a region).
  - Distinguish "this expression result is numeric" vs "this name's storage is unboxed".
- Add per-function "numeric region" inference: a contiguous block of code where a set of names are proven to only hold int/float values derived from constants, range counters, or prior numeric ops. (Start simple: intra-procedural, forward dataflow on the linear IR body.)

Milestone: `valueTypes` and IR resultType are accurate enough that we can trust them for storage decisions without breaking the 143+ tests.

### A2. Unboxed Numeric Locals (Core Feature)
- In codegen, for names proven to be "i64" or "double" throughout their live range (or a sub-region), allocate a native `alloca i64` or `alloca double` instead of (or in addition to) the PyObject* slot.
  - Keep the Python-visible name mapping to a boxed view at function boundaries, call sites, and any point where the variable might escape (return, list append, global assign, passing to a non-specialized call).
  - On assignment from a boxed source or at region exit, lazily box; on use in a numeric context inside the region, unbox once and reuse the native value.
- Extend `assign` / `i64assign` / `box_i64` / `i64_from_box` patterns so lowering can emit "stay unboxed" variants.
- For range loops: keep the hidden `__range_idx_*` as today (already i64). Optionally allow the visible loop variable (`node->id`) to live unboxed *inside the loop body* when all uses inside the loop are numeric and there are no assignments of non-int to it. Re-box only on loop exit if the variable is live after the loop.
  - This is a high-value win for microbenchmarks like `for i in range(N): x = i` or `x += i`.
- Handle control flow: when a numeric local is assigned in one branch but not another, or assigned a boxed value on one path, fall back to boxed storage for that name (or insert phi-like re-boxing at merge points).
- Refcounting: unboxed scalars have no refs. Only boxed values participate in INCREF/DECREF. Ensure we never DECREF a native scalar.

Milestone: `tests/opt_numeric_locals.py` (and similar) run with measurably fewer allocations; all existing tests still pass.

### A3. Widen Native Arithmetic
- Currently only `+ - *` with proven `resultType=int|float` go native (`Codegen.cpp:527,538,585`).
- Add native paths for:
  - Unary minus (already has `PyNumber_Negate`; can unbox + native + box).
  - Comparisons that feed branches (already partially unboxed in the `br` handler for i64 results).
  - Floor div (`//`) and mod (`%`) for the integral case only (preserve the runtime's Python floor semantics and div-by-zero → NULL behavior; do not silently produce inf/nan). Keep `/` (true div) boxed for now for the same reason stated in BENCHMARKS.md.
  - `**` for small non-negative integer exponents (common in nbody-style code) with a fast path; fall back to `Pyc_Pow` otherwise.
- In lowering (`lowerBinOp`, `lowerAugAssign`, `lowerUnaryOp`), propagate more precise result types for the new cases.
- At any point where a native result might be observed by Python (print, return, container element, call arg), ensure boxing happens.

Milestone: Hot loops in nbody-like code (many `+ - *` and a few `//`) spend less time in the runtime.

### A4. Unboxed / Homogeneous Numeric Lists (Vector-backed)
- Add a representation choice inside `PyObject`: for lists that are proven (or annotated) to be homogeneous `list[int]` or `list[float]`, store a compact `std::vector<int64_t>` or `std::vector<double>` (or a tagged buffer) instead of `std::vector<PyObject*>`.
  - Keep the public `PyList_*` API surface working (return boxed elements on get, accept boxed on set, etc.) so the rest of the runtime is unaffected.
  - Provide fast internal paths: `PyList_GetItemInt64`, `PyList_SetItemInt64`, etc., used only by codegen when it knows the list is homogeneous.
- In lowering:
  - Track element types for list literals and comprehensions (`[i*i for i in range(n)]` → homogeneous int list).
  - On subscript get/set and `append`, use the homogeneous fast path when the list variable and index are proven.
  - On aliasing (`alias = values`), conservatively treat both as possibly heterogeneous unless whole-program or escape analysis proves otherwise.
- In codegen: when emitting list subscript in a numeric context, unbox the element directly to i64/double instead of boxing then immediately unboxing.
- Migration: start with "newly created" homogeneous lists from comprehensions and literals in numeric contexts; later handle "this list was only ever appended ints".

Milestone: `tests/opt_numeric_lists.py` exercises the fast path; nbody (which builds lists of floats) benefits indirectly.

### A5. Allocation Sinking and Temporary Boxing Reduction
- Many numeric expressions currently do: compute native → box → store to local (boxed) → later unbox.
- When the boxed temporary does not escape (not passed to a call that might retain it, not stored into a container, not returned), emit the native value directly into a short-lived native temp and only box at the final use if required.
- In the IR, consider adding explicit "box" / "unbox" instructions so codegen can see and optimize the boundaries.

### A6. Specialized Function Variants (Call-site Monomorphization)
- For small functions called with proven concrete types (e.g., only ints, or only floats), emit a specialized native version that takes i64/double directly (no boxing on entry) and returns native (boxed only on return if the caller needs a PyObject*).
- Selection at call sites: if all arguments at a call are proven, emit a direct call to the specialized variant; otherwise fall back to the general `PyObject*` version.
- Start with intra-module calls; later consider a small monomorphic cache for external calls.
- Keep the original boxed version for correctness (and for calls from Python that we don't analyze).

### A7. Measurement and Guardrails
- Add microbenchmarks (extend BENCHMARKS.md) for pure numeric loops, list[int] mutation, and mixed code.
- Ensure every optimization has an opt-out or a conservative "assume boxed" path.
- Run the full test suite (`make check`) after every change; the nbody regression case must continue to match CPython output.
- Use `perf` / allocation counters to quantify wins (target remains the general compiler path, not benchmark-specific hacks).

---

## Part B: Missing Functionality (Completeness)

Implement these in priority order that respects "correctness first". Many have partial support in the runtime already.

### B1. Wire Up Existing Runtime Builtins (High ROI, Low Risk) — **DONE (2026-06)**
Runtime already provides (see `Runtime.cpp`, `runtime.h`):
- `PyBuiltin_Sum`, `PyBuiltin_Sorted`, `PyBuiltin_Any`, `PyBuiltin_All`
- `Pyc_IsInstance`
- `PyString_Find`, `PyString_Count`, `PyString_Replace`

In `Compiler.cpp` (lowerCall + lowerMethodCall):
- Wired special cases for sum/sorted/any/all/isinstance (with bool resultType for the predicate builtins).
- Wired str.find / .count / .replace (with int/str resultType notes).
- Added direct test cases in tests/runner.py; full suite (151/151) passes.

Also wire `list.count` if the runtime grows it; currently not present.

### B2. Full Slicing (Including Step)
- Parser (`PythonParser.cpp:256` and around `Subscript`/`Slice`): already builds up to 3 children for `Slice`.
- Lowering (`lowerSubscriptGet` and the assign path for slices):
  - Extend to pass the step child (may be None) through to a new or extended runtime call.
- Runtime:
  - Extend `Pyc_GetSlice` (and add `Pyc_SetSlice` for `a[i:j:k] = ...`) to accept a step PyObject* (or three separate start/stop/step).
  - Implement Python slice semantics: negative indices, step sign/direction, empty slices, etc. (the current start/stop implementation is partial).
  - For set-slice, handle resizing for lists and error cases for str (str slices are not assignable).
- In codegen, the existing `subscript` path and `Pyc_GetItem`/`Pyc_SetItem` dispatch will need to recognize slice objects (currently slices are represented as lists or special nodes? — check how the parser materializes them).
- Add regression tests for `lst[::2]`, `s[1:10:3]`, negative steps, assignment to slices, and str slicing.

### B3. Dict Comprehensions
- Current `lowerDictComp` (`Compiler.cpp:1262`) is a stub that emits calls to `dict_create`/`iter_create` etc. that do not exist in the runtime or codegen.
- Options:
  - A. Implement the missing tiny runtime helpers (`dict_create`, `iter_*`) to make the stub work (quick but low-quality).
  - B. Rewrite `lowerDictComp` to use the same real for-loop + assign machinery used by list comprehensions (preferred for correctness and to benefit from future unboxing).
- Target syntax: `{k: v for ... in ... if ...}` and nested.
- Ensure the result is a real dict (use `PyDict_New` + `PyDict_SetItem`).
- Add tests.

### B4. Lambda Expressions
- Lower `lambda args: expr` to an anonymous nested function (the existing nested function machinery should help).
- IR/codegen already support nested functions.
- The main work is in the parser (it currently skips or partially handles Lambda) and in assigning a unique name or handling the expression context.
- Capture rules: Python lambdas can close over variables; start with read-only captures (cell-like) and ensure they work like nested `def`.
- Result type of a lambda expression is "callable" (boxed function object). We will not specialize lambdas initially.

### B5. `nonlocal`
- Currently only `global` is handled (two-pass pre-scan + module-level `GlobalVariable`).
- `nonlocal` requires cells for variables that are assigned in an inner scope but live in an enclosing function scope.
- Implementation sketch:
  - Detect `nonlocal` declarations during lowering.
  - For affected names, allocate a cell (a small heap object holding the PyObject*) in the enclosing scope.
  - Both outer and inner functions access the cell through a dedicated slot or a hidden parameter.
  - Codegen needs to treat cell variables specially (load/store through the cell, INCREF/DECREF on the content).
- This is correctness work; it will interact with closures and future class methods.

### B6. Classes and Basic OOP
- Large feature. Plan in stages:
  1. Parse `class` and simple method defs (store methods on a class dict).
  2. Instance creation (`C()`), attribute get/set on instances (use a dict per instance for `__dict__` or a fixed slot map).
  3. Method binding (`self` insertion on attribute lookup of a function from a class).
  4. Inheritance (MRO stub for simple single inheritance) and `super()`.
- For unboxing: instance attributes that are proven numeric can live unboxed inside the instance storage (future).
- Start with a minimal viable that makes common patterns in nbody-like code (if any) and user examples work. Full descriptor/protocol machinery is out of scope initially.

### B7. General Import / Module System
- Current state: only a synthetic `sys` module (populated by `pyc_setup_sys` in `MainWrapper.cpp`) with `sys.argv`. `import sys` is faked via `PyObject_GetAttr` on the module object.
- Real work:
  - Parse `import foo`, `from foo import bar`, `import foo as x`.
  - At compile time, locate `.py` files (search path, or a simple single-file model).
  - Compile imported modules to separate object files or include their IR, and link them.
  - At runtime, provide a module registry (dict of module name → module dict).
  - Handle `sys.path`, `__name__`, packages at a basic level.
- For the nbody benchmark, the only import is `import sys`; the current synthetic support is sufficient for correctness but not general.
- Prioritize: make `import sys` and simple same-directory imports work first; defer packages, bytecode, C extensions, etc.

### B8. `*args` Collection and Related
- Parser already detects `*args` in signatures (see `PythonParser.cpp` comments around vararg).
- Lowering and codegen need to:
  - Accept a variadic tail in the function's IR signature (or pass as a single list).
  - At call sites with extra positional args, collect them into a list and pass it.
  - Inside the function, the `*args` parameter should be a list (or a view).
- `**kwargs` is harder (dict); can be deferred.
- Also handle calls with `*` unpacking in argument lists.

### B9. Walrus Operator `:=`
- Parse `NamedExpr`.
- Lowering: evaluate the value, assign to the target name (respecting scope), and produce the value as the expression result.
- Mostly a matter of wiring the AST node through the existing assign + expr machinery.
- Edge cases: inside comprehensions (the target lives in the containing scope in Python 3.8+), conditions, etc.

### B10. Other Gaps (Lower Priority)
- More string methods if users request them.
- `assert`, `del`, `with`, `async` — decide case-by-case.
- Full exception semantics beyond basic `try/except`.
- `match`/`case` (structural pattern matching).

---

## Part C: Execution Order and Testing Strategy

Recommended order (interleave correctness and unboxing where safe):

1. Wire the easy runtime builtins (B1) — immediate completeness win, zero risk to existing numeric work.
2. Fix and complete slicing (B2) — parser already has the info; small runtime change.
3. Fix dict comprehensions (B3) — make the existing test paths real.
4. Strengthen type tracking (A1) — prerequisite for safe unboxing.
5. Unboxed numeric locals + range loop var improvements (A2) + widened arithmetic (A3).
6. Lambda (B4), walrus (B9), *args (B8) — expression and calling-convention completeness.
7. nonlocal (B5).
8. Homogeneous numeric lists (A4).
9. Allocation sinking (A5) and specialized variants (A6).
10. Classes (B6) and general import (B7) — larger efforts.

Testing:
- Every change must pass `cd build && make check` (or `ctest`) and the full `tests/runner.py`.
- Add new cases to `CASES` or `FILE_CASES` in `runner.py` for each wired builtin, slicing form, dictcomp, lambda, etc.
- For unboxing, add or extend `tests/opt_numeric_locals.py`, `opt_numeric_lists.py`, and a pure-numeric microbenchmark. Verify output identity with CPython; optionally measure allocations via a simple LD_PRELOAD or by counting `PyInt_FromLong` etc. in a debug build.
- nbody.py must remain a correctness regression (it exercises floats, lists, loops, and `import sys`).
- When adding native paths, explicitly test the fallback (mixed types, division by zero, non-numeric values assigned to a previously-numeric local).

---

## Part D: Risks and Guardrails

- Semantics drift: native paths must not change observable behavior (e.g., `//` must floor toward -inf for negatives; div-by-zero must not produce inf; string `%` formatting edge cases).
- Refcounting bugs: unboxed values must never be INCREF/DECREF'd; boxed temporaries created for escape must be properly owned by the slots that receive them.
- Type instability: if a variable is proven numeric in one region but later assigned a string, the analysis must have already widened it to boxed, or we must insert a box at the assignment and switch storage.
- Compile-time cost: keep analyses simple (linear or near-linear on the IR body). We are not building a full SSA + escape analysis in one step.
- Debuggability: when `--verbose` or `--emit-llvm` is used, the generated IR should remain readable; consider naming native unboxed slots clearly (e.g., `i_acc` vs the boxed `acc`).

---

## Summary Checklist (Near-term)

- [ ] Lowering emits calls for sum/sorted/any/all/isinstance and str find/count/replace.
- [ ] Slicing with step works for get (and set for lists).
- [ ] Dict comprehensions produce correct dicts and pass tests.
- [ ] `valueTypes` / resultType tracking is loop-aware and trusted for storage decisions.
- [ ] At least one class of unboxed numeric locals (e.g., induction vars and simple accumulators) live in i64/double allocas inside numeric regions.
- [ ] Native paths exist for a few more ops (`-`, comparisons, safe integral `//`).
- [ ] All 143+ existing tests + new completeness tests pass; nbody output is identical to CPython.

This plan is intended to be updated as work progresses. Add dates or "Implemented in commit X" annotations when items land.
