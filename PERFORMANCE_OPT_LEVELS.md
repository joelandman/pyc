# Performance by Optimization Level

Measured wall-clock **execution** time (median of 3 runs). Compile time excluded.
Host date: 2026-07-22. Binary: `build/pyc`. Interpreter: system `python3`.

| Benchmark | Python | `--opt=0` | `--opt=1` | `--opt=2` | `--opt=3` | Best vs Python |
|-----------|-------:|----------:|----------:|----------:|----------:|---------------:|
| nbody(50k) | 0.274 s | 0.490 s | 0.503 s | 0.529 s | 0.519 s | 1.79× slower |
| fibn(28) | 0.107 s | 0.451 s | 0.501 s | 0.455 s | 0.513 s | 4.21× slower |
| numeric_loop(5M) | 0.828 s | 0.880 s | 0.803 s | 0.836 s | 0.824 s | **1.03× faster** |
| float_loop(2M) | 0.307 s | 0.072 s | 0.070 s | 0.073 s | 0.072 s | **4.38× faster** |
| func_calls(1M) | 0.140 s | 0.166 s | 0.159 s | 0.153 s | 0.161 s | 1.09× slower |
| list_sum(300k) | 0.062 s | 0.072 s | 0.072 s | 0.088 s | 0.080 s | 1.15× slower |
| nested_float_lists | 0.422 s | 0.393 s | 0.393 s | 0.387 s | 0.388 s | **1.09× faster** |

### Same data as ratios (pyc / Python; &lt;1 means faster than CPython)

| Benchmark | opt0 | opt1 | opt2 | opt3 |
|-----------|-----:|-----:|-----:|-----:|
| nbody(50k) | 1.79× | 1.84× | 1.93× | 1.89× |
| fibn(28) | 4.21× | 4.68× | 4.25× | 4.79× |
| numeric_loop(5M) | 1.06× | **0.97×** | 1.01× | 1.00× |
| float_loop(2M) | **0.23×** | **0.23×** | **0.24×** | **0.23×** |
| func_calls(1M) | 1.19× | 1.14× | 1.09× | 1.15× |
| list_sum(300k) | 1.15× | 1.16× | 1.41× | 1.29× |
| nested_float_lists | 0.93× | 0.93× | **0.92×** | 0.92× |

### Benchmark definitions

| Name | Workload |
|------|----------|
| nbody(50k) | `tests/nbody.py 50000` (CLBG n-body) |
| fibn(28) | `tests/fibn.py 28` (recursive fibonacci print loop) |
| numeric_loop(5M) | `for i in range(5_000_000): s += i*2 - i//3` |
| float_loop(2M) | tight float accumulate/multiply loop, 2M iters |
| func_calls(1M) | `add(s,i)` in a 1M loop |
| list_sum(300k) | list comp + sum via `for x in lst` |
| nested_float_lists | 100×3 float rows, 20k outer updates (nbody-like) |

### Notes

- LLVM levels map to `--opt=0..3`. Runtime bitcode LTO link runs at all levels, so
  opt0 is not “no optimization” in the historical sense — it still inlines runtime helpers.
- **opt level has little effect** on most rows: bottleneck is runtime/boxing policy, not LLVM IR shape.
- **float_loop** wins big from native float locals (`fadd`/`fmul` chain).
- **fibn** stays slow: recursive calls through boxed `Pyc_Apply`.
- **nbody** ~1.8× behind CPython; pair unpack + mixed tuple subscripts still limit native paths.
- list_sum with a second indexed pass after iteration can SIGSEGV on teardown (known issue); the table uses for-in sum only.
