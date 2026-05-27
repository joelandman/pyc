# pyc

A minimum viable AOT (Ahead-Of-Time) compiler for a Python subset. It parses Python source using the Python C API, lowers the AST to a custom IR, generates LLVM IR, optimizes it, and produces static native executables with a minimal runtime (PyObject stubs, refcounting, builtins like print/add).

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

Supports `--static`, `--opt=0|1|2|3`, `-o output`.

## License
Apache License 2.0 (see LICENSE).

## Development
Built with assistance from Grok (xAI). See GROK.md for collaboration notes.

Current status: MVC achieved (parser, IR, LLVM codegen, runtime, tests, static binaries, control flow, optimizations, error handling).

Contributions welcome.
