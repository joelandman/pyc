# pyc

An AOT (Ahead-Of-Time) compiler for a substantial Python subset. Parses Python
source using the Python C API, lowers the AST through a visitor-based IR, generates
LLVM IR, optimizes it, and produces standalone native executables via a minimal
`PyObject*`-based boxed runtime with refcounting.

Written in C++ with Clang++ and LLVM 18. No C/C++ intermediate language for
the normal compiler path. **293/293 tests passing** (runner reports 293/293, file_case_failures=0; every case compared against CPython output; see `tests/runner.py`).

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
- `def` with positional args, default args, keyword args (`f(b=3, a=4)`),
  `*args` collection and call-site unpacking, `**kwargs`
- `return` (including multiple return values `return a, b`)
- Nested functions, closures (`nonlocal`, cell capture, skip-level forwarding)
- `lambda` expressions (defaults, `*args`, as values in containers/args/returns)
- First-class functions: defs and lambdas as values with real function objects —
  `print(f)` gives `<function f at 0x...>`, identity-based `==`/`is`
- Decorators: `@deco`, `@deco(args)` factories, stacked (applied bottom-up)
- `global x` declaration (shared module-level storage)

### Classes
- `class` with `__init__`, instance attributes, method dispatch, class attributes
- Single and multiple inheritance with C3-linearized MRO
- `super()` following the runtime MRO (full remaining-MRO method search)
- `__str__` / `__repr__` protocol (used by `print`, `str`, f-strings)

### Exceptions
- `try` / `except` / `except ... as e` / `else` / `finally`
- Typed handler dispatch with the builtin exception hierarchy
  (`ArithmeticError`, `LookupError`, `OSError` parents; `Exception` catch-all)
- Tuple clauses `except (A, B)`, bare re-raise, structured exception objects
- Builtins raise at the point of error (`ZeroDivisionError`, `IndexError`,
  `KeyError`, `ValueError` from `int()`)
- `finally` runs on every exit path: fall-through, exception, `return`,
  `break` / `continue`, raise inside a handler or `else`
- Uncaught exceptions print a CPython-style traceback line to stderr, exit 1

### Statements
- `with` (context managers via `__enter__` / `__exit__`)
- `match` / `case` (literals, wildcard, capture, singletons, guards)
- `assert`, `del`, walrus `:=`
- `import` / `from ... import` (file-based modules, packages, `os.path` /
  `subprocess` / `sys` / `re` stubs)

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
`list`/`dict`/`str`/`cell_content`), refcounting, arithmetic, comparison, print,
and all builtins. Types: int, list, dict, str, float, bool/None, cell, super
proxy, compiled regex, match object, exception, function. Exceptions use
setjmp/longjmp frames (raise pops the frame before the jump; handler dispatch
happens in generated code). Callables dispatch through a registry of
`__apply__` adapters (`Pyc_Apply`). Linked into every compiled binary.

**IR**: linear instruction list per function. Instructions: `const`, `fconst`,
`bconst`, `assign`, `add`/`sub`/`mul`/`div`/`truediv`/`mod`/`pow`, `icmp`, `br`,
`label`, `call`, `ret`. IR instructions can carry conservative result type
metadata (`int`, `float`, `bool`, `str`, or `boxed`). Codegen uses that metadata
for specific general native paths, such as range loop counters and numeric
`+`/`-`/`*`, and otherwise falls back to boxed `PyObject*` runtime operations.

## Optimizations (landed)

Correctness remains the default path; every optimization preserves a boxed
fallback. Landed so far (A1–A7, see `UNBOXING_AND_COMPLETENESS_PLAN.md`):

- Conservative type tracking with loop back-edge widening (A1)
- Native `for ... in range(...)` with unboxed i64 loop variables, and general
  unboxed numeric locals/accumulators with boxing only at escape points (A2)
- Native arithmetic for proven numeric `+ - * // % **` and unary minus, with
  Python floor/sign semantics and zero-division fallback (A3)
- Homogeneous numeric lists with native `int64`/`double` element storage (A4)
- Allocation sinking for numeric locals (A5)
- Specialized function variants from proven call-site types (A6)
- Allocation counters and microbenchmark guardrails (A7; see `BENCHMARKS.md`)

Unsupported or uncertain cases always use the boxed runtime path.

## Known gaps (planned)

- Generators (`yield`) — next major item; design: chunked materialization
  (bounded buffer, ~16 elements per refill) rather than full eager lists
- Decorators on closure defs, decorated class methods, class decorators
- Exception classes as first-class values; `raise ... from ...` chaining
- Complex numbers (negative base with fractional exponent yields NaN)
- Cross-scope function identity (`f is f` from another scope mints a fresh
  object); closure values print as descriptor bundles, not `<function ...>`

## License

Apache License 2.0 — see [LICENSE](LICENSE).

## Development history

Initially scaffolded with Grok (xAI). Extended substantially with Claude (Anthropic).
See `GROK.md` for early history.
