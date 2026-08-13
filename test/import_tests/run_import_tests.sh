#!/bin/bash
# Import system test suite runner
# Tests all import scenarios: simple, packages, namespace packages, relative imports

set -e

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
# Honor PYC_BIN / PYC_BINARY so out-of-tree builds (build_debug, cmake -B)
# do not silently pick a stale repo-root build/pyc. Always resolve to an
# absolute path: this script cds into fixture directories.
if [ -z "${PYC_BIN:-}" ]; then
    PYC_BIN="${PYC_BINARY:-${TEST_DIR}/../../build/pyc}"
fi
if [ ! -x "$PYC_BIN" ]; then
    echo "ERROR: pyc binary not found or not executable: $PYC_BIN"
    echo "Set PYC_BIN or PYC_BINARY to an absolute path, or build ./build/pyc."
    exit 1
fi
PYC_BIN="$(cd "$(dirname "$PYC_BIN")" && pwd)/$(basename "$PYC_BIN")"
PASS=0
FAIL=0
TOTAL=0

# Compile $2 to a binary named after its basename. Prints the compiler's
# output and returns non-zero on failure. Leaves nothing behind on failure.
compile_test() {
    local test_file="$1"
    local binary_name="$2"
    local log
    log=$(mktemp)
    if ! "$PYC_BIN" "$test_file" -o "$binary_name" >"$log" 2>&1; then
        echo "FAIL: Compilation failed"
        cat "$log"
        rm -f "$log"
        return 1
    fi
    rm -f "$log"
    return 0
}

# Run a test and assert its stdout matches expected_output exactly
# (stderr, which carries pyc's runtime DEBUG tracing, is discarded).
run_test() {
    local test_name="$1"
    local test_file="$2"
    local expected_output="$3"
    TOTAL=$((TOTAL + 1))

    echo "=== Test $TOTAL: $test_name ==="
    echo "File: $test_file"

    local binary_name
    binary_name=$(basename "$test_file" .py)
    if ! compile_test "$test_file" "$binary_name"; then
        FAIL=$((FAIL + 1))
        echo ""
        return
    fi

    local actual_output
    if ! actual_output=$(./"$binary_name" 2>/dev/null); then
        echo "FAIL: Execution failed (nonzero exit)"
        FAIL=$((FAIL + 1))
    elif [ "$actual_output" != "$expected_output" ]; then
        echo "FAIL: Output mismatch"
        echo "  expected: $(printf '%q' "$expected_output")"
        echo "  actual:   $(printf '%q' "$actual_output")"
        FAIL=$((FAIL + 1))
    else
        echo "PASS"
        PASS=$((PASS + 1))
    fi
    echo ""

    rm -f "$binary_name" "$binary_name.o" "$binary_name"_b7_modules.c
}

# Smoke test: only checks that the binary runs without crashing. Used for
# cases that are not valid Python to begin with when run as a direct script
# (e.g. relative imports with no package context — CPython itself raises
# "ImportError: attempted relative import with no known parent package" for
# these), so there is no correct expected output to assert against.
run_smoke_test() {
    local test_name="$1"
    local test_file="$2"
    TOTAL=$((TOTAL + 1))

    echo "=== Test $TOTAL: $test_name (smoke only) ==="
    echo "File: $test_file"

    local binary_name
    binary_name=$(basename "$test_file" .py)
    if ! compile_test "$test_file" "$binary_name"; then
        FAIL=$((FAIL + 1))
        echo ""
        return
    fi

    if ./"$binary_name" >/dev/null 2>/dev/null; then
        echo "PASS (ran without crashing)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: Execution failed (nonzero exit / crash)"
        FAIL=$((FAIL + 1))
    fi
    echo ""

    rm -f "$binary_name" "$binary_name.o" "$binary_name"_b7_modules.c
}

cd "$TEST_DIR"

echo "========================================="
echo "Import System Test Suite"
echo "========================================="
echo ""

# Test 1: Simple module import
run_test "Simple module import" "test_simple_import.py" "42"

# Test 2: Multi-module import
run_test "Multi-module import" "test_multi_import.py" "$(printf '42\nhello from mod_a1')"

# Test 3: From import
run_test "From import" "test_from_import.py" "hello from mod_a1"

# Test 4: Multi from import
run_test "Multi from import" "test_multi_from_import.py" "$(printf 'hello from mod_a1\ngoodbye from mod_a2')"

# Test 5: Nested package
run_test "Nested package" "test_nested_package.py" "from subpkg.mod_b1"

# Test 6: Namespace package
run_test "Namespace package" "test_namespace_package.py" "from namespace package"

# Test 7: Relative import sibling
# NOTE: `from . import sibling` is not valid Python when run as a direct
# script (no package context) — CPython itself raises "ImportError:
# attempted relative import with no known parent package" for this exact
# file. There is no correct expected stdout to assert, so this is a smoke
# test only (verifies pyc doesn't crash on it).
run_smoke_test "Relative import sibling" "test_relative_sibling.py"

# Test 8: Relative import parent (run from within relative_imports/child/)
# Same caveat as Test 7: `from .. import sibling` has no valid parent
# package when run directly; CPython also raises ImportError here.
cd relative_imports/child
run_smoke_test "Relative import parent" "test_relative_parent.py"
cd ../..

# Test 9: Relative import used from within a package, reached via a normal
# absolute import from the main script — unlike Tests 7/8, this is valid
# Python: relative_imports/child/child_module.py does `from .. import
# sibling` internally, which is legal because it's never run directly.
run_test "Relative import from package" "test_relative_from_package.py" "from child module: from sibling"

echo "========================================="
echo "Results: $PASS passed, $FAIL failed, $TOTAL total"
echo "========================================="

if [ $FAIL -eq 0 ]; then
    echo "All tests PASSED!"
    exit 0
else
    echo "Some tests FAILED!"
    exit 1
fi
