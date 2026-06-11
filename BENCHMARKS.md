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

### Current Status

**CRITICAL ISSUE: Compiler crashes during Python initialization.**

The compiler crashes during `Py_Initialize()` or `Py_InitializeFromConfig()` when
trying to parse Python source code. This happens even with simple test files.

- The crash occurs in `PyObject_GetIter()` during Python type initialization
- This is likely a Python C API embedding issue
- The issue needs to be investigated before benchmarking can proceed

### Known Limitations (after fixing initialization)

Current known issues that impact performance:

- `import` / module system not implemented (sys.argv not supported)
- Tuple unpacking in nested loops may cause segfaults
- Excessive heap allocations due to PyObject* boxing
- High sys time due to memory allocation overhead

## Workaround (after fixing initialization)

As a workaround, use simplified versions of nbody that avoid:
- Tuple unpacking in nested for loops
- Module-level list/dict creation with complex expressions

Example: see `/tmp/nbody_simple.py` in development tests.

### Notes

- The benchmark uses 5,000,000 iterations for meaningful timing
- Python 3.14+ required (uses modern AST structure)
- Compiler flags: `--opt=2` or `--opt=3` for optimization

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
