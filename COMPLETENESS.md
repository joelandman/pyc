# COMPLETENESS.md

Missing language features and runtime capabilities.
Sorted by criticality (most critical at top).

> **Status (2026-07-19):** Core language features are implemented. Complex number support (Phases 1-5) is complete. Current test results: **300/300 passing** with file_case_failures=0. All known bugs have been fixed.

---

## Critical

### 1. Function Return Values
**Status: FIXED** - Functions now correctly return computed values. The bug was caused by complex arithmetic being incorrectly triggered for all boxed operands. Fixed by using `complexVars` set to track actual complex numbers.

### 2. Import System
**Status: FIXED** - Basic import, `from ... import`, and `from ... import *` all work correctly for same-directory `.py` modules. External modules (like `re`, `math`) are not supported.

### 3. Comprehensions
**Status: FIXED** - List and dict comprehensions now produce correct results.

### 4. Sorting
**Status: FIXED** - `sorted()` with custom key function now works correctly.

### 5. String Methods
**Status: FIXED** - `upper()`, `lower()`, `strip()`, `split()`, `join()` all work correctly.

---

## High

### 6. Exception Handling
**Status: FIXED** - try/except/finally/else fully implemented with structured exceptions.

### 7. Comprehensions (Historical)
**Status: FIXED** - List/set/dict comprehensions and generator expressions fully working.

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
**Status: FIXED** - `from X import Y` and `from X import *` work correctly for same-directory `.py` modules.

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
| Critical | 5 | 5 FIXED |
| High | 10 | 10 FIXED |
| Medium | 14 | 14 FIXED |
| Low | 1 | 1 UNSUPPORTED |
| **Total** | **30** | **29 FIXED, 1 UNSUPPORTED** |

## Recent Additions (2026-07-19)

- **Complex Numbers (Phases 1-5):** Full support including literals, arithmetic, pow, abs, `complex()` builtin, and `cmath` module
- **String literal handling:** Fixed bug where strings were stored as empty strings
- **Function return values:** Fixed bug where functions returned `None` due to complex arithmetic being incorrectly triggered for all boxed operands
- **Import system:** Verified working for same-directory `.py` modules (`import`, `from ... import`, `from ... import *`, `import X as Y`)
- **Test results:** 300/300 passing (all tests passing)
