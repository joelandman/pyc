# GROK.md

This project (pyc) was built interactively with Grok (xAI model grok-4.20-0309-reasoning) using the opencode CLI tool.

## Collaboration Summary
- **Scaffolding:** Grok created directory structure (src/frontend, ir, codegen, runtime, include/pyc, tests, cmake), CMake with LLVM 18 + Python C API integration, initial main, Compiler, PythonParser, IR, Codegen classes.
- **Frontend:** Parser using Python C API (`ast.parse`, recursive buildAST for Module, FunctionDef, Assign, Call, BinOp, Constant, Name, If, While, Compare, ClassDef, Try, Str). Handled PyObject traversal, fixed crashes/warnings with null checks and type handling.
- **Lowering:** AST to custom ModuleIR with instructions (const, add, assign, call, icmp, br, label, ret). Recursive lowerExpression with temp counters, value tracking, support for variables, control flow skeleton (if/else, while with labels/br).
- **Codegen:** LLVM IRBuilder for functions, constants, add, assign (alloca/store/load), calls (printf/PyObject_Print, general functions like add), control flow stubs (BasicBlock, ICmp, CondBr). TargetMachine for object emission with optimizations (O2 via Aggressive/Default). Verification and error handling.
- **Runtime:** Evolved from simple print to full `PyObject` (refcount, type, value), `PyInt_FromLong`, `PyNumber_Add`, `PyObject_Print`, `Py_DECREF`, `PyErr_Print`, stubs for exceptions/strings. Pure C for static linking.
- **CLI/Packaging:** Flags (--static, --opt, -o), CMake install target, automated linking with -static -s -Wl,--gc-sections.
- **Tests/Error Handling:** Comprehensive `tests/test_basic.py` + runner with timing benchmarks, output validation, error cases. Compiler/Parser handle parse errors, IR verification, link failures with messages.
- **Optimizations/Static:** LLVM passes via TargetMachine/PassBuilder (instcombine, GVN, CFG simplify). Static binaries (~700K, no dynamic libs via ldd).
- **Other:** Project rename (pyllvm -> pyc), variable/if/while/classes support, control flow skeleton, benchmarking, Apache 2.0 license, README, list/dict comprehension support.

Grok followed code conventions (no unnecessary comments, mimic existing style, use existing libs, security best practices). Used tools for search/edit/build/test/git. All changes verified with builds, runs, and tests.

The AI acted as an interactive engineering partner, providing plans, code, fixes, and reports after each step.

Project achieved MVC for Python subset to static native executables with LLVM.

### Later Major Work
- Visitor-based AST lowering refactor (Deep Polish 4)
- Full migration to `PyObject*` boxed runtime with basic refcounting (Deep Polish 5, Option A)
- Improved testing (`make check` / ctest), packaging (runtime headers + `libpycrt`), and CLI (`--emit-llvm`, `-S`, `--verbose`)

See README.md and LICENSE for details.
