# Performance by Optimization Level

Measured wall-clock **execution** time (median of 3 runs; nbody 500k = 1 run).
Compile time excluded. Host: 2026-07-23. Binary: `build/pyc`. Interpreter: system `python3`.

## Results (post Phase 26; pre true-O0)

> Table below was measured when `--opt=0` still ran LTO + LLVM O1. After true O0,
> re-measure opt0 separately; opt1–3 should match these numbers.

| Benchmark | Python | `--opt=0`* | `--opt=1` | `--opt=2` | `--opt=3` | Best vs Python |
|-----------|-------:|----------:|----------:|----------:|----------:|---------------:|
| nbody(50k) | 0.241 s | 0.071 s | 0.077 s | 0.074 s | 0.074 s | **3.39× faster** |
| nbody(500k) | 2.326 s | 0.701 s | 0.714 s | 0.721 s | 0.716 s | **3.32× faster** |
| fibn(28) | 0.101 s | 0.522 s | 0.469 s | 0.437 s | 0.435 s | 4.31× slower |
| numeric_loop(5M) | 0.730 s | 0.637 s | 0.647 s | 0.650 s | 0.660 s | **1.14× faster** |
| float_loop(2M) | 0.267 s | 4.8 ms | 5.6 ms | 5.2 ms | 4.6 ms | **~58× faster** |
| func_calls(1M) | 0.128 s | 0.136 s | 0.131 s | 0.128 s | 0.137 s | ~parity |
| list_sum(300k) | 0.063 s | 0.062 s | 0.062 s | 0.055 s | 0.060 s | **1.14× faster** |
| nested_float_lists | 0.324 s | 0.105 s | 0.108 s | 0.100 s | 0.107 s | **3.26× faster** |

\* Historical: opt0 ≈ opt1 (LTO + O1). **True O0** (current): no LTO, no LLVM passes — expect slower.

### Ratio (pyc / Python; &lt;1 = faster than CPython)

| Benchmark | opt0* | opt1 | opt2 | opt3 |
|-----------|-----:|-----:|-----:|-----:|
| nbody(50k) | 0.29× | 0.32× | 0.31× | 0.31× |
| nbody(500k) | 0.30× | 0.31× | 0.31× | 0.31× |
| fibn(28) | 5.18× | 4.65× | 4.33× | 4.31× |
| numeric_loop(5M) | 0.87× | 0.89× | 0.89× | 0.90× |
| float_loop(2M) | 0.02× | 0.02× | 0.02× | 0.02× |
| func_calls(1M) | 1.07× | 1.03× | 1.00× | 1.07× |
| list_sum(300k) | 0.98× | 0.99× | 0.88× | 0.95× |
| nested_float_lists | 0.32× | 0.33× | 0.31× | 0.33× |

## Pipeline

```
codegen IR
  → [opt>=1] linkRuntimeBitcode()
  → [opt>=1] optimize(O1/O2/O3)
  → object
  → final link  (-flto=thin only if opt>=1; -O0 / -O1 / -O2 / -O3)
```

| Flag | Runtime bitcode LTO | LLVM module passes | Final link |
|------|---------------------|--------------------|------------|
| `--opt=0` | **off** | **none** (true O0) | no `-flto`, `-O0` |
| `--opt=1` | on | O1 | `-flto=thin -O1` |
| `--opt=2` | on | O2 | `-flto=thin -O2` (default) |
| `--opt=3` | on | O3 | `-flto=thin -O3` |

Use **`--opt=0 --emit-llvm`** to inspect raw frontend IR (external `Py_*` calls, no inlined runtime).

### Why opt1–3 still look similar on nbody

Hot path is already frontend-native (`fmul`/`llvm.pow`, Unpack2/3, i64 for-index,
GetItemDouble). Remaining cost is refcount/structure; O2/O3 add little once LTO
has inlined runtime helpers.

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

- Prefer **`--opt=2`** for release.
- Use **`--opt=0`** for debugging and IR dumps (true O0, no LTO).
- **fibn** remains ~4× slower (boxed recursion); not fixed by O3.
