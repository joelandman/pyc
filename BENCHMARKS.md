# Performance Benchmarks

This document describes benchmarking methodology for the pyc compiler.

## Benchmark: N-Body Simulation

The `tests/nbody.py` file is used as a performance benchmark. It's an N-body
gravity simulation from the Computer Language Benchmarks Game.

### Running the Benchmark

```bash
# Python interpreter baseline
python3 tests/nbody.py 5000000

# Compiled binary
./build/pyc tests/nbody.py -o nbody_compiled --opt=2
./nbody_compiled 5000000
```

### Expected Output

```
-0.169075164
-0.169059907
```

### Performance Target

The compiler should achieve **50x speedup** over CPython for this benchmark.
That target is for the general compiler path, not benchmark-specific codegen.
Special-casing `tests/nbody.py` is intentionally avoided.

### Known Limitations

Current known issues that impact performance:

- The runtime provides only a synthetic `sys` module for `sys.argv`, not a full
  import/module system
- Excessive heap allocations due to PyObject* boxing
- High sys time due to memory allocation overhead
- Numeric list elements are still represented as boxed objects
- Numeric arithmetic in hot loops still allocates boxed result objects

## Current General Optimization Work

The compiler now lowers `for ... in range(...)` directly to loop control-flow
blocks instead of allocating a boxed range list. Hidden `range` loop counters
use native i64 compare/increment operations, while the Python-visible loop
variable remains boxed for correctness.

Proven numeric `+`, `-`, and `*` operations now lower to native LLVM arithmetic
and box the result afterward. Division remains on the boxed runtime path because
the runtime currently handles divide-by-zero by returning `NULL`; native floating
division would silently produce `inf`/`nan` and change behavior.

The next planned optimizations are:

1. Longer-lived unboxed numeric locals inside proven numeric regions.
2. Homogeneous numeric-vector list representations for `list[int]` and
   `list[float]`.
3. Specialized function variants selected from proven call-site argument types.
4. Allocation sinking for temporary boxed numbers that do not escape.

Every optimization should preserve the boxed fallback path for uncertain or
mixed-type code.

### Notes

- The benchmark uses 5,000,000 iterations for meaningful timing
- Python 3.14+ required (uses modern AST structure)
- Compiler flags: `--opt=2` or `--opt=3` for optimization
- `tests/runner.py` includes `tests/nbody.py 100` as a correctness regression
  for the general compiler path

## Additional Benchmarks

### Simple Loop Test

```bash
echo 'for i in range(10000000): x=i' > /tmp/loop_test.py
python3 /tmp/loop_test.py
./build/pyc /tmp/loop_test.py -o /tmp/loop_test --opt=2
/tmp/loop_test
```

### Arithmetic Intensive Test

```bash
echo 'x=0.0
for i in range(1000000):
    x += i * 0.5
    x *= 1.000001
print(x)' > /tmp/arithmetic_test.py
python3 /tmp/arithmetic_test.py
./build/pyc /tmp/arithmetic_test.py -o /tmp/arithmetic_test --opt=2
/tmp/arithmetic_test
```

## Profiling

Use `perf` or `strace` to identify bottlenecks:

```bash
perf record /tmp/nbody_compiled 5000000
perf report

strace -c /tmp/nbody_compiled 5000000
```
