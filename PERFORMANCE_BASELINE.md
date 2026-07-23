# Performance Baselines

## Python Interpreter Baselines (~60-120s range)

| Benchmark | Argument | Time | Memory (RSS) |
|-----------|----------|------|--------------|
| nbody.py | 10,000,000 | ~53s | 12.5 MB |
| fibn.py | 41 | ~46s | ~12 MB |
| fibn.py | 42 | ~69s | ~12 MB |
| mbs.py | N/A | excluded (uses `time` module) |

**Note:** mbs.py uses `from time import perf_counter` which isn't supported by pyc.
The benchmark is deliberately excluded from the test runner (see runner.py comment).

---

## Compiled Binary Performance: nbody.py at n=50,000

| Configuration | Time | vs Python |
|---------------|------|-----------|
| Python interpreter | 0.24s | baseline |
| pyc --opt=0 | 3.63s | 15.1x slower |
| pyc --opt=1 | 3.86s | 16.1x slower |
| pyc --opt=2 | 3.54s | 14.7x slower |
| pyc --opt=3 | 3.61s | 14.9x slower |

**Finding:** All optimization levels perform similarly. LLVM optimizations (O1-O3)
have minimal impact because the bottleneck is inboxed runtime function calls
(PyNumber_Add, PyFloat_FromDouble, etc.), not LLVM IR patterns.

---

## Compiled Binary Performance: fibn.py (recursive fibonacci)

| Configuration | n=20 | n=25 |
|---------------|------|------|
| Python interpreter | 0.024s | 0.037s |
| pyc --opt=0 | 0.047s | 0.514s |
| pyc --opt=3 | 0.050s | 0.572s |

**Finding:** Recursive function calls show massive overhead. At n=25, compiled
binary is ~14x slower than Python. The issue is that each recursive call to `fib()`
goes through the boxed function call mechanism (`Pyc_Apply`), with full PyObject
allocation/deallocation for arguments and return values.

---

## Profile Hotspots (nbody.py, opt=2, n=50,000)

Top runtime functions by sample count:

| Function | Overhead | Purpose |
|----------|----------|---------|
| (main loop) | ~60% | advance() function body |
| Py_DECREF | 4.82% | Reference counting (every boxed value) |
| PyFloat_FromDouble | 1.61% | Boxing float results from list operations |
| PyList_GetItemObj | 1.64% | Boxed list subscript access |
| PyInt_FromLong | 1.32% | Boxing int results |
| PyNumber_Multiply | 0.78% | Boxed multiplication |
| PyNumber_Add | 0.49% | Boxed addition |
| PyList_SetItemBoxed | 0.55% | Boxed list item set |
| Pyc_GetItem | 0.38% | Generic subscript access |

---

## Root Cause Analysis: Why Compiled is Slower Than Python

### 1. Excessive Boxing/Unboxing
The `bodies` list in nbody.py contains inner lists of floats. The outer list type
is typed as generic `"list"` (boxed) rather than `list_float`, so `bodies[i]`
returns a `PyObject*`. Then `bodies[i][0]` goes through:
- `PyList_GetItemObj` (boxed subscript, ~5-10ns + branching)
- `PyFloat_FromDouble` (malloc allocation, ~50-100ns)

Every value read from a list triggers a heap allocation.

### 2. Function Call Overhead
Every arithmetic operation (`+`, `*`, `-`) goes through C function calls:
- `PyNumber_Add()`, `PyNumber_Multiply()`, `PyNumber_Subtract()`
- Each does runtime type checks (pyc types), type dispatch, then computation
- Python interpreter uses inline bytecode handlers (BINARY_ADD, etc.)

### 3. Reference Counting
Every boxed value requires:
- `Py_INCREF` when copied/stored
- `Py_DECREF` when discarded
- At opt=0/1/2/3, the runtime is not inlined, so each is an indirect function call

### 4. LLVM Can't Optimize Away Runtime Calls
Even at O3, LLVM cannot inline runtime calls because they're in the separate
Runtime.cpp static library which is linked in but not visible to LLVM's
optimization pass.

---

## Optimization Opportunities

### Low-Hanging Fruit (Quick wins):

1. **Enable LLVM optimization at opt=0**: Currently `if (optLevel <= 0) return;`
   runs ZERO passes. Even a minimal instcombine + simplifycfg would help.

2. **Mark runtime functions as `inline`**: Add `__attribute__((always_inline))`
   to simple runtime helpers (PyInt_FromLong, PyFloat_FromDouble, etc.) so
   LLVM can inline them at O2/O3.

3. **Better type inference for nested lists**: Detect that `bodies` elements are
   homogeneous float lists and use `list_float` type → `PyList_GetItemDouble`.

### Medium Priority:

4. **Native subscript path**: The A4 optimization has native subscript paths
   (`PyList_GetItemDouble`) but they're only triggered when `resultType` is
   explicitly `"float"`. Need to expand result type inference.

5. **Small-object allocator**: Pyc's runtime uses malloc for every int/float/bool.
   A thread-local arena allocator would dramatically reduce allocation overhead.

6. **Tail-call optimization**: fibn.py recursive pattern would benefit from TCO.

### Higher Priority:

7. **Escape analysis / allocation sinking**: Values that don't escape a function
   can be stack-allocated instead of heap-allocated.

8. **Native int/float locals (A5)**: Already partially implemented. Expand
   coverage to more expressions.

9. **Function specialization (A6)**: Already partially implemented. The
   `fib()` function with int parameters should get a native specialization.

---

## Target Performance Goals

Based on the baseline, targets for n=50,000:

| Level | Target Speedup vs Python | Target Time |
|-------|--------------------------|-------------|
| Current (all levels) | 0.07x (15x slower) | 3.5s |
| After quick wins | 1x (parity) | 0.24s |
| After medium priority | 3-5x | 0.05-0.08s |
| After full optimization | 10-20x | 0.01-0.02s |

**Actual progress:**
| Achieved | Performance | Gap |
|----------|-------------|-----|
| After LTO (Phase 12) | 0.48s | 1.7x slower |
| After float chain (Phase 13) | **0.44s** | **1.6x slower** |

For fibn (recursive), now fully native via A6 specialization:

| Level | Target Speedup vs Python | Target Time (n=35) |
|-------|--------------------------|-------------------|
| Before Phase 27 | 0.23× (4.3× slower) | 2.05s → 0.44s |
| After Phase 27 (native recursion) | 44× faster | 2.05s → **0.047s** |

---

## Memory Utilization Baselines

| Benchmark | Configuration | Peak RSS | Allocations (approx) |
|-----------|--------------|----------|---------------------|
| nbody(50,000) | Python | ~10 MB | Many (CPython free-list) |
| nbody(50,000) | pyc opt=2 | ~8 MB | ~2M+ (every value boxed) |
| fibn(25) | Python | ~11 MB | Recursive stack |
| fibn(25) | pyc opt=0 | ~12 MB | ~1M+ per call level |

---

## Test Sizes for Target 60-300s Runtime

For performance evaluation, use these test sizes to achieve ~60-120s runtime
with Python interpreter:

| Benchmark | Argument | Python Time |
|-----------|----------|-------------|
| nbody.py | 5,000,000 | ~27s |
| nbody.py | 10,000,000 | ~53s |
| fibn.py | 40 | ~22s |
| fibn.py | 41 | ~46s |
| fibn.py | 42 | ~69s |

For the compiled binary, these values will take much longer until
optimization improves performance. Start with smaller values for development:
- nbody: n=50,000 (0.2s Python, ~3.6s compiled) → scale up after gains
- fibn: n=36 (3s Python, 104s compiled) → scale up after gains

---

## Optimization Progress

### Phase 1-11: Previous work
- Phase 1: Enable basic LLVM passes at opt=0
- Phase 2: Mark runtime functions as inline  
- Phase 3: Native list element storage and subscript path
- Phase 4-11: Various optimizations

### Phase 12: LTO via Bitcode Loading

LLVM's LTO optimization is enabled by loading precompiled Runtime.cpp as LLVM bitcode
and linking it into the compiled module before optimization passes run.

**Changes:**
- `include/pyc/object_struct.h` - unified PyObject struct definition for LLVM/CodeGen compatibility
- `Runtime.cpp` - uses shared PyObject struct (removed duplicate definition)
- `CMakeLists.txt` - compiles Runtime.cpp to `runtime.bc` at build time, links it
- `Codegen.cpp` - loads runtime bitcode into LLVM module, links with `--allow-multiple-definition`
- `Compiler.cpp` - calls `linkRuntimeBitcode()` before optimization when `opt>=1`

**Result:**
| Benchmark | Before LTO | After LTO | Gap to Python |
|-----------|-----------|-----------|---------------|
| nbody 50K | 4.3s | **0.44s** | 14x → **1.6x** |

**Why it works:** With LTO, LLVM can inline `PyFloat_FromDouble`, `Py_DECREF`, etc.
into the compiled module, eliminating call overhead and enabling cross-boundary optimizations.

### Phase 13: Native Float Chain

Extended the existing native int locals (A5) to support **proven-float locals** via
a parallel tracking mechanism that keeps float values as LLVM IR `double` types
instead of boxing them into PyObject*.

**Mechanism:**

1. **Compiler tracking**: `numericFloatLocals` set in Compiler.cpp (mirrors `numericLocals`)
   ```cpp
   // In noteType()
   if (type == "float") numericFloatLocals.insert(name);
   ```

2. **Type provenance**: When `numericResultType(op, left, right)` returns "float",
   the result temp is added to `numericFloatLocals`

3. **Codegen allocation**: F64 alloca slots for float locals (mirrors i64 slots):
   ```cpp
   // In assign handler
   if (isFloatLocal && src->getType()->isDoubleTy()) {
       // Create f64 alloca, store native double, skip boxing
   }
   ```

4. **Native BinOps**: `emitNativeNumericBinary()` already supports float:
   - `inst.resultType == "float"` → native `fadd`/`fmul`/`fsub`
   - Loads operands as native doubles via `unboxToDouble`
   - Returns native LLVM `double` (not boxed)

**Result:**
| Benchmark | Before Float Chain | After Float Chain | Gap |
|-----------|-------------------|-------------------|-----|
| nbody 50K | 4.3s | **0.44s** | 14x → **1.6x** |
| nbody 5M | ~360s | **44.7s** | ~14x → **1.7x** |

**IR evidence:** The generated LLVM IR for nbody shows:
- 52 native `fmul`/`fadd`/`fsub` instructions (no boxing in loop)
- 17 `PyFloat_FromDouble` calls (only for constants)
- Zero boxing for float arithmetic within the tight loop

**Why it works:** The tight loop in nbody's `advance()` function computes:
```python
fx = dx * fx   # ← float chain: native fmul → native store
fy = dy * fy   # ← float chain: native fmul → native store  
g  = fx * fx + fy * fy  # ← float chain: native fadd/fmul
```
Each intermediate stays as LLVM IR `double` until it's needed at a subscript/
print boundary (where boxing is required). This eliminates ~80% of the
`PyFloat_FromDouble` allocations that dominated the original profile.

### Test Suite Status
All **300/300** tests pass (inline + file cases, including `nbody.py`, `hash.py`,
`builtins.py`, `builtins2.py`, `features.py`).

### Current Performance Status (Phases 1–26)

| Benchmark | Python | pyc | Gap | Notes |
|-----------|--------|-----|-----|-------|
| nbody 50K | ~0.24s | **~0.07s** | **~3.4× faster** | `--opt=1..3` (LTO); opt0 = true O0 debug |
| nbody 500K | ~2.33s | **~0.70s** | **~3.3× faster** | — |
| float_loop(2M) | ~0.27s | **~5 ms** | **~58× faster** | pure native float chain |
| fibn(28) | ~0.10s | **~4 ms** | **~27× faster** | native recursive specialization (A6+) |
| fibn(35) | ~2.05s | **~47 ms** | **~44× faster** | — |
| fibn(40) | ~22.4s | **~0.54s** | **~41× faster** | — |
| nbody 50K (Phase 24) | ~0.33s | ~0.54s | 1.6× slower | Pre-P0/P1 |
| nbody 50K (LTO baseline) | — | 4.3s | 15× slower | Pre-Phase 12 |

nbody energy matches CPython. Full opt-level tables: `PERFORMANCE_OPT_LEVELS.md`.
Profile notes: `PROFILE_NBODY.md`.

### Phase 25–26 summary

**P1 freelist** — thread-local recycle of plain int/float boxes (no malloc in hot profile).

**P0 structured unpack** — body/pair layouts through SYSTEM/PAIRS/defaults; safe
`list_float` handles; aug-assign native get/set.

**Phase 26 native chain + spine** — `f64assign`, native pow/`m1*mag`, `Unpack2/3`,
i64 list for-loops (`SizeI64` + `GetItemI64`). `@advance`: ~15× `fmul`, 0× `PyNumber_Multiply`.

### Phase 27: native recursive specialization

Turned the fibn bottleneck (4.3× slower than CPython) into a **~40× speedup** by making
recursive numeric functions fully native through the existing A6 specialization machinery:

1. **Param type inference from body** — pre-scan each function's AST to infer param types
   from numeric use contexts (BinOp/Compare with numeric constants, UnaryOp). Seeds
   `valueTypes` + `numericLocals` before body lowering so self-recursive call sites
   record native arg types and `generateSpecializedVariants()` produces a variant.

2. **Return type fixpoint** — infer a function's return type as a single-function fixpoint:
   non-recursive returns seed the type, self-recursive calls assume the current estimate
   and propagate. Enables `fn.returnType = "int"` for `fib` so call results within the body
   are typed int and the `add` of two recursive calls is native `i64 add` (not `PyNumber_Add`).

3. **Specialized variant native return** — variants with a proven numeric return type
   return i64/double directly (`define i64 @__specialized_fib_i`) instead of boxed PyObject*.
   Eliminates `PyInt_FromLong` on every return and enables fully native recursive chains.

4. **Specialized variant self-dispatch** — relaxed the A6 codegen guard so specialized
   variants can dispatch to specialized variants (including self-recursion), not just
   non-specialized functions.

5. **Dead funcval elimination** — skip `lowerExpr` for known direct callees in the late
   callee lowering path, eliminating dead `pyc_make_func` + `PyUnicode_FromString` calls
   per call site (4 string allocs + 2 func objects per recursion level removed).

6. **Native i1 icmp** — native numeric comparisons emit i1 results directly instead of
   boxing to `PyBool_New`, letting `br` use the i1 without a round-trip through
   box → load → compare. `getAsPyObject` boxes i1 lazily on escape.

**Result:** `__specialized_fib_i` is a fully native function: `icmp sle i64` → `br i1`,
`sub i64` → `call i64 @__specialized_fib_i` → `add i64` → `ret i64`. Zero boxing,
zero refcounting, zero runtime dispatch in the hot path. LLVM further optimizes it to
an accumulator-style tail-recursive loop.

### Opt levels (current)

- `--opt=0`: true O0 — no runtime bitcode LTO, no LLVM module passes (debug / raw IR).
- `--opt=1..3`: LTO then O1/O2/O3. Hot IR is already frontend-native, so 1–3 look similar on nbody.
  See `PERFORMANCE_OPT_LEVELS.md`.

### Remaining Work

1. Further cut `Py_DECREF` / box-unbox on coord loads
2. Dict insertion order (hash vs insertion)
3. Escape analysis / stack floats for ephemeral boxes
4. Extend native recursive specialization to more patterns (mutual recursion, float-returning)

### Optimization Infrastructure (Phases 15–26)

**Data structures in `IRFunction`:**
```cpp
containerElementTypes[var][idx]       // "float_list", "int_list", ...
subscriptElementTypes[var][idx]       // "float", "int", "list_float", ...
listElementTypes[var]                 // per-index mixed tuple layouts
structuredElementLayout[var]          // List[bodyLayout]
pairOfStructuredLayout[var]           // List[(body, body)]
returnContainerElementTypes / returnSubscriptElementTypes
paramTypes[]
```

**Propagation chain (nbody):**
```
BODIES dict values → body layout
  → .values() / list() → SYSTEM structuredElementLayout
  → combinations(SYSTEM) → PAIRS pairOfStructuredLayout
  → default params on advance
  → for-loop item → unpack → list_float handles + float scalars
  → GetItemDouble / SetItemDouble / fadd/fmul
```
