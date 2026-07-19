# Generator Implementation Plan for pyc

## 1. Problem Statement

`yield` and `yield from` are parsed by the lexer/parser and produce `ast::YieldExpr` AST nodes,
but the compiler had no real support for generator semantics.

## 2. Design Decision: Eager Materialization (Phase 1)

**Approach:** Generator functions run to completion, collecting all yielded values into a list.
The list is returned as the result. This is the same strategy already used for `GeneratorExp`.

## 3. Implementation Status: COMPLETE

### 3.1 Runtime (`src/runtime/Runtime.cpp`, `include/pyc/runtime.h`)

Added three runtime functions:
- `pyc_yield_collect(PyObject* value)` — appends to thread-local buffer, returns value
- `pyc_get_yield_buffer(void)` — returns the collected list, clears buffer
- `pyc_clear_yield_buffer(void)` — clears the buffer

### 3.2 Parser (`src/frontend/PythonParser.cpp`)

Added handling for `Yield` and `YieldFrom` node types from Python C API:
- `Yield` → renamed to `YieldExpr`, with optional `is_from` flag in node args
- `YieldFrom` → renamed to `YieldExpr` with `is_from=1`

### 3.3 IR Builder (`ir/builder.cpp`)

Updated `build_yield()` to call `pyc_yield_collect` and handle `yield from` by iterating
the sub-generator's result list.

### 3.4 Compiler (`src/Compiler.cpp`)

- Added `generatorFunctions` set to track generator functions (containing yield)
- Added `scanForGenerators()` to pre-scan function bodies for yield expressions
- Added `lowerYield()` method to handle yield/yield from expression lowering
- Modified `lowerCall()` to wrap generator calls with clear→call→get_buffer
- Modified `lowerExpr()` to dispatch YieldExpr to `lowerYield()`

### 3.5 Tests

Added `tests/generators.py` with tests for:
- Basic generator with yield
- `yield from` delegation
- Generator with return value
- Nested generators
- Generator expressions

## 4. How It Works

1. **Generator detection:** During module lowering, `scanForGenerators()` pre-scans all
   function bodies for `YieldExpr` nodes and records generator function names.

2. **Generator call wrapping:** When a generator function is called, `lowerCall()` wraps
   the call with:
   - `pyc_clear_yield_buffer()` — clear the thread-local buffer
   - `call <generatorFunc>(args)` — run the generator (yields append to buffer)
   - `pyc_get_yield_buffer()` — return the collected list

3. **Yield expression lowering:** `lowerYield()` emits calls to `pyc_yield_collect(value)`
   which appends the value to the thread-local buffer and returns it.

4. **Yield from lowering:** For `yield from subgen()`, the sub-generator is called directly
   (no wrapper), and the result list is iterated, yielding each element.

## 5. Known Limitations (Phase 1)

- No lazy evaluation: generators are eagerly materialized
- No `.send()` method: `yield` always returns the yielded value
- No `.throw()` method
- No `StopIteration` from generators (they return normally with the collected list)

## 6. Future Work (Phase 2+)

- Chunked lazy materialization (~16 elements per refill)
- `.send(value)` support
- `.throw(exception)` support
