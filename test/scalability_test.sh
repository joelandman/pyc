#!/bin/bash
# Scalability testing for pyc compiler
# Measures compile time, binary size, and runtime for programs of increasing size

BUILD_DIR="build"
PYC="./$BUILD_DIR/pyc"
RESULTS="test/scalability_results.txt"

if [ ! -x "$PYC" ]; then
    echo "Error: Compiler not found at $PYC"
    exit 1
fi

echo "PyC Scalability Test Results" > "$RESULTS"
echo "=============================" >> "$RESULTS"
date >> "$RESULTS"
echo "" >> "$RESULTS"

# Test 1: Compile time for programs of increasing size
echo "=== Compile Time (lines of code) ===" >> "$RESULTS"

for lines in 10 50 100 500; do
    # Generate a program with N lines
    gen_file="/tmp/scal_gen_${lines}.py"
    echo "def main():" > "$gen_file"
    echo "    x = 0" >> "$gen_file"
    
    i=0
    while [ $i -lt $lines ]; do
        echo "    x = x + $i" >> "$gen_file"
        i=$((i + 1))
    done
    
    echo "    print(x)" >> "$gen_file"
    
    # Measure compile time
    start=$(date +%s%N)
    $PYC --test-compile "$gen_file" > /dev/null 2>&1
    end=$(date +%s%N)
    compile_ms=$(( (end - start) / 1000000 ))
    
    echo "  ${lines} lines: ${compile_ms}ms" >> "$RESULTS"
    
    rm -f "$gen_file"
done

echo "" >> "$RESULTS"

# Test 2: Binary size for programs of increasing size
echo "=== LLVM IR Size (bytes) ===" >> "$RESULTS"

for lines in 10 50 100 500; do
    gen_file="/tmp/scal_gen_${lines}.py"
    echo "def main():" > "$gen_file"
    echo "    x = 0" >> "$gen_file"
    
    i=0
    while [ $i -lt $lines ]; do
        echo "    x = x + $i" >> "$gen_file"
        i=$((i + 1))
    done
    
    echo "    print(x)" >> "$gen_file"
    
    output_file="/tmp/scal_out_${lines}.ll"
    $PYC "$gen_file" -o "$output_file" 2>/dev/null
    
    if [ -f "$output_file" ]; then
        size=$(wc -c < "$output_file")
        echo "  ${lines} lines: ${size} bytes" >> "$RESULTS"
        rm -f "$output_file"
    else
        echo "  ${lines} lines: SKIPPED (parser error)" >> "$RESULTS"
    fi
    
    rm -f "$gen_file"
done

echo "" >> "$RESULTS"
echo "Scalability test complete." >> "$RESULTS"
cat "$RESULTS"
