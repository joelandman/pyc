#!/bin/bash
# Benchmark runner for pyc compiler
# Measures compile time and execution time for benchmark programs

BENCHMARKS_DIR="test/benchmarks"
BUILD_DIR="build"

if [ ! -d "$BENCHMARKS_DIR" ]; then
    echo "Error: $BENCHMARKS_DIR not found"
    exit 1
fi

echo "PyC Benchmark Suite"
echo "==================="
echo ""

# Compile the compiler
echo "Building compiler..."
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1
cd ..

PYC="./$BUILD_DIR/pyc"

if [ ! -x "$PYC" ]; then
    echo "Error: Compiler not found at $PYC"
    exit 1
fi

echo "Running benchmarks..."
echo ""

for bench in "$BENCHMARKS_DIR"/*.py; do
    name=$(basename "$bench" .py)
    echo "--- $name ---"
    
    # Test lexer
    start=$(date +%s%N)
    $PYC --test-compile "$bench" > /dev/null 2>&1
    end=$(date +%s%N)
    lexer_time=$(( (end - start) / 1000000 ))
    echo "  Lexer: ${lexer_time}ms"
    
    # Try full compilation (requires working lark parser)
    output_file="/tmp/pyc_${name}.ll"
    start=$(date +%s%N)
    $PYC "$bench" -o "$output_file" 2>/dev/null
    end=$(date +%s%N)
    compile_time=$(( (end - start) / 1000000 ))
    
    if [ -f "$output_file" ]; then
        echo "  Compile: ${compile_time}ms"
        echo "  LLVM IR: $(wc -c < "$output_file") bytes"
        
        # Try to compile to executable and run
        clang++ -o "/tmp/pyc_${name}" "$output_file" \
            -l:libpyc_runtime.a \
            -L"$BUILD_DIR" \
            -l:libpyc_runtime.a \
            -lpthread 2>/dev/null
        
        if [ -f "/tmp/pyc_${name}" ]; then
            start=$(date +%s%N)
            output=$(/tmp/pyc_${name} 2>&1)
            end=$(date +%s%N)
            exec_time=$(( (end - start) / 1000000 ))
            echo "  Run: ${exec_time}ms"
            echo "  Output: $output"
            rm -f "/tmp/pyc_${name}"
        else
            echo "  Run: SKIPPED (link failed)"
        fi
        
        rm -f "$output_file"
    else
        echo "  Compile: SKIPPED (parser error - lark not available)"
    fi
    
    echo ""
done

echo "Benchmark suite complete."
