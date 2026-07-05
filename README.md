# pyc

An AOT (Ahead-Of-Time) compiler for a substantial Python subset. Parses Python
source using the Python C API, lowers the AST through a visitor-based IR, generates
LLVM IR, optimizes it, and produces standalone native executables via a minimal
`PyObject*`-based boxed runtime with refcounting.

Written in C++ with Clang++ and LLVM 18. No C/C++ intermediate language for
the normal compiler path. **216 tests passing** (208 curated + 8 file-based) against CPython output.

## Build

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

## Usage

```bash
pyc hello.py -o hello          # compile
pyc hello.py -o hello --static # fully static binary
pyc hello.py --emit-llvm       # dump LLVM IR
pyc hello.py --opt=2 -o hello  # with O2 optimisation
./hello
```

Flags: `--static`, `--opt=0|1|2|3`, `-o output`, `--emit-llvm`, `--emit-asm`/`-S`, `--verbose`.

## Testing

```bash
cd build && make check   # or: ctest
```

The test suite in `tests/runner.py` compiles each case and compares output
against CPython. It also runs file-based regression programs in `tests/` that
cover optimization-sensitive behavior such as native `range` loops, numeric
locals, numeric list mutation and aliasing, command-line arguments, default
arguments, nested destructuring, and `tests/nbody.py`.

## What compiles today

### Types
- **int** — arbitrary arithmetic, comparison, floor/true division
- **float** — `3.14`, `1.5e-3`, mixed int/float arithmetic
- **bool** — `True`/`False`, prints correctly, arithmetic with ints works
- **str** — literals, concatenation (`+`), repetition (`*`), `len()`, `str()`,
  f-strings `f"x={x}"`, `upper()`/`lower()`/`strip()`/`split()`/`join()`, `% formatting`
- **list** — literals, subscript get/set, slicing, `append()`, `sort()`, `pop()`,
  `len()`, list comprehensions `[x*2 for x in range(n) if cond]`
- **dict** — literals, subscript get/set, `keys()`/`values()`/`items()`
- **tuple** — literals (mapped to list internally), unpacking `a, b = 1, 2`
- **None** — `x = None`, `print(None)`

### Operators
- Arithmetic: `+ - * / // % **`
- Comparison: `== != < > <= >=`, `is`, `in`, `not in`, chained `1 < x < 10`
- Boolean: `and`, `or`, `not`
- Unary: `-x`, `+x`
- Augmented assign: `+= -= *= /= //= %= **=` (including on subscripts `a[i] += 1`)

### Control flow
- `if` / `elif` / `else`
- `while` with `break` / `continue`
- `for` loop over lists and other list-like iterables
- `for` loop over `range(...)` lowered without materializing a boxed range list
- Recursive tuple/list destructuring in loop targets
- Ternary `x if cond else y`

### Functions
- `def` with positional args, default args, keyword args (`f(b=3, a=4)`)
- `return` (including multiple return values `return a, b`)
- Nested functions
- `global x` declaration (shared module-level storage)
- `try` / `except` basic

### Builtins
`print()` (multi-arg), `range()`, `len()`, `str()`, `int()`, `float()`, `abs()`,
`min()`, `max()`, `list()`, `enumerate()`, `zip()`,
`sum()`, `sorted()`, `any()`, `all()`, `isinstance()`

### Assignment
- Simple: `x = 1`
- Multi-target: `a = b = 5`
- Tuple unpack: `a, b = 1, 2`
- Subscript: `a[i] = v`, `d[k] = v`

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

**Runtime** (`src/runtime/Runtime.cpp`): standalone C++ file, no CPython
dependency. Provides `PyObject` (flat struct: `refcount`, `type`, `value`/`dvalue`/
`list`/`dict`/`str`), refcounting, arithmetic, comparison, print, and all builtins.
Linked into every compiled binary.

**IR**: linear instruction list per function. Instructions: `const`, `fconst`,
`bconst`, `assign`, `add`/`sub`/`mul`/`div`/`truediv`/`mod`/`pow`, `icmp`, `br`,
`label`, `call`, `ret`. IR instructions can carry conservative result type
metadata (`int`, `float`, `bool`, `str`, or `boxed`). Codegen uses that metadata
for specific general native paths, such as range loop counters and numeric
`+`/`-`/`*`, and otherwise falls back to boxed `PyObject*` runtime operations.

## Optimization Roadmap

Correctness remains the default path. The first general allocation reductions are
native lowering for `for ... in range(...)` and native i64 loop-control counters,
which avoid building a Python list for the range object and avoid boxed compare /
increment operations for the hidden loop counter. The visible Python loop
variable is still boxed each iteration.

The compiler also lowers proven numeric `+`, `-`, and `*` operations to LLVM
integer or double arithmetic and boxes the result at the boundary. Division,
mixed/unknown types, and operations with additional Python edge cases remain on
the boxed runtime path. The next planned steps are longer-lived unboxed numeric
locals, vector-backed homogeneous numeric lists, and specialized function
variants selected from proven call-site types. Unsupported or uncertain cases
must continue to use the boxed runtime path.

## Known gaps (planned)

- `lambda` expressions
- `nonlocal` statement
- Classes (`class`, `self`, method dispatch)
- `import` / module system

## License

Apache License 2.0 — see [LICENSE](LICENSE).

## Development history

Initially scaffolded with Grok (xAI). Extended substantially with Claude (Anthropic).
See `GROK.md` for early history.
