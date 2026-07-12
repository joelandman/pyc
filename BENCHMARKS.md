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

## Current Optimizations (2026-07)

### A1-A2: Type Tracking and Unboxed Numeric Locals
- `valueTypes` tracking with loop widening for type stability
- Visible range loop variables use native i64 storage (no boxing per iteration)
- Escape points (calls, returns, containers) box on demand via `getAsPyObject`

### A3: Widen Native Arithmetic
- `+ - *` with proven `resultType=int|float` use native LLVM arithmetic
- `// %` have native i64 paths with Python floor semantics and zero-division fallback
- `**` for small constant exponents (0-8) peeled into repeated `mul`
- Unary minus uses native `neg` when operand is numeric
- Codegen intercepts boxed runtime calls when operands are native

### A4: Homogeneous Numeric Lists
- List comprehensions/literals create `list_int`/`list_float` when element type is proven numeric
- Native `PyList_GetItemInt64`/`PyList_GetItemDouble`/`PyList_SetItemInt64`/`PyList_SetItemDouble`
- Runtime printing fixed for homogeneous lists

### A5: Allocation Sinking
- `numericLocals` tracking for variables that stay numeric
- Native i64 alloca for numeric locals (no boxing cycle)
- Escape always boxes via `getAsPyObject`

### A6: Specialized Function Variants
- Call-site type tracking: `callSiteTypes` records argument types at each call site
- `generateSpecializedVariants()` creates native-param variants when all call sites use consistent numeric types
- Codegen registers variants with native LLVM types (i64/double)
- Call sites dispatch to specialized variants with native args (no boxing)
- Adapters skipped for specialized variants

## Known Limitations

Current known issues that impact performance:

- The runtime provides only a synthetic `sys` module for `sys.argv`, not a full
  import/module system
- Function parameters are still boxed (type tracking doesn't know parameter types at lowering)
- Mixed-type code falls back to boxed runtime path
- Division by zero handling requires runtime call (native would produce inf/nan)
- ** (power) for non-constant exponents uses boxed `Pyc_Pow`

## Profiling

Use `perf` or `strace` to identify bottlenecks:

```bash
perf record /tmp/nbody_compiled 5000000
perf report

strace -c /tmp/nbody_compiled 5000000
```

## Microbenchmarks

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

### Homogeneous List Test

```bash
echo 'lst = [i for i in range(1000000)]
s = 0
for x in lst:
    s += x
print(s)' > /tmp/list_test.py
python3 /tmp/list_test.py
./build/pyc /tmp/list_test.py -o /tmp/list_test --opt=2
/tmp/list_test
```

### Function Call Test

```bash
echo 'def add(a, b):
    return a + b

s = 0
for i in range(100000):
    s = add(s, i)
print(s)' > /tmp/call_test.py
python3 /tmp/call_test.py
./build/pyc /tmp/call_test.py -o /tmp/call_test --opt=2
/tmp/call_test
```
