# pyc — Implementation Details

Design choices, known limitations, optimization status, and implementation notes.

## Design Choices of Omission

### `exec()` / `eval()` — Intentionally Unsupported
Security implications. No plan to implement.

### Full Import/Module System — Partial
Only same-directory `.py` modules are supported. External modules (`re`, `math`, etc.)
report clear ImportError. Real module loading would require a full Python runtime.

### `**kwargs` — Not Yet Implemented
Function calls support `*args` collection and call-site unpacking, but `**kwargs`
keyword expansion is not yet implemented.

### Full First-Class Function Objects — Partial
Functions have identity (`is`, `==`) and repr (`<function f at 0x...>`), but full
first-class object protocol (`__call__`, `__name__`, `__doc__` attributes) is not
implemented.

### Tail Call Optimization — Not Implemented
Recursive functions use O(n) stack space. No plan to implement TCO.

### Lazy Compilation — Not Implemented
All functions are compiled regardless of whether they are called. Compile time
scales with program size.

### JIT Compilation — Not Implemented
pyc is strictly AOT (Ahead-Of-Time). No runtime compilation or caching.

## Known Limitations

### Performance
- **Function parameters are still boxed**: Type tracking doesn't know parameter types at lowering
- **Mixed-type code falls back to boxed runtime path**: Native paths only trigger when `resultType` is proven numeric
- **Division by zero handling requires runtime call**: Native would produce inf/nan
- **`**` (power) for non-constant exponents uses boxed `Pyc_Pow`**
- **Runtime provides only a synthetic `sys` module**: Not a full import/module system

### Type System
- **Conservative type tracking**: `widenLoopTypes()` widens to "boxed" on any type divergence at loop back-edges
- **No flow-sensitive type inference**: Type tracking is intra-procedural only
- **No union types**: A variable is either one type or "boxed"

### Runtime
- **`PyObject` is a flat struct**: `{refcount, type, value(i64), dvalue(double), list, dict, str}`
- **Most values flow as boxed `PyObject*`**: Native paths are optimizations, not the default
- **Exceptions use setjmp/longjmp**: Raise pops the frame before the jump; handler dispatch happens in generated code
- **Callables dispatch through a registry**: `Pyc_Apply(token, list)` with `__apply__` adapters

### IR
- **Linear instruction list per function**: No CFG in IR; control flow is represented via labels and branches
- **Conservative result type metadata**: `int`, `float`, `bool`, `str`, or `boxed` — not a full type lattice
- **No SSA form**: Variables are stored in alloca slots

### Optimization Status

#### Landed (A1–A7)
- **A1**: Conservative type tracking with loop back-edge widening
- **A2**: Native `for ... in range(...)` with unboxed i64 loop variables
- **A3**: Native arithmetic for proven numeric `+ - * // % **` and unary minus
- **A4**: Homogeneous numeric lists with native `int64`/`double` element storage
- **A5**: Allocation sinking for numeric locals (native i64 alloca)
- **A6**: Specialized function variants from proven call-site types
- **A7**: Allocation counters and microbenchmark guardrails

#### Planned (not implemented)
- IR-level constant folding
- LLVM aggressive inlining
- Arena allocator for PyObject
- Dead code elimination at IR level
- Type-based specialization for common patterns
- Lazy compilation for unused functions
- LLVM `opt` with aggressive flags

### Correctness Guarantees
- Every optimization preserves a boxed fallback path
- Native paths only trigger when `resultType` is proven numeric
- Mixed types fall back to boxed `PyNumber_*` calls
- Division by zero uses runtime call with proper Python semantics
- `getAsPyObject()` ensures values are properly boxed when they escape native context

## Architecture

```
Python source
    │  Python C API (ast.parse)
    ▼
ASTNode tree  (PythonParser.cpp)
    │  LoweringVisitor
    ▼
ModuleIR  (IR.h / IR.cpp)
    │  Codegen::generate
    ▼
llvm::Module  (Codegen.cpp)
    │  LLVM passes (O0–O3)
    ▼
.o object file
    │  clang++ + Runtime.cpp
    ▼
native executable
```

### Runtime (`src/runtime/Runtime.cpp`)
Standalone C++ file, no CPython dependency. Provides:
- `PyObject` (flat struct: `refcount`, `type`, `value`/`dvalue`/`list`/`dict`/`str`/`cell_content`)
- Refcounting, arithmetic, comparison, print, and all builtins
- Types: int, list, dict, str, float, bool/None, cell, super proxy, compiled regex,
  match object, exception, function, exception class, complex
- Exceptions use setjmp/longjmp frames
- Callables dispatch through a registry of `__apply__` adapters (`Pyc_Apply`)
- Linked into every compiled binary

### IR (`include/pyc/IR.h`, `src/ir/IR.cpp`)
Linear instruction list per function. Instructions:
- `const`, `fconst`, `bconst`, `nconst` — constants
- `assign` — variable assignment
- `add`/`sub`/`mul`/`div`/`truediv`/`mod`/`pow` — arithmetic
- `icmp` — integer comparison
- `br`, `label` — control flow
- `call` — function call
- `ret` — function return

IR instructions carry conservative result type metadata (`int`, `float`, `bool`, `str`, `boxed`).

## Testing

### Test Suite
- `tests/runner.py`: 300 inline test cases + file-based regression tests
- Each case compiled and compared against CPython output
- File cases: `tests/opt_*.py`, `tests/nbody.py`, `tests/fib*.py`, `tests/builtins*.py`, etc.

### Running Tests
```bash
cd build && make check   # or: ctest
PYC_BINARY=./build/pyc python3 tests/runner.py
```

### Benchmarking
```bash
# N-Body benchmark
python3 tests/nbody.py 5000000
./build/pyc tests/nbody.py -o nbody_compiled --opt=2
./nbody_compiled 5000000

# Profiling
perf record /tmp/nbody_compiled 5000000
perf report
```

## Development History

Initially scaffolded with Grok (xAI). Extended substantially with Claude (Anthropic).
See `GROK.md` for early history.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

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
