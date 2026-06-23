# PYC Compiler - Session Memory

## Project: Python 3 Compiler in C++ with LLVM
**Location**: /home/joe/work/pc/pyc/
**Status**: 80% complete for MVP - core language features, import system, and correctness fixes all implemented

## Current Context
Working on building a Python 3 compiler that generates native binaries with minimal external dependencies. Using self-developed recursive descent parser, C++ for compilation, and LLVM for code generation.

## Architecture

```
Source (.py) → Recursive Descent Parser → AST → IR → LLVM IR → Native Binary
```

## Key Files

### Frontend (Lexer, Parser, AST)
- `frontend/lexer.h/cpp` - Lexical analyzer, handles all Python tokens
- `frontend/parser.h/cpp` - Recursive descent parser with visitor pattern
- `frontend/ast.h` - 60+ AST node types

### IR (Intermediate Representation)
- `ir/ir.h/cpp` - 53 IR instruction types, blocks, functions
- `ir/builder.h/cpp` - AST to IR code generation
- `ir/interpreter.h/cpp` - Frame-stack interpreter with unique_ptr ownership

### Codegen (LLVM Backend)
- `codegen/ir2ll.h/cpp` - IR to LLVM IR translation with O2 optimization

### Runtime
- `runtime/object.h/cpp` - PyObject model, factory, registry
- `runtime/builtins.cpp` - 60+ builtin functions
- `runtime/gc.cpp` - Mark-and-sweep garbage collector
- `runtime/import_system.cpp/h` - File-based module loading
- `runtime/libpyc_runtime.cpp/h` - 27 runtime functions

## Build System
- CMakeLists.txt in /home/joe/work/pc/pyc/
- Run: cmake -B build && cmake --build build
- Test: ./build/pyc --test lexer (lexer unit tests)
- Test: ./build/pyc --test ir (IR builder unit tests)
- Test: ./build/pyc --test codegen (LLVM codegen unit tests)
- Compile: ./build/pyc program.py && ./program

## Test Files

### Unit Tests (3 tests, all pass)
- Lexer tests: Tokenization of Python syntax
- Parser tests: Recursive descent parser validation
- IR tests: IR builder validation
- Codegen tests: LLVM codegen validation

### Integration Tests
- test/import_tests/ - Import system test suite with 10 test programs
- test/packages/mypackage/ - Package structure test fixtures
- test/benchmarks/ - Benchmark Game slowest implementations (fannkuchredux, spectralnorm, binarytrees, knucleotide, fasta)

## Implemented Features

### Core Language
- Arithmetic, comparison, boolean operators
- Functions with parameters and return values
- Classes with `__init__` and methods
- `if/elif/else` conditionals
- `while` loops, `for` loops with `range()`
- Dict/tuple literals, slice notation, tuple unpacking
- Lambda expressions, comprehensions (list/set/dict/generator)
- f-strings, decorators, match/case, async/await, yield
- `in` and `is` operators, short-circuit `and`/`or`

### Exception Handling
- `raise`, `try/except/else/finally` blocks
- Exception propagation with `__current_exception__` local

### Import System
- File-based module loading via recursive descent parser
- Packages, submodules, name binding
- Relative imports, namespace packages
- `sys.path` support with `set_sys_path()` / `get_sys_path()` API

### Builtins (60+)
- Type conversion: `int`, `float`, `str`, `bool`, `list`, `dict`, `tuple`
- Numeric: `abs`, `max`, `min`, `pow`, `sum`, `round`, `divmod`, `chr`, `ord`, `hex`, `oct`, `bin`
- Iteration: `range`, `sorted`, `reversed`, `enumerate`, `zip`, `map`, `filter`
- Introspection: `type`, `len`, `isinstance`, `callable`, `hasattr`, `dir`, `globals`, `locals`
- List methods: `append`, `extend`, `pop`, `remove`, `clear`, `count`, `index`
- Dict methods: `get`, `keys`, `values`, `items`, `pop`, `setdefault`, `update`, `fromkeys`, `popitem`
- String methods: `join`, `split`, `strip`, `replace`, `upper`, `lower`, `find`, `startswith`, `endswith`, `format`

## Memory Management
- Built with AddressSanitizer (-fsanitize=address) - all tests pass
- Built with Valgrind (3.26.0) - all tests pass, 0 bytes lost
- Mark-and-sweep GC with proper refcounting
- PyObjectRegistry tracks all non-singleton allocations

## Excluded for Foreseeable Future
- **exec() and eval() builtins**: Intentionally unsupported due to security implications (arbitrary code execution, code injection, sandbox escape). Will not be implemented. Use explicit function calls or pre-compiled IR modules instead.

## Next Steps
1. Finish walrus operator (add parser handler for `:=` token)
2. Add `sys` module stub (sys.argv, sys.platform, sys.version, sys.exit)
3. Implement `repr()` with proper type-specific formatting
4. Add `__main__` entry point detection
5. Implement `super()`, `property`, `staticmethod`, `classmethod`
6. Performance: constant folding, type specialization, builtin inlining
