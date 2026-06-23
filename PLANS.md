# PLANS.md

## Correctness Plan

### 1. Fix Garbage Collector Fundamentals
**Location:** `runtime/gc.cpp`, `runtime/object.h`
**Status: FIXED**
- `mark_object()` only sets mark bit, no longer touches refcount
- `del_ref()` deletes at refcount zero without mark set check
- `collect()` rebuilds `roots_` after sweep to contain only marked objects
- `new_object()` no longer auto-registers as root

### 2. Fix Object Model Memory Management
**Location:** `runtime/object.cpp`, `runtime/builtins.cpp`
**Status: FIXED**
- `create_str()` stores string via `str_value` member
- `create_function()` stores callable via `func_callable` member
- `to_int()` fixed for TYPE_FLOAT via `reinterpret_cast<uint64_t*>`
- `bool` builtin fixed (inverted logic)
- `PyObjectRegistry` tracks all non-singleton allocations
- `Py_INCREF`/`Py_DECREF` helpers added to object.h
- `finalize()` calls `registry.cleanup()` to free all registered objects
- Small int caching: separate singletons for -1, 0, 1
- Runtime lib objects registered with `PyObjectFactory::register_object()`

### 3. Fix IR Builder Control Flow
**Location:** `ir/builder.cpp`, `ir/builder.h`
**Status: FIXED**
- `build_unary()` fixed: NEG emits `0 - operand`, UPLUS returns operand directly
- `build_call()` detects class constructors via `build_class_call()`
- `build_class_call()` creates NEWOBJ + CALL for instantiation
- `build_subscript()` added for LIST_GET
- Loop context tracking via `LoopContext` struct for break/continue
- All 10 statement handlers added: delete, global, nonlocal, assert, raise, with, try, break, continue

### 4. Fix Interpreter Memory Management
**Location:** `ir/interpreter.cpp`, `ir/interpreter.h`
**Status: FIXED**
- Frames use `std::unique_ptr<CallFrame>` in frame stack
- Instruction result cache (`instr_results` map) with `cache_result()`/`get_cached_result()`
- `getattr/setattr` implemented using `instance_attrs` on PyObject
- `current_func_`/`current_block_` added for intrinsic code generation
- `handle_intrinsic_type()` returns type string (int/float/str/NoneType)
- `handle_intrinsic_init()` returns the object unchanged
- `handle_intrinsic_len()` handles strings
- `handle_call()` now checks module functions, builtins, and global vars for dynamic calls

### 5. Fix LLVM Codegen for Python-Specific Operations
**Location:** `codegen/ir2ll.cpp`
**Status: FIXED**
- `POW` calls `pyc_pow` runtime function with `llvm::intrinsic::pow` fallback
- `GETATTR`/`LOAD_ATTR` emit calls to `pyc_getattr` with attribute name string
- `SETATTR` emits call to `pyc_setattr` with proper attribute name string
- `MAKE_LIST` calls `pyc_new_list()`
- `LIST_GET` calls `pyc_list_get()`
- `LIST_SET` calls `pyc_list_set()`
- `NEWOBJ` calls `pyc_codegen_new_object()`
- `INTRINSIC_PRINT` calls `pyc_print()`
- `INTRINSIC_TYPE` calls `pyc_type_name()`
- `INTRINSIC_LEN` calls `pyc_len()`
- `INTRINSIC_INIT` calls `pyc_object_init()`
- `ISINSTANCE` calls `pyc_isinstance()`
- `NEWTYPE` calls `pyc_new_type()`
- `range()` calls `pyc_range_list()` via CALL instruction
- `INTRINSIC_RANGE` now calls `pyc_range_list()` with start, stop, step parameters
- LLVM O2 optimization pipeline enabled via `buildPerModuleDefaultPipeline(Opt2)`

---

## Completeness Plan

### 1. Complete `for` Loop Iteration
**Status: FIXED**
- `build_for_stmt()` uses index-based iteration over lists
- Loop initializes index to 0, compares `index < len(list)` in condition
- Uses `LIST_GET` to get element at current index
- Added `pyc_range_list(start, stop, step)` runtime function

### 2. Implement Comprehensions
**Status: FIXED**
- List comprehensions: `[expr for target in iterable]`
- Set comprehensions: `{expr for target in iterable}`
- Generator expressions: `(expr for target in iterable)`
- Dict comprehensions: `{k: v for target in iterable}`
- All return lists

### 3. Implement Lambda Expressions
**Status: FIXED**
- `build_lambda_expr()` creates new IR function with unique name
- Lambda body built in new function scope with proper parameter handling
- Returns CALL instruction to the lambda function

### 4. Implement Import System
**Status: FIXED**
- `build_import_stmt()` calls `pyc_import_module(module_name)` runtime function
- `runtime/import_system.cpp` implements `import_module()`, `import_from_module()`, `clear_loaded_modules()`
- File-based module loading: reads .py file, tokenizes, parses with recursive descent parser, builds IR, executes in module namespace
- Caches loaded modules in `g_loaded_modules` map
- Simple imports (`import module1, module2`) supported via `names_` vector in ImportFrom
- `sys.path` support with `set_sys_path()` / `get_sys_path()` API (default: [".", "./modules", "./lib"])
- Package structure support: detects packages (directories with `__init__.py`)
- Package initialization: loads `__init__.py` when importing a package
- Submodule imports: `from package import submodule` loads and caches submodules
- Submodules accessible via dot notation: `package.submodule.name`
- Relative imports: `from . import x`, `from ..pkg import y` (level-based resolution)
- Namespace package support: directories with .py files but no `__init__.py`
- Namespace packages get `__path__` and `__name__` attributes

### 5. Implement Exception Handling
**Status: FIXED**
- `build_raise_stmt()` emits `CALL pyc_raise_exception` with exception object
- `pyc_raise_exception()`, `pyc_get_exception()`, `pyc_clear_exception()` implemented
- `build_try_stmt()` stores exception in `__current_exception__` local
- Exception passed to handlers, stored in `__exception_type__` when handler has a type
- `finally` block always executes regardless of exception state
- `else` clause after `except` blocks supported

### 6. Add Dict Literal Support
**Status: FIXED**
- `LBRACE`/`RBRACE` tokens added to lexer
- `DictLiteral` AST node added
- Parse `{key: val, ...}` in parser
- `MAKE_DICT`, `DICT_GET`, `DICT_SET` IR instructions added
- `build_dict()` IR builder method implemented
- `pyc_new_dict()`, `pyc_dict_set()`, `pyc_dict_get()` runtime functions added

### 7. Add Tuple Literal Support
**Status: FIXED**
- `TupleExpr` AST node added
- Parse `(1, 2, 3)` as tuples (parenthesized comma-separated expressions)
- `MAKE_TUPLE` IR instruction added
- `build_tuple()` IR builder method implemented
- `pyc_new_tuple()` runtime function added

### 8. Add `in` and `is` Operators
**Status: FIXED**
- `IN` token added to lexer
- `IN` and `IS` added to `BinOpExpr::Op` enum
- `IN` and `IS` IR instructions added
- `pyc_contains()` and `pyc_is()` runtime functions added

### 9. Add Proper `and`/`or` Short-Circuit Evaluation
**Status: FIXED**
- `build_binop()` generates short-circuit IR for AND/OR
- PHI nodes and conditional jumps for short-circuit semantics
- `pyc_and()` and `pyc_or()` runtime functions added
- AND: if left is 0, return 0; else return right
- OR: if left is non-zero, return left; else return right

### 10. Complete `from ... import` with Name Binding
**Status: FIXED**
- Parser captures imported names in `ImportFrom.names_`
- `build_import_stmt()` imports module, then for each name calls `pyc_getattr(module, name)`
- Store each imported name in a global variable

### 11. Add Tuple Unpacking
**Status: FIXED**
- `TupleAssignStmt` AST node added
- Parser detects `a, b = value` syntax
- IR builds tuple then extracts elements via `LIST_GET`

### 12. Add Slice Notation
**Status: FIXED**
- `SliceExpr` AST node with start/stop/step
- Parser detects `a[1:3]` and `a[1:3:2]`
- `SLICE_GET` IR instruction added
- `pyc_slice()` runtime function added

### 13. Add Walrus Operator (`:=`)
**Status: PARTIAL**
- `WALRUS` token added to lexer
- `NamedExpr` AST node added
- `build_named_expr()` in IR builder
- Parser does NOT yet handle the `WALRUS` token

### 14. Add f-strings
**Status: FIXED**
- `FSTRING_START/MIDDLE/END` lexer tokens added
- `JoinedStr` and `FormattedValue` AST nodes added
- Parser handles `f"hello {x}"` syntax
- IR generates `pyc_str_concat` calls

### 15. Add Decorators
**Status: FIXED**
- `AT` lexer token added
- `decorators_` field on `FunctionDef` and `ClassDef`
- Parser handles `@decorator` syntax
- IR generates decorator call sequences

### 16. Add `match`/`case`
**Status: FIXED**
- `parse_match_stmt()` parser function added
- Translates match/case to nested if/elif/else
- Supports value patterns (int, str) and guards

### 17. Add `async`/`await`
**Status: FIXED**
- Parser handles `async def` and `await expr`
- Await treated as simplified call

### 18. Add `yield`/Generators
**Status: FIXED**
- Parser handles `yield` and `yield from`
- IR generates `pyc_yield` call instruction

---

## Performance Plan

### 1. Introduce Type-Specialized IR Instructions
**Current bottleneck:** Every arithmetic operation boxes values into `PyObject*`, calls runtime functions, checks types, and unboxes.

**Plan:**
- Add type metadata to IR: `TypeKind` field to track int/float/object
- In LLVM codegen, emit native `add`/`mul`/`sub` for int/float operations
- Only call runtime functions for object operations (getattr, len, type, isinstance)
- Add `IS_INT`/`IS_FLOAT` type check instructions

### 2. Add IR-Level Constant Folding
**Current bottleneck:** No compile-time evaluation of constant expressions.

**Plan:**
- After building each IR function, run constant folding pass over linear blocks
- Check if all operands are constants, evaluate and replace with `LOADCONST`
- Handle arithmetic ops, comparisons, and boolean ops
- Expected speedup: 20-40% for programs with many constant expressions

### 3. Inline Small Built-in Functions
**Current bottleneck:** Every builtin call goes through `std::function` indirection.

**Plan:**
- In LLVM codegen, detect calls to known builtins by name and emit inline code
- Eliminate LOADGLOBAL step for common builtins
- Expected speedup: 30-50% for programs heavy on builtin calls

### 4. Optimize Object Allocation with Arena Allocator
**Current bottleneck:** Every `new PyObject()` goes through `malloc`/`new` with per-object overhead.

**Plan:**
- Create `PyObjectArena` class that pre-allocates 4KB blocks
- Track allocations per arena; free entire arena at once
- Add arena per function scope; reclaim at function return
- Expected speedup: 15-30% for programs with heavy object creation

### 5. Enable LLVM Inlining and Profile-Guided Optimization
**Current bottleneck:** Aggressive inlining not configured.

**Plan:**
- Configure LLVM passes for aggressive inlining
- Add `alwaysinline` attribute to small runtime functions
- Use `opt` command-line tool to inspect generated LLVM IR
- Expected speedup: 10-25% for programs with many small function calls

---

## Future Implementation Plans

### Issue: Add `sys` Module Stub
**Location:** `runtime/builtins.cpp`
**Severity:** Medium
**Status: NOT STARTED**
- Implement `sys.version`, `sys.platform`, `sys.argv` as globals
- Implement `sys.exit(code)` as a special return that terminates program
- Implement `sys.getsizeof(obj)` using registry stats

### Issue: Implement `repr()` Properly
**Location:** `runtime/builtins.cpp`
**Severity:** Medium
**Status: NOT STARTED**
- Implement `repr()` with proper type-specific formatting
- List: `[repr(elem) for elem in list]`
- Dict: `{repr(k): repr(v) for k,v in dict}`
- String: `"'value'"` (with quotes)

### Issue: Add `__main__` Entry Point Detection
**Location:** `main.cpp`, `ir/ir.h`
**Severity:** Medium
**Status: NOT STARTED**
- Detect if module is run as main (not imported)
- Emit call to `__main__` function at program start
- If imported, only define functions without calling main

---

## Performance Plans: Advanced Optimizations

### 1. Add Dead Code Elimination at IR Level
**Plan:**
- Remove unused instructions and unreachable basic blocks
- Remove unused function definitions (not in call graph from __main__)
- Expected: 10-20% reduction in generated LLVM IR size

### 2. Optimize Global Variable Access
**Plan:**
- Track which globals are read/written in each function
- Promote frequently-accessed globals to local variables (SSA form)
- Expected: 5-15% speedup for programs with many global variable accesses

### 3. Implement Tail Call Optimization
**Plan:**
- Detect tail calls and emit `tail` call attribute in LLVM
- Enables recursive functions to use O(1) stack space

### 4. Add Type-Based Specialization for Common Patterns
**Plan:**
- Analyze IR to detect pure-integer or pure-float functions
- Generate specialized LLVM IR that uses `i64`/`f64` instead of `i8*` (PyObject*)
- Expected: 3-10x speedup for numeric computation functions

### 5. Integrate LLVM `opt` with Aggressive Flags
**Plan:**
- Run `opt -O3` on generated LLVM IR before codegen
- Enable `inliner`, `instcombine`, `licm`, `loop-unroll`, `slp-vectorize`
- Expected: 10-30% speedup across all programs

---

## MVP Assessment: Are We Close?

**Verdict: Approximately 80% complete for a basic Python compiler MVP**

### What Works
- Arithmetic, comparison, boolean operators
- Functions with parameters and return values
- Classes with `__init__` and methods
- `if/elif/else` conditionals
- `while` loops, `for` loops with `range()`
- Lists, dicts, strings, tuples, dict/tuple literals
- Function calls (named functions and lambdas)
- Lambda expressions
- List/set/dict comprehensions
- Basic exception handling (`raise`, `try/except/else/finally`)
- Class instantiation
- Import system (file-based module loading, packages, submodules, name binding, relative imports, namespace packages)
- `in` and `is` operators
- Proper `and`/`or` short-circuit evaluation
- 60+ builtin functions (type conversion, numeric, iteration, membership, list/dict/string methods, format, dir, globals, locals, chr, ord, hex, oct, bin)
- Tuple unpacking, slice notation, f-strings, decorators, match/case, async/await, yield

### Significant Remaining Issues

#### 1. Missing Core Language Features (HIGH PRIORITY)
| Feature | Status |
|---------|--------|
| Walrus operator `:=` | PARTIAL (lexer + AST + IR builder, parser missing) |
| `sys` module | NOT IMPLEMENTED |
| `repr()` proper formatting | NOT IMPLEMENTED |
| `__main__` entry point | NOT IMPLEMENTED |
| `with` statement context manager | PARTIAL (parser handles syntax, runtime not complete) |
| Augmented assignment (`%=` etc.) | PARTIAL (only `+=`, `-=`, `*=`, `/=` work) |

#### 2. Performance Gap vs CPython (MEDIUM PRIORITY)
| Issue | Current State |
|-------|---------------|
| No type specialization | All ops through PyObject* |
| No constant folding | Runtime evaluation of constants |
| No builtin inlining | Function call overhead |
| No arena allocator | Per-object malloc |

#### 3. Runtime Gaps (MEDIUM PRIORITY)
| Builtin | Status |
|---------|--------|
| `exec()` / `eval()` | EXCLUDED (security implications) |
| `input` | Listed but not registered |
| `super()` | NOT DEFINED |
| `property` | NOT DEFINED |
| `staticmethod` | NOT DEFINED |
| `classmethod` | NOT DEFINED |

### MVP Roadmap

**Phase 1: Complete Missing Features**
1. Finish walrus operator (add parser handler)
2. Add `sys` module with `argv`, `exit`
3. Implement `repr()` with proper type formatting
4. Add `__main__` entry point detection

**Phase 2: Performance Foundation**
1. Constant folding at IR level
2. Type metadata in IR for int/float specialization
3. Inline small builtin functions in LLVM codegen

**Phase 3: Testing & Validation**
1. End-to-end test suite with 50+ test cases
2. CPython benchmark comparison
3. Memory leak detection with Valgrind

### Current Capability Summary

| Category | Status | Notes |
|----------|--------|-------|
| **Lexer** | COMPLETE | Handles all Python tokens including LBRACE, RBRACE, IN, WALRUS, FSTRING |
| **Parser** | 95% | Recursive descent parser, handles all Python syntax except walrus operator expression context |
| **AST** | COMPLETE | All node types defined |
| **IR Builder** | 95% | Most features implemented, short-circuit evaluation, finally blocks, match/case |
| **LLVM Codegen** | 90% | All IR instructions handled except BUILD_STRING (no handler) |
| **Interpreter** | 80% | Many intrinsics implemented, globals/locals working, AND/OR value propagation fixed |
| **Runtime** | 85% | 60+ builtins implemented, dict/tuple/and/or/in/is support, import system |
| **GC** | 80% | Core works, registry cleanup implemented |
| **Testing** | 40% | Lexer, parser, IR, codegen tests pass, import test suite added |
| **Performance** | 25% | LLVM O2 optimization enabled, no type specialization yet |

**Overall: 80% complete for MVP**
