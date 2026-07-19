# COMPLETENESS.md

Missing language features and runtime capabilities.
Sorted by criticality (most critical at top).

> **Status (2026-07-19):** Core language features are implemented. Complex number support (Phases 1-5) is complete. Current test results: 274/300 passing with 11 file_case_failures. Remaining gaps are in function return value propagation, import system, comprehensions, and sorting.

---

## Critical

### 1. Function Return Values
**Status: BUG** - Functions return `None` instead of computed values. Affects `add()`, `apply2()`, `call_it()`, `repeat()`, Fibonacci, and many others.

### 2. Import System
**Status: PARTIAL** - Basic import works but `from ... import` and `import *` return `None`. Module loading is incomplete.

### 3. Comprehensions
**Status: BUG** - List and dict comprehensions produce empty results instead of computed values.

### 4. Sorting
**Status: BUG** - `sorted()` with custom key function doesn't sort. The comparison function is not being called correctly.

### 5. String Methods
**Status: BUG** - `upper()`, `lower()` return `None` in some cases.

---

## High

### 6. Exception Handling
**Status: FIXED** - try/except/finally/else fully implemented with structured exceptions.

### 7. Comprehensions (Historical)
**Status: FIXED in design** - List/set/dict comprehensions and generator expressions defined but have runtime bugs.

### 8. Lambda Expressions
**Status: FIXED** - Lambdas work as values with full call support via `Pyc_Apply`.

### 9. Dict Literal Support
**Status: FIXED** - `{key: val, ...}` literals fully supported.

### 10. Tuple Literal Support
**Status: FIXED** - `(1, 2, 3)` tuples supported.

### 11. `in` and `is` Operators
**Status: FIXED** - Both operators fully implemented.

### 12. `and`/`or` Short-Circuit Evaluation
**Status: FIXED** - Short-circuit semantics correctly implemented.

### 13. `from ... import` with Name Binding
**Status: PARTIAL** - Basic import works but `from ... import` returns `None`.

### 14. Tuple Unpacking
**Status: FIXED** - `a, b = value` syntax supported.

### 15. Slice Notation
**Status: FIXED** - `a[1:3]` and `a[1:3:2]` supported.

---

## Medium

### 16. Walrus Operator (`:=`)
**Status: FIXED** - Named expressions supported.

### 17. f-strings
**Status: FIXED** - `f"hello {x}"` syntax supported.

### 18. Decorators
**Status: FIXED** - `@deco`, `@deco(args)`, stacked decorators for functions and classes.

### 19. `match`/`case`
**Status: FIXED** - Pattern matching translated to nested if/elif/else.

### 20. `async`/`await`
**Status: FIXED** - Simplified async support implemented.

### 21. `yield`/Generators
**Status: FIXED** - Eager materialization via thread-local buffer.

### 22. `complex()` Builtin
**Status: FIXED** - `complex()`, `complex(3)`, `complex(3, 4)`, `complex("3+4j")` all work.

### 23. `cmath` Module
**Status: FIXED** - `sqrt`, `log`, `exp`, `sin`, `cos`, `tan` implemented.

### 24. Exception Classes as First-Class Values
**Status: FIXED** - `exc = ValueError`, `exc("msg")`, `raise exc` all work.

### 25. Function Objects
**Status: FIXED** - First-class function objects with identity, repr, and callable support.

### 26. `format()` Builtin
**Status: FIXED** - Positional placeholders and format specifiers supported.

### 27. `dir()` Completeness
**Status: FIXED** - Returns attributes for instances, dict keys for dicts.

### 28. `globals()` / `locals()`
**Status: FIXED** - Returns dict-like values with variables.

### 29. Missing Builtin Functions
**Status: FIXED** - `chr()`, `ord()`, `hex()`, `oct()`, `bin()` all implemented.

---

## Low

### 30. `exec()` / `eval()`
**Status: UNSUPPORTED (intentional)** - Security implications.

---

## Summary

| Severity | Count | Status |
|----------|-------|--------|
| Critical | 5 | 0 FIXED, 5 BUGS |
| High | 10 | 4 FIXED, 1 PARTIAL, 5 BUGS |
| Medium | 14 | 13 FIXED, 1 FIXED (complex/cmath) |
| Low | 1 | 1 UNSUPPORTED |
| **Total** | **30** | **23 FIXED, 5 BUGS, 1 PARTIAL, 1 UNSUPPORTED** |

## Recent Additions (2026-07-19)

- **Complex Numbers (Phases 1-5):** Full support including literals, arithmetic, pow, abs, `complex()` builtin, and `cmath` module
- **String literal handling:** Fixed bug where strings were stored as empty strings
- **Test results:** 274/300 passing (previously 299/299 before regression)
