#!/bin/bash
# Runtime scalability test
# Measures execution time for programs with increasing function counts

BUILD_DIR="build"
PYC="./$BUILD_DIR/pyc"

echo "PyC Runtime Scalability Test"
echo "============================"
echo ""

# Test 1: Function call scaling
echo "--- Function Call Scaling ---"
for nfuncs in 10 50 100 200; do
    gen_file="/tmp/runtime_funcs_${nfuncs}.py"
    echo "def main():" > "$gen_file"
    
    i=0
    while [ $i -lt $nfuncs ]; do
        echo "def func_${i}(x):" >> "$gen_file"
        echo "    return x + 1" >> "$gen_file"
        echo "" >> "$gen_file"
        i=$((i + 1))
    done
    
    echo "    x = 0" >> "$gen_file"
    echo "    i = 0" >> "$gen_file"
    echo "    while i < 100:" >> "$gen_file"
    echo "        x = x + 1" >> "$gen_file"
    echo "        i = i + 1" >> "$gen_file"
    echo "    print(x)" >> "$gen_file"
    
    # Compile and measure
    output="/tmp/runtime_funcs_${nfuncs}.ll"
    start=$(date +%s%N)
    $PYC "$gen_file" -o "$output" 2>/dev/null
    end=$(date +%s%N)
    compile_ms=$(( (end - start) / 1000000 ))
    
    if [ -f "$output" ]; then
        # Try to link and run
        clang++ -o "/tmp/runtime_funcs_exe" "$output" \
            -l:libpyc_runtime.a -L"$BUILD_DIR" \
            -lpthread 2>/dev/null
        
        if [ -f "/tmp/runtime_funcs_exe" ]; then
            start=$(date +%s%N)
            /tmp/runtime_funcs_exe > /dev/null 2>&1
            end=$(date +%s%N)
            exec_ms=$(( (end - start) / 1000000 ))
            echo "  ${nfuncs} functions: compile=${compile_ms}ms run=${exec_ms}ms"
            rm -f "/tmp/runtime_funcs_exe"
        else
            echo "  ${nfuncs} functions: compile=${compile_ms}ms (link failed)"
        fi
        
        rm -f "$output"
    else
        echo "  ${nfuncs} functions: SKIPPED (parser error)"
    fi
    
    rm -f "$gen_file"
done

echo ""

# Test 2: Global variable scaling
echo "--- Global Variable Scaling ---"
for nglobals in 10 50 100 200; do
    gen_file="/tmp/runtime_globals_${nglobals}.py"
    echo "def main():" > "$gen_file"
    
    i=0
    while [ $i -lt $nglobals ]; do
        echo "g${i} = 0" >> "$gen_file"
        i=$((i + 1))
    done
    
    echo "    x = 0" >> "$gen_file"
    echo "    i = 0" >> "$gen_file"
    echo "    while i < 1000:" >> "$gen_file"
    echo "        x = x + 1" >> "$gen_file"
    echo "        i = i + 1" >> "$gen_file"
    echo "    print(x)" >> "$gen_file"
    
    start=$(date +%s%N)
    $PYC --test-compile "$gen_file" > /dev/null 2>&1
    end=$(date +%s%N)
    compile_ms=$(( (end - start) / 1000000 ))
    
    echo "  ${nglobals} globals: ${compile_ms}ms"
    
    rm -f "$gen_file"
done

echo ""
echo "Runtime scalability test complete."
