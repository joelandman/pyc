# pyc Optimization Plan

Multi-level optimization strategy for pcy compiler output binaries.
Each level encompasses all lower-level optimizations with additional passes.

## Current State

| optLevel | LLVM Level | Description |
|----------|------------|-------------|
| 0 | None | True O0: no runtime bitcode LTO, no module passes (debug/IR) |
| 1 | O1 + LTO | Simple optimization + runtime bitcode LTO |
| 2 | O2 + LTO | Standard optimization (default) |
| 3 | O3 + LTO | Aggressive optimization |

**Gap:** Levels 4-5 not implemented. Level 3 needs enhancement for advanced passes.

---

## Optimization Levels

### Level 0: No Optimization (`--opt=0`)
**Purpose:** Debug builds, correctness verification, IR inspection

**Passes / link:**
- No `linkRuntimeBitcode()` (runtime stays external via libpycrt)
- No LLVM module pass pipeline
- Final link without `-flto`, `-O0`

**Characteristics:**
- Maximum debuggability; raw frontend IR via `--emit-llvm`
- Slowest execution among opt levels
- Test runner defaults to `--opt=0` for correctness

**Use cases:**
- Debugging compiler bugs
- Inspecting generated IR (`--emit-llvm`)
- Verifying correctness before optimization

---

### Level 1: Simple Optimization (`--opt=1`)
**Purpose:** Basic performance improvement with minimal compile time

**Encompasses:** Level 0 +

**Passes:**
1. **Dead Code Elimination (DCE)**
   - Remove unused instructions
   - Remove unreachable basic blocks
   - Remove unused function definitions

2. **Constant Propagation**
   - Fold constant expressions at IR level
   - Replace dead stores with constants

3. **Simple Inlining**
   - Inline small functions (< 5 instructions)
   - Heuristic: inline if callee size < 3x caller call site

4. **Common Subexpression Elimination (CSE)**
   - Detect repeated computations
   - Hoist to single computation

5. **Basic Loop Optimizations**
   - Loop-invariant code motion (simple cases)
   - Basic dead branch elimination

**Expected speedup:** 1.5-2x over Level 0

**Compile time impact:** Minimal (< 10%)

---

### Level 2: Standard Optimization (`--opt=2`) — DEFAULT
**Purpose:** Good performance with reasonable compile time

**Encompasses:** Level 1 +

**Passes:**
1. **All Level 1 passes**

2. **Aggressive Inlining**
   - Inline functions with known call patterns
   - Profile-guided inlining hints
   - Inline threshold: callee size < 10x caller

3. **Full Loop Optimizations**
   - Loop-invariant code motion (full)
   - Loop unrolling (small factors: 2x, 4x)
   - Loop fusion where beneficial
   - Redundant load elimination

4. **Global Value Numbering (GVN)**
   - Expressions equivalence detection
   - Cross-basic-block CSE

5. **Function-Specific Optimizations**
   - Tail call optimization
   - Dead argument elimination
   - Constant argument propagation

6. **Target-Specific Optimizations**
   - X86-specific instruction selection
   - Use of SIMD instructions for simple vector ops
   - Hardware-specific instruction scheduling

**Expected speedup:** 3-5x over Level 0, 2-3x over Level 1

**Compile time impact:** Moderate (20-40%)

**LLVM mapping:** `O2` optimization level

---

### Level 3: Advanced Optimization (`--opt=3`)
**Purpose:** Maximum performance for compute-intensive workloads

**Encompasses:** Level 2 +

**Passes:**
1. **All Level 2 passes**

2. **Aggressive Loop Optimizations**
   - Loop unrolling (large factors: 8x, 16x) with runtime guards
   - Loop interchange for better cache locality
   - Loop tiling/blocking for matrix operations
   - Full loop vectorization (auto-vectorization)

3. **Advanced Constant Folding**
   - Compile-time evaluation of complex expressions
   - String constant folding and interning
   - List/dict literal folding

4. **Whole-Program Analysis**
   - Cross-function constant propagation
   - Interprocedural analysis
   - Dead code elimination across function boundaries

5. **Memory Optimizations**
   - Escape analysis for allocation sinking
   - Scalar replacement of aggregates
   - Stack allocation of heap objects where possible

6. **Branch Optimizations**
   - Profile-based branch prediction hints
   - Basic block layout optimization
   - Indirect branch elimination

7. **Hardware-Specific Optimizations**
   - AVX2/AVX-512 vectorization for numeric code
   - FMA (Fused Multiply-Add) instruction usage
   - Prefetch hints for data-intensive loops

**Expected speedup:** 5-10x over Level 0, 2-3x over Level 2

**Compile time impact:** High (50-100%)

**LLVM mapping:** `O3` optimization level + custom passes

---

### Level 4: Intensive Optimization (`--opt=4`)
**Purpose:** Maximum performance for production deployments

**Encompasses:** Level 3 +

**Passes:**
1. **All Level 3 passes**

2. **Advanced SIMD Optimization**
   - Manual SIMD intrinsics for hot loops
   - Auto-vectorization with target features
   - SIMD-friendly data structure layout
   - Vectorized string operations

3. **Profile-Guided Optimization (PGO)**
   - Instrument compilation with profiling
   - Run instrumented binary with representative workload
   - Recompile with profile data
   - Optimize hot paths, cold paths, branch prediction

4. **Link-Time Optimization (Basic LTO)**
   - Cross-module inlining
   - Whole-program dead code elimination
   - Cross-module constant propagation

5. **Cache Optimizations**
   - Data structure layout for cache efficiency
   - Structure of Arrays (SoA) transformation
   - Prefetch insertion for sequential access patterns

6. **Instruction-Scheduling Optimizations**
   - Register allocation optimization
   - Instruction pipelining
   - Avoiding pipeline stalls

**Expected speedup:** 8-15x over Level 0, 2-3x over Level 3

**Compile time impact:** Very high (100-200%)

**LLVM mapping:** `O3` + PGO + LTO (thin-LTO)

---

### Level 5: Maximum Optimization (`--opt=5`)
**Purpose:** Maximum possible performance, production releases

**Encompasses:** Level 4 +

**Passes:**
1. **All Level 4 passes**

2. **Full Link-Time Optimization (Full LTO)**
   - Whole-program optimization across all modules
   - Cross-module inlining and devirtualization
   - Whole-program dead code elimination
   - Whole-program constant propagation

3. **Auto-Vec with Target Features**
   - Detect target CPU features at compile time
   - Generate multiple code paths (multi-versioning)
   - Runtime CPU dispatch for optimal path selection

4. **Specialization and Monomorphization**
   - Type-based specialization for hot functions
   - Monomorphize generic patterns
   - Generate specialized variants for common call patterns

5. **Extreme Loop Optimizations**
   - Aggressive loop unrolling with runtime guards
   - Loop strip mining for large iterations
   - Multi-versioned loop implementations
   - Software pipelining

6. **Advanced Memory Optimizations**
   - Aggressive escape analysis
   - Stack allocation of all local objects
   - Custom allocator integration for hot paths

7. **Binary Size Optimizations**
   - Function splitting (thin-static-LTO)
   - Remove unused code at fine granularity
   - Compressed code generation where beneficial

**Expected speedup:** 10-20x over Level 0, 2-3x over Level 4

**Compile time impact:** Extreme (200-400%)

**LLVM mapping:** `O3` + Full LTO + PGO + multi-versioning

---

## Implementation Plan

### Phase 1: Foundation (Weeks 1-2)
- [ ] Implement Level 1 passes (DCE, constant propagation, simple inlining)
- [ ] Add pass configuration infrastructure
- [ ] Update `Codegen::optimize()` to support new levels
- [ ] Add tests for each optimization level

### Phase 2: Advanced Passes (Weeks 3-4)
- [ ] Implement Level 3 passes (aggressive loop opts, constant folding)
- [ ] Add GVN and cross-basic-block optimizations
- [ ] Implement escape analysis foundation
- [ ] Add hardware-specific optimization hooks

### Phase 3: LTO and PGO (Weeks 5-6)
- [ ] Implement thin-LTO for Level 4
- [ ] Add PGO infrastructure (instrumentation + profile use)
- [ ] Implement full LTO for Level 5
- [ ] Add multi-versioning support

### Phase 4: SIMD and Specialization (Weeks 7-8)
- [ ] Implement SIMD auto-vectorization hooks
- [ ] Add manual SIMD intrinsics for hot paths
- [ ] Implement type-based specialization
- [ ] Add CPU dispatch infrastructure

### Phase 5: Testing and Tuning (Weeks 9-10)
- [ ] Benchmark all optimization levels
- [ ] Tune pass thresholds and parameters
- [ ] Add regression tests for optimization correctness
- [ ] Document optimization levels and trade-offs

---

## Testing Strategy

### Correctness Tests
- All 300 existing tests must pass at ALL optimization levels
- No correctness regressions allowed
- Test with `--opt=0` through `--opt=5`

### Performance Benchmarks
- N-Body simulation (`tests/nbody.py`)
- Microbenchmarks (loop, arithmetic, list, function call)
- Real-world programs (`tests/fib.py`, `tests/builtins.py`, etc.)

### Memory Testing (First-Class Concern)

Memory usage and leaks are treated as a first-class problem. No run-away allocations allowed.

#### Memory Metrics to Track
- **Peak RSS** (Resident Set Size) - maximum memory used
- **Memory Leaks** - allocated but never freed memory
- **Allocation Count** - total number of PyObject allocations
- **Allocation Rate** - allocations per second
- **Memory Growth** - memory usage over time (detect leaks)

#### Memory Leak Detection

**Tool 1: Valgrind (Development)**
```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
    ./build/pyc tests/nbody.py -o nbody_test --opt=2
./nbody_test 10000
```

**Tool 2: AddressSanitizer (Development)**
```bash
# Rebuild with ASan
cmake .. -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
make -j$(nproc)

# Run with ASan
./build/pyc tests/nbody.py -o nbody_asan --opt=2
./nbody_asan 10000
```

**Tool 3: Custom Allocation Counters (Production)**
```cpp
// Runtime provides allocation counters in runtime.h
extern "C" int64_t PyAlloc_GetTotal();
extern "C" int64_t PyAlloc_GetIntCount();
extern "C" int64_t PyAlloc_GetListCount();
extern "C" int64_t PyAlloc_GetDictCount();
extern "C" int64_t PyAlloc_GetStrCount();

// After program execution, check for leaks:
// - Total allocations should match expected count
// - No unbounded growth during loops
// - All allocations properly DECREFed
```

#### Memory Limits

| Optimization Level | Max Peak RSS | Max Leak | Max Allocation Count (nbody 1M iterations) |
|-------------------|--------------|----------|-------------------------------------------|
| 0 | 100 MB | 0 bytes | 500,000 |
| 1 | 100 MB | 0 bytes | 400,000 |
| 2 | 150 MB | 0 bytes | 300,000 |
| 3 | 200 MB | 0 bytes | 200,000 |
| 4 | 300 MB | 0 bytes | 150,000 |
| 5 | 500 MB | 0 bytes | 100,000 |

**Note:** Higher optimization levels may use more memory due to:
- Larger binaries (more inlined code)
- More aggressive constant folding (more constants in memory)
- LTO (whole-program optimization data)
- PGO (profile data in memory)

But **memory leaks are never acceptable** at any level.

#### Memory Testing Script

```bash
#!/bin/bash
# memory_test.sh - Test memory usage across optimization levels
set -e

TEST_FILE="tests/nbody.py"
ITERATIONS=100000
MAX_RSS_KB=512000  # 500 MB max

for opt in 0 1 2 3 4 5; do
    echo "=== Testing Level $opt ==="
    
    # Compile
    ./build/pyc "$TEST_FILE" -o nbody_l${opt} --opt=$opt
    
    # Run with memory tracking
    /usr/bin/time -v ./nbody_l${opt} $ITERATIONS 2> memory_stats.txt
    
    # Extract peak RSS (KB)
    PEAK_RSS_KB=$(grep "Maximum resident set size" memory_stats.txt | awk '{print $NF}')
    echo "Peak RSS: ${PEAK_RSS_KB} KB"
    
    # Check against limit
    if [ "$PEAK_RSS_KB" -gt "$MAX_RSS_KB" ]; then
        echo "ERROR: Peak RSS ${PEAK_RSS_KB} KB exceeds limit ${MAX_RSS_KB} KB"
        exit 1
    fi
    
    # Run with Valgrind for leak detection (Level 0 only, too slow for others)
    if [ "$opt" -eq 0 ]; then
        echo "Running Valgrind leak check..."
        valgrind --leak-check=full --error-exitcode=1 \
            ./nbody_l${opt} $ITERATIONS > /dev/null
    fi
    
    echo "Level $opt: PASS"
done

echo "All memory tests passed!"
```

#### Memory Regression Detection

**Automated Memory Testing:**
```bash
# In CI/CD pipeline
./memory_test.sh

# Git hook to prevent memory regressions
# Pre-commit: run memory tests on changed files
# Pre-push: run full memory test suite
```

**Memory Profiling:**
```bash
# Track memory over time
valgrind --tool=massif ./nbody_l2 1000000
ms_print massif.out.<PID>

# Heap profiling
heaptrack ./nbody_l2 1000000
heaptrack_print heaptrack.<PID>.<PID>.gz
```

### Metrics to Track

| Metric | Description | Target |
|--------|-------------|--------|
| Execution time | Wall-clock time | See success criteria below |
| Compile time | Compiler runtime | See success criteria below |
| Binary size | Output file size | See success criteria below |
| Peak RSS | Maximum memory used | See memory limits table |
| Memory leaks | Allocated but unfreed bytes | **0 bytes at all levels** |
| Allocation count | Total PyObject allocations | Decreasing with optimization |
| Instruction count | CPU instructions executed | Decreasing with optimization |

---

## Benchmarking

### N-Body Simulation

The `tests/nbody.py` file is used as a performance benchmark. It's an N-body
gravity simulation from the Computer Language Benchmarks Game.

```bash
# Python interpreter baseline
python3 tests/nbody.py 5000000

# Compiled binary
./build/pyc tests/nbody.py -o nbody_compiled --opt=2
./nbody_compiled 5000000
```

**Expected output:**
```
-0.169075164
-0.169059907
```

### Profiling

```bash
perf record /tmp/nbody_compiled 5000000
perf report
strace -c /tmp/nbody_compiled 5000000
```

### Microbenchmarks

**Simple Loop Test:**
```bash
echo 'for i in range(10000000): x=i' > /tmp/loop_test.py
python3 /tmp/loop_test.py
./build/pyc /tmp/loop_test.py -o /tmp/loop_test --opt=2
/tmp/loop_test
```

**Arithmetic Intensive Test:**
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

**Homogeneous List Test:**
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

**Function Call Test:**
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

### Allocation Counters (A7)

The runtime tracks allocations per type via atomic counters:

- `PyAlloc_GetIntCount()` — `PyInt_FromLong` allocations (excludes small int cache)
- `PyAlloc_GetFloatCount()` — `PyFloat_FromDouble` allocations
- `PyAlloc_GetListCount()` — `PyList_New` allocations
- `PyAlloc_GetDictCount()` — `PyDict_New` allocations
- `PyAlloc_GetStrCount()` — `PyUnicode_FromString` allocations
- `PyAlloc_GetTotal()` — sum of all above

These counters are exposed via `extern "C"` functions in `runtime.h` for external measurement.

---

## Success Criteria

| Level | Speedup vs CPython | Compile Time Overhead | Binary Size Overhead | Peak RSS Limit | Memory Leaks |
|-------|-------------------|----------------------|---------------------|----------------|--------------|
| 0 | 1x (baseline) | 0% | 0% | 100 MB | **0 bytes** |
| 1 | 2x | < 10% | < 5% | 100 MB | **0 bytes** |
| 2 | 5x | < 40% | < 10% | 150 MB | **0 bytes** |
| 3 | 10x | < 100% | < 20% | 200 MB | **0 bytes** |
| 4 | 15x | < 200% | < 30% | 300 MB | **0 bytes** |
| 5 | 20x | < 400% | < 50% | 500 MB | **0 bytes** |

**Memory leaks are never acceptable at any optimization level.**

---

## Risk Mitigation

### Correctness Risks
- **Risk:** Optimizations introduce bugs
- **Mitigation:** All 300 tests must pass at each level; fuzz testing

### Compile Time Risks
- **Risk:** Levels 4-5 take too long to compile
- **Mitigation:** Progressive compilation; cache intermediate results

### Binary Size Risks
- **Risk:** Optimizations increase binary size
- **Mitigation:** Function splitting; measure and monitor size

### Memory Risks
- **Risk:** Memory leaks or run-away allocations
- **Mitigation:** 
  - Valgrind/ASan in CI for every commit
  - Memory limits enforced in automated tests
  - Allocation counters in runtime for leak detection
  - Memory regression tests before merge

### Portability Risks
- **Risk:** SIMD optimizations only work on specific CPUs
- **Mitigation:** Runtime CPU detection; fallback paths

---

## References

- LLVM Optimization Pipeline: https://llvm.org/docs/Passes.html
- LLVM Optimization Options: https://llvm.org/docs/CommandGuide/llvm-opt.html
- PGO Guide: https://llvm.org/docs/LinkTimeOptimization.html
- Auto-Vectorization: https://llvm.org/docs/Vectorizers.html
- Valgrind: https://valgrind.org/
- AddressSanitizer: https://github.com/google/sanitizers/wiki/AddressSanitizer
- Massif/Heaptrack: Memory profiling tools
