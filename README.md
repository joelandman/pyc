# pyc

A minimum viable AOT (Ahead-Of-Time) compiler for a Python subset. It parses Python source using the Python C API, lowers the AST using a visitor-based IR lowerer, generates LLVM IR, optimizes it, and produces static native executables using a minimal `PyObject*`-based runtime with refcounting.

Written in C++ with Clang++ and LLVM 18. No C/C++ as intermediate language. Focus on minimal dependencies and small binaries.

## Goals
- Static, standalone binaries with no CPython runtime dependency.
- High performance via LLVM optimizations and native code.
- Support for core Python (functions, variables, if/else, loops, basic classes/exceptions/strings).
- Minimal runtime (objects, refcount, builtins).
- Easy to extend and embed.
- Apache 2.0 licensed for broad use.

## Build
```bash
mkdir -p build && cd build
cmake ..
make -j
sudo make install
```

## Usage
```bash
pyc test.py --static --opt=2 -o myapp
./myapp
```

Supports `--static`, `--opt=0|1|2|3`, `-o output`, `--emit-llvm`, `--emit-asm` / `-S`, `--verbose`.

See `examples/` for sample programs.

After `make install`, you can also link against the runtime library:
```bash
clang++ myprog.o -lpycrt -o myprog
```

## Testing

```bash
make check          # or: ctest
```

The test suite lives in `tests/` and compares output against CPython.

## License
Apache License 2.0 (see LICENSE).

## Development
Built with assistance from Grok (xAI). See GROK.md for collaboration notes.

### Current Status
- Working features: functions, returns, variables, `if`/`else`, `while`, basic arithmetic, `print`, list comprehensions, dictionary comprehensions, *args and **kwargs
- Architecture: Visitor-based AST lowering + full `PyObject*` boxed runtime (with basic refcounting)
- Tooling: `--emit-llvm`, `--emit-asm`/`-S`, `--verbose`, `make check` / `ctest`
- Packaging: Installable runtime library (`libpycrt`) + public headers

The compiler correctly handles list and dictionary comprehensions in the AST and generates appropriate IR, though full comprehension semantics with proper iteration control flow are still being implemented.

The project is in active development. See the commit history for recent deep polish work (visitor refactor, boxed runtime migration, testing infrastructure).

Contributions welcome.
