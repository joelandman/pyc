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
- Lambda (B4) and *args (B8) have partial but useful implementations (see sections below); no support yet for: `nonlocal`, `class`, general `import`, walrus `:=`, **kwargs.

Files of interest:
- Lowering: `src/Compiler.cpp` (LoweringVisitor, lowerRangeFor, numericResultType, noteType, lowerCall/lowerMethodCall/lowerSubscriptGet/lowerListComp/lowerDictComp).
- Codegen: `src/codegen/Codegen.cpp` (unboxToI64/unboxToDouble, box*, emitNativeNumericBinary, i64* instructions, valueMap slots).
- IR: `include/pyc/IR.h`, `src/ir/IR.cpp`.
- Runtime: `src/runtime/Runtime.cpp`, `include/pyc/runtime.h`.
- Tests: `tests/runner.py`, `tests/opt_*.py`, `tests/nbody.py`.

---

## Part A: Unboxing Roadmap (Performance, after correctness)

Goal: Reduce PyObject* allocations and refcount traffic for hot numeric code while preserving full Python semantics via conservative analysis + boxed fallback.

### A1. Strengthen Type Tracking (Foundation) — **IN PROGRESS (2026-06)**
Concrete steps taken so far (foundation layer):
- `valueTypes` (the per-lowering type map) and `noteType`/`typeOf`/`numericResultType` are the central mechanisms. `resultType` annotations flow from them into IR instructions and are consulted by codegen for native `+ - *` etc.
- Normalized "i64" (native range counters from lowerRangeFor) to "int" inside `typeOf` and `numericResultType` so range induction variables participate in numeric result type decisions and do not force "boxed" for `+ - *` etc. (`Compiler.cpp`).
- Added `valueTypes.clear()` at module and per-function entry points so prior function state does not leak.
- Added `widenLoopTypes(entryTypes)` helper (conservative back-edge widening): at the end of while/for/range bodies, any name whose type differs from the type it had on entry to the loop head is widened to "boxed". This prevents the rest of the function (and future iterations) from assuming a type that is not stable across all iterations.
- Wired the widening calls into `lowerWhile`, general `lowerFor`, and `lowerRangeFor` (right before the back-edge branch).
- Added 3 regression cases in `tests/runner.py`:
  - Variables that become non-numeric on a later iteration (e.g., `x=i` then `x="done"`) must be "boxed" after the loop / on the path that observes the string.
  - Stable numeric accumulation across a range loop remains "int" (no spurious widen).
- All 170/170 tests continue to pass (the new cases specifically exercise the widening logic).
- `mergeBranchTypes` still only handles if/else; while/for widening is intentionally conservative (widens on any observed divergence at back-edge). This is the correct first step before a fuller lattice or per-region analysis.

Remaining for a solid A1 milestone (before heavy unboxing in A2):
- Teach `numericResultType` / callers about more sources that produce numeric results even if the input is "boxed" at the IR level but proven numeric at runtime (e.g., `int(x)`, `float(x)`, `abs(x)`, unary `-` on a name).
- Consider a tiny explicit lattice in comments or a small enum if we need to distinguish "i64 storage" vs "int boxed" vs "unknown" more precisely for codegen decisions.
- Optionally add a debug dump of `valueTypes` under `--verbose` for future tuning.

Milestone: `valueTypes` + IR resultType + loop widening are reliable enough that we can trust "int"/"float" annotations for deciding native storage and arithmetic inside regions without breaking the full test suite (currently 170 tests). This is the prerequisite for A2 (unboxed numeric locals).

### A2. Unboxed Numeric Locals (Core Feature) — **IN PROGRESS (2026-06)**
Work completed in this increment:
- Lowering change in `lowerRangeFor` (`Compiler.cpp`): the visible Python loop variable (`node->id`) is now published via `i64assign` + `noteType "i64"` instead of `box_i64` + `assign` on every iteration. This keeps the value as a native i64 alloca inside the loop body.
- Codegen infrastructure (`Codegen.cpp`):
  - Added `getAsPyObject(name)` helper: returns a PyObject* for any name. If the underlying storage is native i64 (or later double), it boxes on demand using the existing `boxI64`/`boxDouble` paths.
  - Updated all escape points that need Python objects (call arguments, `PyNumber_*` / runtime calls for mixed/boxed ops, `icmp` for general compares, `ret`, `print`, list subscript etc.) to go through `getAsPyObject` (or equivalent boxing) instead of raw `getOrLoad`.
  - `emitNativeNumericBinary` now stores the raw i64/double result in the instruction's result temp (instead of immediately boxing). This enables longer-lived unboxed temporaries and locals inside numeric regions. Boxing happens later only if/when the value must escape.
  - `assign` handling was extended: when the target is currently an i64 slot and the source is not i64, we switch the name's storage to a fresh PyObject* slot (so later arbitrary values, including strings from type-widening paths, are supported). When the source *is* i64 we keep the native slot.
- Tests: added several A2-specific cases in `runner.py` exercising the visible range var in pure numeric contexts (`s += i*i`), list append, calls (`f(i)`), and use-after-loop (final value must be correctly boxed for the print after the loop). The existing A1 widening cases (int→str assignment inside a range loop) continue to pass, confirming that the "switch to boxed storage" path works.
- All 174/174 tests pass (including nbody.py and opt_* files).

Remaining for fuller A2:
- General unboxed numeric locals (not just range induction variables): detect simple accumulators and temporaries that stay numeric for their live range and allocate native i64/double slots for them.
- More complete escape analysis / use-site boxing (e.g. when a numeric local is stored into a list that may later be read as a general object).
- Interaction with control flow merges inside loops (phi-like re-boxing or slot switching at certain points).
- Measurement: show reduced allocations for hot numeric microbenchmarks.

Milestone (updated): Visible `range` loop variables are unboxed; the getAsPyObject + native-result-temp infrastructure is in place. This is the first real step toward longer-lived unboxed numeric locals. Full general unboxed locals and allocation reduction will be expanded in follow-ups. All existing tests (174) remain green.

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

### B2. Full Slicing (Including Step) — **DONE (2026-06)**
- Parser (`PythonParser.cpp:256` Slice now walks "step" too): builds up to 3 children (lower/upper/step), None as nullptr placeholders.
- Lowering (`Compiler.cpp:lowerSubscriptGet`, assign path for `__subscript__`): pass step (empty string sentinel when absent) to `Pyc_GetSlice`/`Pyc_SetSlice`. Slice assignment now dispatches `Pyc_SetSlice` for Slice targets (get still uses `Pyc_GetItem` for simple index).
- Runtime: `Pyc_GetSlice(obj, start, stop, step)` and new `Pyc_SetSlice(obj, start, stop, step, value)` implement full Python semantics (negative indices, positive/negative step, empty results, length-changing basic slices, length-preserving extended slices with exact count matching).
- Codegen: updated declarations (4-arg GetSlice, 5-arg SetSlice).
- Tests: many new cases in `tests/runner.py` (step, negatives, str, list get+set, `[::-1]`, `[4:1:-1]=...` etc.). All 167/167 pass.
- Preserves boxed fallback; no native paths for slices yet.

### B3. Dict Comprehensions — **DONE (2026-06)**
- `lowerDictComp` fully rewritten in `Compiler.cpp` (now also registered in `lowerExpr`).
- Uses the same real index/len/loop/if machinery as list comprehensions (no fake `iter_create` etc.).
- Supports: single generator + ifs, multiple generators (product / nested `for`), target unpack (Name or Tuple/List).
- Emits `PyDict_New` + `PyDict_SetItem` per qualifying iteration.
- Parser already provided the AST shape (key, value, then comprehension generator nodes with target/iter/ifs).
- Tests added to `runner.py` (simple, filtered, product/nested). All 167/167 pass.
- Benefits from future unboxing work (numeric keys/values in dictcomp will be candidates for numeric locals).

### B4. Lambda Expressions — PROGRESS (2026-06)
- Parser fully handles Lambda (args including *args/**kwargs, defaults via Default children, body expression).
- Lowering (`lowerLambda`): creates a unique synthetic nested IR function (`__lambda_N`), cleans * / ** markers for the IR parameter list while preserving the original view in funcParamNames, lowers defaults (evaluated in definition context, stored as module globals for call-site injection), lowers the body expression as an implicit `ret`.
- Call resolution:
  - Direct literal calls: `(lambda ...)(args)` — lowerCall detects a Lambda callee child, lowers it (registers the synthetic + defaults), and emits the call to the returned synthetic name.
  - Assigned: `f = lambda ...` records `lambdaAliases[f] = synthetic`; subsequent calls through the name resolve via the alias map.
  - Lambdas can be passed to other functions and invoked indirectly in simple cases.
- *args in lambda signatures are parsed and the synthetic receives the collected list for the corresponding parameter.
- Defaults on lambdas are supported (mirrors FunctionDef default handling + injection).
- Limitations (follow-up): no full first-class function objects (the expression "value" is the synthetic name for resolution); closures over cells, `nonlocal`, and **kwargs forwarding are incomplete; complex capture falls back or may require cell allocation.
- Manual cases (including defaults, direct calls, assigned, passed lambdas, and *args on lambdas) pass; core suite (178/178) green.

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

### B8. `*args` Collection and Related — PROGRESS (2026-06)
- Parser produces Starred nodes for * unpacking in calls and vararg markers (`*name`) in FunctionDef/Lambda signatures (plus **kwargs markers).
- Call-site *args handling in lowerCall:
  - Static expansion: when the starred source is a list/tuple literal tracked via `listLiteralElemASTs` (assigned earlier in the function) or appears directly as a List/Tuple child of the Starred, its elements are lowered as separate positional operands on the emitted `call`. This yields an exact-arity call; normal default injection and callee * collection still apply.
  - Dynamic cases: a small runtime splice (PyList_SizeBoxed + loop + PyList_GetItemObj + PyList_Append) builds a collected "va" list of the effective positionals (fixed prefix + starred elements). The call is routed to a generated wrapper `__va_<target>` (registered via `ensureVaWrapper`).
- Wrapper + forwarding (`ensureVaWrapper`, `emitForwardCallFromList`): the wrapper takes a single list parameter and emits a signature-aware unpack that supplies the correct number of operands to the original target (fixed params before any *vararg, plus a runtime-collected tail list for the * slot if the target declares one). The wrapper then returns the target's result.
- Signature side: FunctionDef/Lambda with *args clean the marker for the IR parameter list but retain the original view (with `*` prefix) in `funcParamNames` for call-site and callee-collection analysis. Callee-side collection for declared *vararg parameters is implemented (excess positionals after the fixed prefix before the * are gathered into a list and supplied as the *param value).
- **kwargs, ** unpacking at call sites, and some corner cases (keyword-only after *, complex aliasing) remain for follow-up.
- This was advanced together with lambda support; the original `test_comprehensions.py` (*args via name) and many manual cases (direct literal lists, name-based, mixed) exercise the paths. Core suite stays green.

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

- [x] Lowering emits calls for sum/sorted/any/all/isinstance and str find/count/replace. (B1, 2026-06)
- [x] Slicing with step works for get (and set for lists). Full semantics incl. negatives. (B2, 2026-06)
- [x] Dict comprehensions produce correct dicts (incl. multi-generator) and pass tests. (B3, 2026-06)
- [x] `valueTypes` / resultType tracking strengthened (A1 foundation, 2026-06).
- [x] Unboxed numeric locals start: visible range vars native i64 + escape boxing + native numeric results (A2 start, 2026-06).
- [x] Native unary minus + safe integral `//` (A3, 2026-06).
- [x] Lambda expression lowering + *args call-site support (B4/B8, 2026-06). Parser handles Lambda (incl. *args/**kwargs) and Starred. lowerLambda creates synthetic nested IR functions with body-as-implicit-return and default handling. Call resolution covers direct literal lambdas, assigned lambdas, and passing lambdas. *args at call sites: static expansion for tracked list/tuple literals (assigned names + direct List/Tuple in * position) and runtime splice + generated `__va_<target>` wrappers with `emitForwardCallFromList` for dynamic cases; wrappers unpack to the target's fixed params (+ collected tail for declared *vararg). Callee-side *args collection for declared varargs is supported. **kwargs and full first-class callables remain follow-ups.
- [ ] All 178+ tests + new cases pass; nbody identical to CPython.

This plan is intended to be updated as work progresses. Add dates or "Implemented in commit X" annotations when items land.
