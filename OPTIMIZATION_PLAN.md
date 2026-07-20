# pyc Optimization Plan

Multi-level optimization strategy for pyc compiler output binaries.
Each level encompasses all lower-level optimizations with additional passes.

## Current State

| optLevel | LLVM Level | Description |
|----------|------------|-------------|
| 0 | None | No optimization (debug builds) |
| 1 | O1 | Simple optimization |
| 2 | O2 | Standard optimization (default) |
| 3 | O3 | Aggressive optimization |

**Gap:** Levels 4-5 not implemented. Level 3 needs enhancement for advanced passes.

---

## Optimization Levels

### Level 0: No Optimization (`--opt=0`)
**Purpose:** Debug builds, correctness verification, IR inspection

**Passes:**
- None (raw LLVM IR from codegen)

**Characteristics:**
- Maximum debuggability
- Slowest execution
- Largest binary size
- Every IR instruction maps to runtime call

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

### Metrics to Track
- Execution time (relative to CPython)
- Compile time (absolute and relative)
- Binary size (absolute and relative)
- Memory usage (peak RSS)
- Instruction count (via perf)

### Benchmark Script
```bash
#!/bin/bash
# benchmark_all_levels.sh
for opt in 0 1 2 3 4 5; do
    echo "=== Level $opt ==="
    ./build/pyc tests/nbody.py -o nbody_l${opt} --opt=$opt
    time ./nbody_l${opt} 1000000
    du -h nbody_l${opt}
done
```

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

### Portability Risks
- **Risk:** SIMD optimizations only work on specific CPUs
- **Mitigation:** Runtime CPU detection; fallback paths

---

## Success Criteria

| Level | Speedup vs CPython | Compile Time Overhead | Binary Size Overhead |
|-------|-------------------|----------------------|---------------------|
| 0 | 1x (baseline) | 0% | 0% |
| 1 | 2x | < 10% | < 5% |
| 2 | 5x | < 40% | < 10% |
| 3 | 10x | < 100% | < 20% |
| 4 | 15x | < 200% | < 30% |
| 5 | 20x | < 400% | < 50% |

---

## References

- LLVM Optimization Pipeline: https://llvm.org/docs/Passes.html
- LLVM Optimization Options: https://llvm.org/docs/CommandGuide/llvm-opt.html
- PGO Guide: https://llvm.org/docs/LinkTimeOptimization.html
- Auto-Vectorization: https://llvm.org/docs/Vectorizers.html
