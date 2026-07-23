# nbody Profile — bulk unpack + native for-index

## Wall-clock (`--opt=2`)

| Workload | Python | pyc | Speedup |
|----------|-------:|----:|--------:|
| nbody 50k | ~0.28 s | **~0.07 s** | **~4.0×** |
| nbody 500k | ~2.53 s | **~0.71 s** | **~3.6×** |

Energy matches CPython.

## `@advance` IR

| Kind | Count | Notes |
|------|------:|-------|
| `fmul` / `fadd` / `fsub` | 15 / 8 / 6 | full hot arithmetic |
| `llvm.pow.f64` | 1 | `** -1.5` |
| `PyNumber_*` | **0** | |
| `PyList_Unpack3` | 6 | body triples |
| `PyList_Unpack2` | 1 | pair |
| `PyList_GetItemI64` | 2 | for-loop item only |
| `GetItemDouble` / `SetItemDouble` | 9 / 9 | |
| `PyList_SizeI64` | 2 | native for bounds |
| `Py_DECREF` | 45 | was ~140 at Phase 24 |
| `PyInt_FromLong` | 1 | was 25+ |

## This pass

1. **Native i64 for-loops** over lists: `PyList_SizeI64` + `i64` index + `i64add` + `GetItemI64(list, idx)` — eliminates boxed `idx+1`.
2. **`PyList_Unpack2` / `Unpack3`** — one call unpacks pair/body instead of 2–3 indexed gets.
3. Prior: f64 locals, native pow/mul chain, freelist, list_float aug-assign.

## Remaining (optional)

- Further cut `Py_DECREF` / `PyFloat_FromDouble` on coord loads (still boxed then unboxed)
- Dict insertion order
- Profile-guided / SIMD on the double kernel
