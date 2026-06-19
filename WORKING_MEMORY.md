# PYC Compiler - Session Memory

## Project: Python 3 Compiler in C++ with LLVM
**Location**: /home/joe/work/pc/pyc/
**Status**: In progress - Step 5 (Remaining Language Features) complete

## Current Context
Working on building a Python 3 compiler that generates native binaries with minimal external dependencies. Using Lark PEG grammar for parsing, C++ for compilation, and LLVM for code generation.

## Recent Work - Step 5: Remaining Language Features

### Critical Bug Fixes
1. **POW bug** (`codegen/ir2ll.cpp:463-486`): Was using `CreateFMul` (multiplication) instead of actual power. Fixed to call `pyc_pow` runtime function with LLVM `intrinsic::pow` fallback.
2. **to_int bug** (`runtime/builtins.cpp:27`): `reinterpret_cast<double*>(obj->data)` was wrong - obj->data is uint64_t, not a double pointer. Fixed to use `reinterpret_cast<uint64_t*>(&obj->data)`.
3. **bool builtin** (`runtime/builtins.cpp:1021`): Logic was inverted (`!args[0]->data`). Fixed to `args[0]->data != 0`.
4. **build_unary NEG/UPLUS** (`ir/builder.cpp:487-500`): NEG mapped to SUB with no second operand, UPLUS mapped to ADD with no second operand. Fixed NEG to emit `0 - operand`, UPLUS to return operand directly.
5. **class instantiation** (`ir/builder.cpp:635-652`): build_call() didn't detect class constructors. Now checks if function name has `.__init__` in module and calls build_class_call().
6. **getattr/setattr** (`ir/interpreter.cpp:446-462`): Were stubs returning 0. Now use global_vars_ with key format `instance_attr_{obj_id}_{attr_name}`.
7. **getattr/setattr LLVM** (`codegen/ir2ll.cpp:349-384`): Were stubs. Now emit calls to `pyc_getattr`/`pyc_setattr` runtime functions.

### New Built-in Functions
- **List methods**: append, extend, pop, remove, clear, count, index
- **Dict methods**: get, keys, values, items, pop_dict, setdefault, dict_clear
- **String methods**: join, split, strip, replace, upper, lower, find, format

### New Statement Handlers (IR Builder)
- **DeleteStmt**: Stores 0 to target slots
- **GlobalStmt/NonlocalStmt**: Emits LOADGLOBAL for each name
- **AssertStmt**: Branches to false block on assertion failure
- **RaiseStmt**: Builds exception expression, emits RETURN
- **WithStmt**: Handles context managers with optional vars
- **TryStmt**: Creates try/except/merge block structure
- **BreakStmt/ContinueStmt**: Track loop context (cond_block, merge_block) for proper jumps

### Other Improvements
- **SubscriptExpr**: Added build_subscript() for list/dict indexing (LIST_GET instruction)
- **Loop context tracking**: Added LoopContext struct to track break/continue targets
- **IR intrinsic declarations**: Added pyc_getattr, pyc_setattr function declarations

## Grammar (python.lark)
Complete Python 3 PEG grammar covering:
- Statements: function def, class def, if/elif/else, for, while, try/except/finally, with, match/case, import, assign, augassign, return, raise, break, continue, pass, delete, global, nonlocal, assert
- Expressions: operator precedence (14 levels), lambdas, comprehensions, subscripts, slices
- Builtins, literals, keywords, punctuation
- Python indentation handling (INDENT/DEDENT)

## Key Relationships
- `python.lark` defines grammar → `lark_bridge.py` parses .py → outputs JSON → `lark_parser.cpp` reads JSON → builds AST → AST feeds IR builder → IR generates LLVM IR
- All AST nodes in `ast.h` have corresponding visitor functions in `lark_parser.cpp`
- Runtime (`runtime/`) implements PyObject model, builtins, and GC

## Files Modified in Step 5
1. `codegen/ir2ll.cpp` - POW fix, getattr/setattr LLVM codegen, runtime function declarations
2. `runtime/builtins.cpp` - Fixed to_int, bool; added 20+ new builtin functions
3. `runtime/object.h` - Added declarations for new builtin functions
4. `ir/builder.cpp` - Fixed build_unary, integrated build_class_call, added 10 new statement handlers, added build_subscript
5. `ir/builder.h` - Added declarations for new methods, LoopContext struct
6. `ir/interpreter.cpp` - Implemented getattr/setattr, simplified INTRINSIC_RANGE, added current_func_/current_block_ context
7. `ir/interpreter.h` - Added current_func_, current_block_ members

## Build System
- CMakeLists.txt in /home/joe/work/pc/pyc/
- Run: cmake -B build && cmake --build build
- Test: ./build/pyc --test-compile <file.py> (lexer only)
- Test IR: ./build/pyc --test-ir <file.py> (requires lark_bridge.py JSON output)

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
- Created `test/benchmarks/` directory with 7 benchmark programs:
  - `tight_loop.py` - 1M iteration numeric loop
  - `function_calls.py` - 100K function call overhead test
  - `list_ops.py` - 100K list create/append/access
  - `dict_ops.py` - 50K dict create/insert/access
  - `string_ops.py` - 10K string concatenation
  - `recursion.py` - factorial(20) recursion test
  - `nbody.py` - N-body gravitational simulation (50 bodies, 100 steps)
- Created `run_benchmarks.sh` runner script with timing measurements
