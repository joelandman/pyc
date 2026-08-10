# Tuple Literal Support

## Overview

The pyc compiler now correctly handles Python tuple literals in source code. Previously, tuple literals were incorrectly constructed as lists; this has been fixed to properly create Python tuples using `PyTuple_New` and `PyTuple_SetItem`.

## Implementation Details

- Added `"tuple"` type tracking in `typeOf()` function in `src/Compiler.cpp`
- Modified `lowerList()` to use `PyTuple_New` and `PyTuple_SetItem` for tuple literals
- Updated `noteType()` to track "tuple" type information
- Verified correct behavior with test cases

## Test Cases

The following test cases now work correctly:

```python
# Tuple literal
x = (1, 2, 3)
print(x)  # Output: (1, 2, 3)

# Nested tuples
y = ((1, 2), (3, 4))
print(y)  # Output: ((1, 2), (3, 4))

# Empty tuple
z = ()
print(z)  # Output: ()
```

## Verification

All tests pass with `PYC_BINARY=./build/pyc python3 tests/runner.py`.

## Note

This change ensures pyc's behavior matches CPython for tuple literals, where tuples are distinct from lists and maintain their type throughout the program.