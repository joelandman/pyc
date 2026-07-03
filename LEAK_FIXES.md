# Memory Leak Fixes

Starting point: **5,904 bytes in 41 blocks** definitely lost. End point: **0 bytes in 0 blocks**.

---

## Fix 1 — Anonymous boxes in binary ops (`add`, `sub`, `mul`, `div`, `truediv`, `mod`, `neg`, `icmp`)

**Problem:** When a native `i64` or `double` value (produced by `emitNativeNumericBinary`) was passed to a PyObject-level fallback like `PyNumber_Add`, `getAsPyObject` silently created an anonymous box (`PyFloat_FromDouble` / `PyInt_FromLong`). That box had no name, was never in `ownedTemps`, and was never freed.

**Fix:** Before calling `getAsPyObject`, check if the raw value is native (`i64`/`double`). If so, emit an explicit `Py_DECREF` on the boxed result after the operation. Applied the same `lhsNative`/`rhsNative` pattern to all binary ops and `icmp`.

**Impact:** Eliminated 10 PyFloat blocks per 2 `report_energy` calls in nbody.py (5 per call across `add`/`sub`/`mul` on the energy accumulator).

---

## Fix 2 — Unused user-function call results (forward declaration problem)

**Problem:** When a user function's return value was never used (`tempUseCounts == 0`), the codegen was supposed to DECREF it immediately. But the check `!callee->isDeclaration()` was always `false` for forward-declared user functions (pre-declared without bodies before codegen), so the result was incorrectly treated as a borrowed ref and left owned but never freed.

**Fix:** Built a `userFunctionNames` set from all `ir.functions` entries (including their mangled `llvmFunctionName` variants) before codegen. The `isUserFunc` check became `!callee->isDeclaration() || userFunctionNames.count(funcName) > 0`.

**Impact:** Eliminated 1 PyInt block (the unused return value of `pyc_py_main` called from `pyc_user_main`).

---

## Fix 3 — `__name__` PyUnicode caching in `getOrLoad`

**Problem:** Every call to `getOrLoad("__name__")` allocated a fresh `PyUnicode_FromString("__main__")`. The `icmp` handler called `getOrLoad("__name__")` twice (once for the native-type check, once via `getAsPyObject`), creating two separate objects. Only one was tracked in `ownedTemps`; the other leaked.

**Fix:** Added a cache inside the `"__name__"` branch of `getOrLoad`: if `"__name__"` is already in both `ownedTemps` and `valueMap`, return the cached value. Otherwise allocate once, store in both, and track with `markOwned`.

**Impact:** Prevented double-allocation on every `__name__` comparison.

---

## Fix 4 — `emitDecRefIfOwned` resolves value before erasing from `ownedTemps`

**Problem:** `emitDecRefIfOwned` called `ownedTemps.erase(name)` before calling `getAsPyObject(name)` to obtain the LLVM value to DECREF. For `"__name__"`, `getOrLoad` uses `ownedTemps.count("__name__")` as part of its cache guard — finding it absent after the erase, it allocated a **new** PyUnicode and immediately DECREFed that, while the **original** `%__name__.name` was never freed (definitely lost).

**Fix:** Resolve the LLVM value via `getAsPyObject(name)` **before** `ownedTemps.erase(name)`, so the cache hits and returns the correct existing value.

**Impact:** Eliminated the final 1 definitely-lost block (144 bytes, 1 PyUnicode) — the last leak in nbody.py.

---

## Fix 5 — LLVM API type mismatch (`std::string` → `llvm::Triple`)

**Problem:** `createTargetMachine` and `setTargetTriple` in both `emitObjectCode` and `emitAssembly` were passing `std::string` where LLVM 17+ requires `llvm::Triple`. Pre-existing compile errors.

**Fix:** Changed `std::string targetTriple = "x86_64-unknown-linux-gnu"` to `llvm::Triple targetTriple("x86_64-unknown-linux-gnu")` in both locations.

---

**Net result:** 100% of definitely-lost blocks eliminated. Only "still reachable" memory remains (live globals like `BODIES` dict and default args held by the process at exit — correct behavior).
