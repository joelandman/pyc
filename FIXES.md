# Compiler Fixes and Improvements

## Overview
This commit addresses several issues in the pyc compiler related to module initialization, imports, and argument parsing.

## Changes

### 1. Fixed `__name__` Module Variable (src/Compiler.cpp)
**Issue:** The `__name__` variable was being set to `"__module__"` instead of `"__main__"` for the top-level module.

**Fix:** Changed line 123-125 to always use `"__main__"` for the module's `__name__`:
```cpp
// Before:
std::string nameVal = "c" + std::to_string(tempCounter++);
ir.addInstruction(currentFunc, "const", {"\"" + currentFunc + "\""}, nameVal, "str");

// After:
std::string moduleName = "__main__";
std::string nameVal = "c" + std::to_string(tempCounter++);
ir.addInstruction(currentFunc, "const", {"\"" + moduleName + "\""}, nameVal, "str");
```

### 2. Fixed ImportFrom for time.perf_counter (src/Compiler.cpp)
**Issue:** When importing from a non-compiled module like `time`, the code was calling `pyc_import_failed` before checking for special cases.

**Fix:** Reorganized the import logic to:
1. First check for special cases (like `time.perf_counter`)
2. Only call `pyc_import_failed` if no special cases apply
3. Use `handledSpecial` flag to track if all imports were handled

### 3. Fixed Argument Parsing (src/main.cpp)
**Issue:** The argument parsing loop started at index 2, skipping the first argument after the command.

**Fix:** Changed the loop to start at index 1 and properly collect the input file:
```cpp
// Before:
for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    ...
}

// After:
std::string input;
for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (input.empty()) input = arg;
    ...
}
```

## Known Issues (Resolved)

### ~~Pyc_Apply Dispatch for main()~~ — Fixed
The `mbs.py` test used to segfault before ever reaching `main()`. The root
cause turned out to be unrelated to `__name__ == "__main__"` comparison
(that path worked correctly once `__name__` was fixed above): module-level
float globals with a literal or negation right-hand side (e.g.
`xmin = -1.5`) were being misclassified as native-local doubles by codegen
and never actually stored into their boxed `pyc_global_*` slot, so `fill_z`
dereferenced a null pointer. Fixed by gating the native-local fast paths in
`Codegen.cpp`'s `"assign"` handling on whether the target is actually a
declared module global, and by fixing a related leak where a nested
function's float-typed parameter names could persist into the enclosing
scope's `numericFloatLocals` set. See commit `16548be`.

### Import/module system — substantially expanded since this commit
The import handling described above (same-directory modules and `time.perf_counter`)
has since grown into a much fuller system: dotted package imports, nested and
namespace packages, and relative imports (`from . import x`) are all now
supported. See `FEATURES.md`'s Import System section and commits `be832b3`
and the relative-imports work that followed it.

## Testing
- Simple tests with `print()` statements work correctly
- Import handling for standard library modules is improved
