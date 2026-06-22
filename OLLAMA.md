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
│   ├── builtins.cpp             # 40+ Python built-in functions
│   └── gc.cpp                   # Mark-and-sweep garbage collector
├── examples/
│   └── example.py               # Comprehensive Python test program
├── test/
├── main.cpp                     # Compiler driver & test runner
├── CMakeLists.txt               # Build configuration
├── PLANS.md                     # Correctness, completeness, performance plans
└── REMEDIATION.md               # Outstanding issues requiring fixes
```

## Current Status

### ✅ COMPLETED
1. **Parser**: Self-developed recursive descent parser with full visitor pattern (~700 lines of C++)
3. **AST**: Extended AST nodes for all Python 3 constructs (60+ node types)
4. **IR**: Intermediate representation with types, instructions, control flow
5. **Runtime**: PyObject model with proper string/list/dict/function storage, 40+ builtins
6. **Garbage Collector**: Fixed mark-and-sweep GC with proper refcounting and sweep
7. **LLVM Backend**: IR-to-LLVM translation with runtime function calls (pyc_new_object, pyc_list_get, etc.)
8. **Interpreter**: Frame-stack based interpreter with proper ownership (unique_ptr) and control flow
9. **Build System**: CMake configuration with LLVM 21 dependency discovery
10. **Test Suite**: Built-in test runner (`--test lexer|ir|codegen`)

### 🚧 IN PROGRESS
- Correctness fixes (completed steps 1-5, see REMEDIATION.md)

### TODO (Next Steps)
1. **Import System**: Module loading, package structure
2. **Error Recovery**: Better error messages for parse failures
3. **Python 3 Completeness**: async/await, match/case, descriptors, comprehensions
4. **Testing**: Run real Python 3 programs through the compiler, add test files
5. **Performance**: SSA form, constant propagation, inlining, register allocation
6. **Class System**: Full class inheritance, attributes, method calls
7. **Exception Handling**: try/except/else/finally and raise statements

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
