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

## Known Issues

### Pyc_Apply Dispatch for main()
The `mbs.py` test still has issues with calling the user's `main()` function. The branch condition checking `__name__ == "__main__"` is not evaluating correctly, preventing `pyc_py_main()` from being executed.

**Root Cause:** The comparison `PyObject_CompareBool` is returning 0 (false) even when comparing identical strings `"__main__"`. This suggests either:
1. String comparison in the runtime has an issue
2. The values being compared are not actually equal
3. There's a mismatch between the LLVM IR and runtime expectations

**Next Steps:** Need to investigate the string comparison logic in `PyObject_CompareBool` and verify that the strings being compared are correctly set to `"__main__"`.

## Testing
- Simple tests with `print()` statements work correctly
- The `mbs.py` test runs but exits before executing user code
- Import handling for standard library modules is improved
