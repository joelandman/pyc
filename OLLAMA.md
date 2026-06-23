# PYC: Python 3 Compiler Written in C++ with LLVM

## Project Overview
A standalone Python 3 compiler that generates native binary executables with minimal dependencies. Written in C++17 leveraging LLVM infrastructure for code generation.

## Architecture

```
Source (.py) → Recursive Descent Parser → AST → IR → LLVM IR → Native Binary
```

## Project Structure

```
pyc/
  ├── frontend/                    # Lexing, parsing, AST
│   ├── ast.h/cpp                # AST node definitions (60+ node types)
│   ├── lexer.h/cpp              # Lexical analyzer
│   └── parser.h/cpp             # Recursive descent parser
├── ir/                          # Intermediate representation
│   ├── ir.h/cpp                 # IR types, instructions, blocks, functions
│   ├── builder.h/cpp            # AST → IR code generation
│   └── interpreter.h/cpp        # IR-based bytecode interpreter
├── codegen/                     # LLVM backend
│   ├── ir2ll.h/cpp              # IR → LLVM IR translation pass
│   └── llir_gen.h/cpp           # Legacy IR → LLVM IR generator
├── runtime/                     # Runtime system
│   ├── object.h/cpp             # PyObject model, PyTypeObject, factory
│   ├── builtins.cpp             # 60+ Python built-in functions
│   ├── gc.cpp                   # Mark-and-sweep garbage collector
│   └── import_system.cpp/h      # File-based module loading
├── examples/
│   └── example.py               # Comprehensive Python test program
├── test/
│   ├── benchmarks/              # Benchmark Game slowest implementations
│   ├── import_tests/            # Import system test suite
│   └── packages/                # Package structure test fixtures
├── main.cpp                     # Compiler driver & test runner
├── CMakeLists.txt               # Build configuration
├── PLANS.md                     # Correctness, completeness, performance plans
└── REMEDIATION.md               # Outstanding issues requiring fixes
```

## Current Status

### COMPLETED
1. **Parser**: Self-developed recursive descent parser with full visitor pattern (~700 lines of C++)
2. **AST**: Extended AST nodes for all Python 3 constructs (60+ node types)
3. **IR**: Intermediate representation with 53 instruction types, types, instructions, control flow, short-circuit evaluation
4. **Runtime**: PyObject model with proper string/list/dict/function/tuple storage, 60+ builtins
5. **Garbage Collector**: Fixed mark-and-sweep GC with proper refcounting and sweep
6. **LLVM Backend**: IR-to-LLVM translation with runtime function calls, O2 optimization enabled
7. **Interpreter**: Frame-stack based interpreter with proper ownership (unique_ptr), globals/locals support
8. **Build System**: CMake configuration with LLVM 21 dependency discovery
9. **Test Suite**: Built-in test runner (`--test lexer|ir|codegen`) - all tests pass
10. **Import System**: File-based module loading, packages, submodules, name binding, relative imports, namespace packages, sys.path support
11. **Correctness Fixes**: All 18 issues in REMEDIATION.md fixed or verified fixed
12. **Core Language Features**: Dict/tuple literals, `in`/`is` operators, short-circuit `and`/`or`, tuple unpacking, slice notation, f-strings, decorators, match/case, async/await, yield

### TODO (Next Steps)
1. **Error Recovery**: Better error messages for parse failures
2. **Walrus Operator**: Parser handler for `:=` token (lexer + AST + IR builder done)
3. **`sys` Module**: `sys.argv`, `sys.platform`, `sys.version`, `sys.exit()`
4. **`repr()`**: Proper type-specific formatting (strings with quotes, lists, dicts)
5. **`__main__` Detection**: Detect if module is run as main vs imported
6. **OOP Features**: `super()`, `property`, `staticmethod`, `classmethod`
7. **Performance**: Constant folding, type specialization, builtin inlining, arena allocator
8. **Testing**: End-to-end test suite with 50+ test cases, CPython benchmark comparison

## Dependencies
- **LLVM 17+**: Code generation (only major external dependency)
- **C++17**: Modern C++ features
- **CMake 3.20+**: Build system

## Build Instructions

```bash
# Install dependencies
sudo apt install llvm-dev cmake

# Build
cmake -B build
cmake --build build

# Run test suite
./build/pyc --test lexer
./build/pyc --test ir
./build/pyc --test codegen

# Compile example
./build/pyc examples/example.py
```

## Key Design Decisions
- **Self-developed recursive descent parser**: Hand-written parser with proper operator precedence handling
- **TokenType enum**: Lexer uses enum class for type-safe token identification
- **PyObject model**: Mirrors CPython's object model with proper memory management
- **LLVM IR**: Direct emission to LLVM IR for optimization and native codegen
- **Minimal runtime**: Only critical stdlib shipped; rest compiled at runtime
- **exec()/eval() excluded**: Intentionally unsupported due to security implications
