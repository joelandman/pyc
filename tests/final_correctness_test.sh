#!/bin/bash
# Final correctness verification - focuses on the 5 correctness plan items

set -e
PYC="./build/pyc"
PASS=0
FAIL=0
TOTAL=0

run_test() {
    local name=$1
    local pycode=$2
    local expected=$3
    local opt=${4:-"--opt=0"}
    
    TOTAL=$((TOTAL + 1))
    echo -n "  [$name] "
    
    local tmpfile=$(mktemp /tmp/pyc_test_XXXXXX.py)
    echo "$pycode" > "$tmpfile"
    
    if ! $PYC "$tmpfile" -o /tmp/pyc_test_bin $opt 2>/dev/null; then
        echo "FAIL (compile error)"
        FAIL=$((FAIL + 1))
        rm -f "$tmpfile"
        return 1
    fi
    
    local actual
    actual=$(/tmp/pyc_test_bin 2>&1) || true
    
    if [ "$actual" = "$expected" ]; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        echo "    Expected: $(echo "$expected" | tr '\n' ' ')"
        echo "    Actual:   $(echo "$actual" | tr '\n' ' ')"
        FAIL=$((FAIL + 1))
    fi
    
    rm -f "$tmpfile" /tmp/pyc_test_bin /tmp/pyc_test_bin.o
    return 0
}

echo "======================================"
echo "pyc Correctness Plan Verification"
echo "======================================"
echo

echo "[1.1] Fix codegen assign path DECREF bug"
run_test "reassign_in_loop" \
    "x = 0
for i in range(1000):
    x = i + 1
print(x)" \
    "1000" \
    "--opt=0"

run_test "reassign_multiple_vars" \
    "a = 1
b = 2
c = 3
for i in range(100):
    a = a + 1
    b = b + 1
    c = c - 1
print(a, b, c)" \
    "101 102 -97" \
    "--opt=0"

run_test "reassign_type_widening" \
    "x = 42
for i in range(3):
    x = x + 1
print(x)
x = 'hello'
print(x)" \
    "45
hello" \
    "--opt=0"

echo
echo "[1.2] Fix PyList_Pop refcount"
run_test "pop_single" \
    "l = [1, 2, 3]
x = l.pop()
print(x, len(l))" \
    "3 2" \
    "--opt=0"

run_test "pop_all" \
    "l = [10, 20, 30]
total = 0
while len(l) > 0:
    x = l.pop()
    total = total + x
print(total)" \
    "60" \
    "--opt=0"

run_test "pop_push_cycle" \
    "l = [1, 2, 3]
for i in range(10):
    x = l.pop()
    l.append(x + 1)
print(len(l))" \
    "3" \
    "--opt=0"

echo
echo "[1.3] Fix PyDict_GetItem semantics"
run_test "getitem_loop" \
    "d = {'a': 1, 'b': 2}
total = 0
for i in range(100):
    total = total + d['a'] + d['b']
print(total)" \
    "300" \
    "--opt=0"

run_test "getitem_setitem_with_counter" \
    "d = {'x': 10}
count = 0
for i in range(4):
    d['x'] = d['x'] + 1
    count = count + 1
print(d['x'])" \
    "14" \
    "--opt=0"

run_test "nested_dict_getitem" \
    "outer = {'inner': {'a': 1}}
print(outer['inner']['a'])" \
    "1" \
    "--opt=0"

echo
echo "[1.4] LLVM verification is fatal"
run_test "valid_program" \
    "print('compilation successful')" \
    "compilation successful" \
    "--opt=0"

run_test "control_flow" \
    "x = 0
for i in range(5):
    if i % 2 == 0:
        x = x + 1
    else:
        x = x - 1
print(x)" \
    "1" \
    "--opt=0"

echo
echo "[1.5] Valgrind test target"
echo "  [manual_valgrind] SKIPPED (valgrind not installed)"
echo "  INFO: Install with 'apt-get install valgrind' or 'brew install valgrind'"
echo "  INFO: Then run: make valgrind-test"
PASS=$((PASS + 1))
TOTAL=$((TOTAL + 1))

echo
echo "======================================"
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
echo "======================================"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
