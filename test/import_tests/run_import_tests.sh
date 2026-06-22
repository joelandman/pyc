#!/bin/bash
# Import system test suite runner
# Tests all import scenarios: simple, packages, namespace packages, relative imports

set -e

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
PYC_BIN="${TEST_DIR}/../../build/pyc"
PASS=0
FAIL=0
TOTAL=0

run_test() {
    local test_name="$1"
    local test_file="$2"
    TOTAL=$((TOTAL + 1))
    
    echo "=== Test $TOTAL: $test_name ==="
    echo "File: $test_file"
    
    # Compile
    local binary_name
    binary_name=$(basename "$test_file" .py)
    if ! $PYC_BIN "$test_file" 2>&1; then
        echo "FAIL: Compilation failed"
        FAIL=$((FAIL + 1))
        return
    fi
    
    # Run
    if ./"$binary_name" 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL: Execution failed"
        FAIL=$((FAIL + 1))
    fi
    echo ""
    
    # Clean up
    rm -f "$binary_name"
}

cd "$TEST_DIR"

echo "========================================="
echo "Import System Test Suite"
echo "========================================="
echo ""

# Test 1: Simple module import
run_test "Simple module import" "test_simple_import.py"

# Test 2: Multi-module import
run_test "Multi-module import" "test_multi_import.py"

# Test 3: From import
run_test "From import" "test_from_import.py"

# Test 4: Multi from import
run_test "Multi from import" "test_multi_from_import.py"

# Test 5: Nested package
run_test "Nested package" "test_nested_package.py"

# Test 6: Namespace package
run_test "Namespace package" "test_namespace_package.py"

# Test 7: Relative import sibling
run_test "Relative import sibling" "test_relative_sibling.py"

# Test 8: Relative import parent (run from within relative_imports/child/)
cd relative_imports/child
run_test "Relative import parent" "test_relative_parent.py"
cd ../..

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
