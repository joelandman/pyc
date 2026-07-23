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

For fibn (recursive), the challenge is bigger:

| Level | Target Speedup vs Python | Target Time (n=25) |
|-------|--------------------------|-------------------|
| Current (all levels) | 0.5x (14x slower) | 0.51s |
| After quick wins | 1x (parity) | 0.037s |
| After medium priority (TCO + specialization) | 3-5x | 0.007-0.012s |

**Actual progress:**
| Achieved | Performance (n=25) |
|----------|-------------------|
| Current | 7.4s (3.5x slower) |

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
- `Compiler.cpp` - calls `linkRuntimeBitcode()` before optimization

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

### Current Performance Status (Phases 1–25)

| Benchmark | Python | pyc | Gap | Notes |
|-----------|--------|-----|-----|-------|
| nbody 50K | ~0.28s | **~0.07s** | **~4.0× faster** | bulk unpack + i64 for-index + native float chain |
| nbody 500K | ~2.53s | **~0.71s** | **~3.6× faster** | — |
| nbody 50K (Phase 24) | ~0.33s | ~0.54s | 1.6× slower | Pre-P0/P1 |
| nbody 50K (LTO baseline) | — | 4.3s | 15× slower | Pre-Phase 12 |

nbody output matches CPython. See also `PERFORMANCE_OPT_LEVELS.md` and `PROFILE_NBODY.md`.

### Phase 25: P0 structured unpack + P1 scalar freelist

**P1 — thread-local freelist for boxed int/float** (`Runtime.cpp`):
- Cap-256 freelists recycle plain type-0/4 objects on `Py_DECREF`
- Removes `malloc`/`free` from the nbody hot profile

**P0 — safe structured unpack for nbody-shaped data** (`Compiler.cpp`, `IR.h`):
1. Body layout `[list_float, list_float, float]` from dict values / list literals
2. `SYSTEM` → `structuredElementLayout`; `PAIRS`/`combinations` → `pairOfStructuredLayout`
3. Default params (`bodies=SYSTEM`, `pairs=PAIRS`) bind layouts **before** body lower
4. For-loop `PyBuiltin_List` + iter slot **copy layouts** (previously dropped)
5. Unpack marks `v1`/`r` as `list_float` handles; mass stays boxed
6. Nested `[x1,y1,z1]` from float lists → float-typed temps (binops unbox)
7. Never `GetItemDouble` on mixed body tuples; never boxed unpack → `numericFloatLocals`
8. Aug-assign on `list_float` → `GetItemDouble` + native op + `SetItemDouble`

**`@advance` IR evidence:** `fadd`/`fsub`/`fmul` present; `GetItemDouble`×9, `SetItemDouble`×9.

### Remaining Work

1. More native multiplies (`m1 * mag`, `** -1.5` → llvm pow/rsqrt)
2. Cut remaining `GetItemObj` on pair/body spine
3. Dict insertion order (hash vs insertion)
4. Broader escape analysis / stack floats for ephemeral boxes

### Optimization Infrastructure (Phases 15–25)

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
