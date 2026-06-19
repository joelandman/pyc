# PYC Compiler - Session Memory

## Project: Python 3 Compiler in C++ with LLVM
**Location**: /home/joe/work/pyc/
**Status**: In progress - Task 1 (C++ visitor functions) complete

## Current Context
Working on building a Python 3 compiler that generates native binaries with minimal external dependencies. Using Lark PEG grammar for parsing, C++ for compilation, and LLVM for code generation.

## Recent Work
Just completed Task 1: Implementing all C++ visitor functions for the Lark parser. This involved:
1. Extended `frontend/ast.h` with 12+ new AST node types (LambdaExpr, ListComp, MatchStmt, ImportFrom, etc.)
2. Wrote complete `frontend/lark_parser.cpp` (~700+ lines) with visitor implementations for all grammar rules
3. Updated `frontend/lark_parser.h` with all visitor declarations

## Grammar (python.lark)
Complete Python 3 PEG grammar covering:
- Statements: function def, class def, if/elif/else, for, while, try/except/finally, with, match/case, import, assign, augassign, return, raise, break, continue, pass, delete, global, nonlocal, assert
- Expressions: operator precedence (14 levels), lambdas, comprehensions, subscripts, slices
- Builtins, literals, keywords, punctuation
- Python indentation handling (INDENT/DEDENT)

## Key Relationships
- `python.lark` defines grammar → `lark_parser.cpp` converts Lark trees to pyc AST → AST feeds IR builder → IR generates LLVM IR
- All AST nodes in `ast.h` have corresponding visitor functions in `lark_parser.cpp`
- Runtime (`runtime/`) implements PyObject model, builtins, and GC

## Files to Work On Next
1. **LLVM Integration**: `codegen/llir_gen.cpp` (finish LLVM API calls)
2. **Interpreter**: `runtime/object.cpp` (execute compiled IR)
3. **Testing**: Run example programs through compiler

## Build System
- CMakeLists.txt in /home/joe/work/pyc/
- Run: cmake -B build && cmake --build build
- Test: ./build/pyc --test lexer/parser/ir/codegen

## Test Example
example.py in /home/joe/work/pyc/examples/ tests:
- factorial (recursive function)
- fibonacci (loop + list)
- Rectangle/Circle classes (inheritance)
- list comprehensions
- exception handling
- ternary operator
- tuple unpacking
- dict operations
