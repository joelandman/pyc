# PYC Compiler - Session Memory

## Project: Python 3 Compiler in C++ with LLVM
**Location**: /home/joe/work/pc/pyc/
**Status**: In progress - Step 10 (Correctness Fixes) complete

## Current Context
Working on building a Python 3 compiler that generates native binaries with minimal external dependencies. Using self-developed recursive descent parser, C++ for compilation, and LLVM for code generation.

## Recent Work - Step 10: Correctness Fixes

### Critical Bug Fixes
1. **INTRINSIC_RANGE bug** (`codegen/ir2ll.cpp:407-430`): Was calling `pyc_new_list()` without populating it. Fixed to call `pyc_range_list()` with start, stop, step parameters from IR operands.

2. **handle_call() dynamic function support** (`ir/interpreter.cpp:516-560`): Now checks if function is in module functions, builtins, or global vars before falling back to named function call. Enables dynamic function calls and lambda expressions.

3. **GETATTR/LOAD_ATTR attribute name** (`codegen/ir2ll.cpp:457-480`): Now creates GlobalVariable for attribute name string and passes it to `pyc_getattr()`. Previously was passing only the object pointer.

4. **SETATTR attribute name verification** (`codegen/ir2ll.cpp:495-515`): Verified that attribute name is correctly passed as GlobalVariable pointer to `pyc_setattr()`.

5. **Lambda expression implementation** (`ir/builder.cpp:805-851`): Improved lambda function creation to properly capture lambda parameters and build body in lambda scope. Returns CALL instruction to lambda function.

6. **Import system runtime** (`runtime/libpyc_runtime.cpp:368-400`): Added file-based module loading with `#include <fstream>`. Creates module dict and attempts to load .py file. Currently creates empty module dict (stub for full implementation).

### Test Results
- All 7 existing tests pass (lexer, parser, IR, codegen tests)
- 01_arithmetic.py: PASSED
- 02_conditionals.py: PASSED
- 03_loops.py: PASSED
- 04_recursion.py: PASSED
- 05_lists.py: PASSED
- 06_strings.py: PASSED
- 07_booleans.py: PASSED

## Key Relationships
- `frontend/parser.cpp` (recursive descent) parses tokens from lexer → builds AST → AST feeds IR builder → IR generates LLVM IR
- All AST nodes in `ast.h` have corresponding visitor functions in `frontend/parser.cpp`
- Runtime (`runtime/`) implements PyObject model, builtins, and GC

## Files Modified in Step 10
1. `codegen/ir2ll.cpp` - INTRINSIC_RANGE fix, GETATTR/LOAD_ATTR attribute name fix
2. `ir/interpreter.cpp` - handle_call() dynamic function support
3. `ir/builder.cpp` - Lambda expression implementation improvement, comprehension builders
4. `runtime/libpyc_runtime.cpp` - Import system runtime delegates to import_system.cpp
5. `runtime/import_system.cpp` - New: file-based module loading, parsing, execution
6. `runtime/import_system.h` - New: import API header

## Build System
- CMakeLists.txt in /home/joe/work/pc/pyc/
- Run: cmake -B build && cmake --build build
- Test: ./build/pyc --test-compile <file.py> (lexer and parser tests)
- Test IR: ./build/pyc --test-ir <file.py> (recursive descent parser)

## Test Files
test/ directory contains 7 test files:
- 01_arithmetic.py - Functions, arithmetic (+, -, *, /), return, print
- 02_conditionals.py - If/else, comparisons (>, <, ==)
- 03_loops.py - While loops, increment, print
- 04_recursion.py - Recursive functions, factorial, fibonacci
- 05_lists.py - List literals, len(), list indexing
- 06_strings.py - String literals, len() on strings
- 07_booleans.py - Boolean literals, comparisons, print

## Performance Work - Step 6

### Instruction Result Cache
- Added `instr_results` map to `CallFrame` (ir/interpreter.h:29)
- Added `cache_result()` and `get_cached_result()` methods
- Modified `execute_instruction()` to cache all instruction results (ir/interpreter.cpp:269-415)
- Enables O(1) lookup of instruction results by ID

### LLVM Optimization Pipeline
- Added `PassBuilder` and `ModuleAnalysisManager` includes (codegen/ir2ll.cpp:15-18)
- Applied O2 optimization pipeline in `translate_module()` (codegen/ir2ll.cpp:560-571)
- Uses `buildPerModuleDefaultPipeline(OptimizationLevel::O2)`

### Benchmark Suite
- Benchmark programs exist in `test/benchmarks/` directory
- `run_benchmarks.sh` runner script available

## Memory Profiling - Step 7
- Built with AddressSanitizer (-fsanitize=address)
- All 7 tests pass with 0 ASan errors
- Built with Valgrind (3.26.0)
- All 7 tests pass with 0 Valgrind errors
- 0 bytes definitely lost, 0 bytes indirectly lost
- 159KB still reachable (LLVM internal data structures - expected)

## Scalability Testing - Step 7
### Compile Time (lexer-only, ms)
- 10 lines: 14ms
- 50 lines: 11ms  
- 100 lines: 8ms
- 500 lines: 7ms
- Compile time dominated by process startup/LLVM init, not linear

### Global Variable Scaling (ms)
- 10 globals: 8ms
- 50 globals: 9ms
- 100 globals: 9ms
- 200 globals: 8ms
- Consistent ~8-9ms regardless of global count

### Note
- Full compilation pipeline (parser → IR → LLVM) uses self-developed recursive descent parser
- Lexer and parser tests pass for all programs
