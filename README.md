# pyc

An AOT (Ahead-Of-Time) compiler for a substantial Python subset. Parses Python
source using the Python C API, lowers the AST through a visitor-based IR, generates
LLVM IR, optimizes it, and produces standalone native executables via a minimal
`PyObject*`-based boxed runtime with refcounting.

Written in C++ with Clang++ and LLVM 18. No C/C++ intermediate language for
the normal compiler path. **300/300 tests passing** (runner reports 300/300, file_case_failures=0; every case compared against CPython output; see `tests/runner.py`).

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

### Command-Line Options

| Flag | Description |
|------|-------------|
| `-o output` | Output file path (default: `a.out`) |
| `--static` | Produce fully static binary (no dynamic libs) |
| `--opt=0` | No optimization |
| `--opt=1` | O1 optimization |
| `--opt=2` | O2 optimization (default) |
| `--opt=3` | O3 optimization |
| `--emit-llvm` | Emit LLVM IR to `output.ll` instead of binary |
| `--emit-asm` / `-S` | Emit assembly to `output.s` instead of binary |
| `--verbose` | Print verbose compilation information |

## Testing

```bash
cd build && make check   # or: ctest
```

The test suite in `tests/runner.py` compiles each case and compares output
against CPython. It also runs file-based regression programs in `tests/` that
cover optimization-sensitive behavior such as native `range` loops, numeric
locals, numeric list mutation and aliasing, command-line arguments, default
arguments, nested destructuring, and `tests/nbody.py`.

### Running Tests Directly

```bash
PYC_BINARY=./build/pyc python3 tests/runner.py
```

## What Compiles Today

See [FEATURES.md](FEATURES.md) for a complete list of supported features.

### Quick Summary

- **Types**: int, float, bool, str, list, dict, tuple, None, complex
- **Operators**: `+ - * / // % **`, `== != < > <= >=`, `is`, `in`, `and`, `or`, `not`, unary `-`
- **Control flow**: `if/elif/else`, `while`, `for`, `break`, `continue`, ternary
- **Functions**: `def`, `lambda`, nested functions, closures (`nonlocal`), decorators
- **Classes**: `class`, `__init__`, inheritance, `super()`, `__str__`/`__repr__`
- **Exceptions**: `try/except/finally/else`, structured exceptions, exception classes
- **Statements**: `with`, `match/case`, `assert`, `del`, walrus `:=`, `import`
- **Builtins**: `print`, `range`, `len`, `str`, `int`, `float`, `complex`, `abs`, `min`, `max`, `list`, `enumerate`, `zip`, `sum`, `sorted`, `any`, `all`, `isinstance`, `bool`, `type`, `id`, `repr`, `hex`, `oct`, `bin`, `ord`, `chr`, `round`, `divmod`, `pow`, `reversed`, `cmp_to_key`
- **Standard library stubs**: `os.path` (exists, isfile, isdir), `subprocess` (call, check_output), `sys`, `cmath`

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
proxy, compiled regex, match object, exception, function, exception class, complex.
Exceptions use setjmp/longjmp frames. Callables dispatch through a registry of
`__apply__` adapters (`Pyc_Apply`). Linked into every compiled binary.

**IR**: linear instruction list per function. Instructions: `const`, `fconst`,
`bconst`, `nconst`, `assign`, `add`/`sub`/`mul`/`div`/`truediv`/`mod`/`pow`,
`icmp`, `br`, `label`, `call`, `ret`. IR instructions carry conservative result
type metadata (`int`, `float`, `bool`, `str`, or `boxed`).

## Documentation

- [README.md](README.md) — This file: build, usage, options, quick feature summary
- [FEATURES.md](FEATURES.md) — Complete feature list and capabilities
- [IMPLEMENTATION.md](IMPLEMENTATION.md) — Design choices, limitations, architecture details

## License

Apache License 2.0 — see [LICENSE](LICENSE).

## Development history

Initially scaffolded with Grok (xAI). Extended substantially with Claude (Anthropic).
See [GROK.md](GROK.md) for early history.
