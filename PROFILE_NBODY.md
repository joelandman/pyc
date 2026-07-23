# nbody Profile — Phases 25–26

## Wall-clock (`--opt=2`; similar at 1/3; opt0 is true O0 debug)

| Workload | Python | pyc | Speedup |
|----------|-------:|----:|--------:|
| nbody 50k | ~0.24 s | **~0.07 s** | **~3.4×** |
| nbody 500k | ~2.33 s | **~0.70 s** | **~3.3×** |

Energy matches CPython. Opt-level sweep: `PERFORMANCE_OPT_LEVELS.md`.

## `@advance` IR (representative)

| Kind | Count | Notes |
|------|------:|-------|
| `fmul` / `fadd` / `fsub` | ~15 / 8 / 6 | hot arithmetic |
| `llvm.pow.f64` | 1 | `** -1.5` into native `dt*mag` |
| `PyNumber_Multiply` | **0** | |
| `PyList_Unpack3` / `Unpack2` | 6 / 1 | body / pair |
| `PyList_GetItemI64` | 2 | for-loop item only |
| `GetItemDouble` / `SetItemDouble` | 9 / 9 | vel/pos updates |
| `PyList_SizeI64` | 2 | native bounds |
| `Py_DECREF` | ~45–60 | was ~140 pre-P0 |
| `PyInt_FromLong` | ~1 | was 25+ |

## Landed techniques

1. Structured body/pair layouts + default/for-loop propagation  
2. Scalar int/float freelist (P1)  
3. `f64assign` / native double RHS → float locals  
4. Native mul when either operand is already double  
5. Bulk `Unpack2/3`; i64 for-loop index (`SizeI64` + `i64add`)  

## Why O1–O3 look the same

`--opt>=1`: runtime bitcode LTO then LLVM O1/O2/O3. `--opt=0`: true O0 (no LTO,
no module passes) for debug/IR dumps. Frontend already emits the native kernel;
higher opt cannot remove remaining refcount/structure cost once LTO is on.

## Remaining (optional)

- Fewer box/unbox round-trips on coord loads  
- Dict insertion order  
- SIMD / PGO on the double kernel  
