# Performance by Optimization Level

Measured wall-clock **execution** time (median of 3 runs; nbody 500k = 1 run).
Compile time excluded. Host: 2026-07-23. Binary: `build/pyc`. Interpreter: system `python3`.

## Results (post Phase 26)

| Benchmark | Python | `--opt=0` | `--opt=1` | `--opt=2` | `--opt=3` | Best vs Python |
|-----------|-------:|----------:|----------:|----------:|----------:|---------------:|
| nbody(50k) | 0.241 s | 0.071 s | 0.077 s | 0.074 s | 0.074 s | **3.39× faster** |
| nbody(500k) | 2.326 s | 0.701 s | 0.714 s | 0.721 s | 0.716 s | **3.32× faster** |
| fibn(28) | 0.101 s | 0.522 s | 0.469 s | 0.437 s | 0.435 s | 4.31× slower |
| numeric_loop(5M) | 0.730 s | 0.637 s | 0.647 s | 0.650 s | 0.660 s | **1.14× faster** |
| float_loop(2M) | 0.267 s | 4.8 ms | 5.6 ms | 5.2 ms | 4.6 ms | **~58× faster** |
| func_calls(1M) | 0.128 s | 0.136 s | 0.131 s | 0.128 s | 0.137 s | ~parity |
| list_sum(300k) | 0.063 s | 0.062 s | 0.062 s | 0.055 s | 0.060 s | **1.14× faster** |
| nested_float_lists | 0.324 s | 0.105 s | 0.108 s | 0.100 s | 0.107 s | **3.26× faster** |

### Ratio (pyc / Python; &lt;1 = faster than CPython)

| Benchmark | opt0 | opt1 | opt2 | opt3 |
|-----------|-----:|-----:|-----:|-----:|
| nbody(50k) | 0.29× | 0.32× | 0.31× | 0.31× |
| nbody(500k) | 0.30× | 0.31× | 0.31× | 0.31× |
| fibn(28) | 5.18× | 4.65× | 4.33× | 4.31× |
| numeric_loop(5M) | 0.87× | 0.89× | 0.89× | 0.90× |
| float_loop(2M) | 0.02× | 0.02× | 0.02× | 0.02× |
| func_calls(1M) | 1.07× | 1.03× | 1.00× | 1.07× |
| list_sum(300k) | 0.98× | 0.99× | 0.88× | 0.95× |
| nested_float_lists | 0.32× | 0.33× | 0.31× | 0.33× |

## Why `--opt=0..3` barely differs

Pipeline order:

```
codegen IR → linkRuntimeBitcode() → optimize(--opt=N) → object / final link -ON
```

### 1. LTO is always on

`Compiler::compile` always calls `linkRuntimeBitcode()` **before** `optimize()`.
Runtime helpers (`Py_DECREF`, freelist, `GetItemDouble`, …) are visible to LLVM at
every opt level, so the big inlining win is not gated on O2/O3.

### 2. `--opt=0` is not LLVM O0

In `Codegen::optimize()`:

| Flag | LLVM pipeline |
|------|----------------|
| `--opt=0` | **O1** (explicitly; “minimal” still runs O1) |
| `--opt=1` | O1 |
| `--opt=2` | O2 |
| `--opt=3` | O3 |

So **opt0 ≈ opt1** for the pass manager. Only 2/3 step up.

### 3. Hot path is already frontend-native

nbody’s `advance` is lowered to explicit `fmul`/`fadd`/`fsub`/`llvm.pow`,
`Unpack2/3`, i64 for-index, `GetItemDouble`/`SetItemDouble`. O2/O3 help most when
they must invent that shape from abstract calls; here the remaining cost is
refcount/structure work, not “unoptimized LLVM IR.”

### 4. What would make higher opt matter more

- A true O0 mode (skip bitcode LTO and/or run no module passes) for debug/compare
- Workloads still dominated by abstract `Pyc_Apply` / boxed calls (e.g. **fibn**)
  where extra inlining can still shave a little (see slight opt3 gain on fibn)

## Benchmark definitions

| Name | Workload |
|------|----------|
| nbody(50k) / (500k) | `tests/nbody.py N` |
| fibn(28) | `tests/fibn.py 28` |
| numeric_loop(5M) | `for i in range(5e6): s += i*2 - i//3` |
| float_loop(2M) | tight float accumulate/multiply, 2M iters |
| func_calls(1M) | `add(s,i)` in a 1M loop |
| list_sum(300k) | list comp + `for x in lst` sum |
| nested_float_lists | 100×3 float rows, 20k outer updates |

## Takeaways

- **nbody ~3.3× faster than CPython** at all opt levels after Phases 25–26.
- **float_loop ~58×** — pure native float chain.
- **fibn ~4× slower** — recursive boxed `Pyc_Apply`; not fixed by O3.
- Prefer **`--opt=2`** as default: same runtime as 0/1/3 here, standard pipeline.
