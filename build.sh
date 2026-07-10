#!/usr/bin/env bash
# build.sh - Build script for pyc (Python AOT Compiler)
#
# Usage:
#   ./build.sh                  # Build in-place (build/ directory)
#   ./build.sh --clean          # Clean build artifacts
#   ./build.sh --install /path  # Build and install to /path (bin, lib, include)
#
# Options:
#   --clean    Remove the build/ directory
#   --install  Build and install the compiler to the specified prefix
#   --help     Show this help message

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_PREFIX=""
CLEAN=false
BUILD=true

# Parse arguments
INSTALL_PATH=""
for arg in "$@"; do
    case "$arg" in
        --clean)
            CLEAN=true
            BUILD=false
            ;;
        --install)
            BUILD=true
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --clean              Remove the build/ directory"
            echo "  --install /path      Build and install pyc to /path"
            echo "                         /path/bin  - pyc compiler binary"
            echo "                         /path/lib  - runtime library headers"
            echo "                         /path/include - public API headers"
            echo "  --help, -h           Show this help message"
            exit 0
            ;;
        *)
            if [ -n "$INSTALL_PATH" ]; then
                echo "Error: unexpected argument: $arg"
                echo "Use --help for usage information"
                exit 1
            fi
            INSTALL_PATH="$arg"
            ;;
    esac
done

if [ -n "$INSTALL_PATH" ]; then
    INSTALL_PREFIX="$INSTALL_PATH"
fi

# Check for required tools
command -v cmake >/dev/null 2>&1 || { echo "Error: cmake is required but not installed."; exit 1; }
command -v make >/dev/null 2>&1 || { echo "Error: make is required but not installed."; exit 1; }

echo "========================================"
echo " PyC Compiler Build"
echo "========================================"
echo ""

# Clean step
if [ "$CLEAN" = true ]; then
    echo "Cleaning build artifacts..."
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        echo "  Removed ${BUILD_DIR}"
    fi
    echo "Done."
    exit 0
fi

# Build step
if [ "$BUILD" = true ]; then
    echo "Configuring build..."
    mkdir -p "$BUILD_DIR"
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    echo ""
    echo "Building..."
    make -C "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    echo ""
    echo "Build complete. Binary: ${BUILD_DIR}/pyc"
    echo ""

    # Run tests (only if binary works)
    if [ -f "${BUILD_DIR}/pyc" ] && [ -f "${SCRIPT_DIR}/tests/runner.py" ]; then
        echo "Running tests..."
        # Unset LD_LIBRARY_PATH to avoid conflicts with local Python installs
        env -u LD_LIBRARY_PATH python3 -c "
import os, sys
os.environ['PYC_BINARY'] = '${BUILD_DIR}/pyc'
sys.path.insert(0, '${SCRIPT_DIR}')
from tests import runner
sys.exit(0 if runner.main() else 1)
" 2>&1 || echo "Some tests failed (non-fatal)."
        echo ""
    fi
fi

# Install step
if [ -n "$INSTALL_PREFIX" ]; then
    echo "========================================"
    echo " Installing pyc to ${INSTALL_PREFIX}"
    echo "========================================"
    echo ""

    BIN_DIR="${INSTALL_PREFIX}/bin"
    LIB_DIR="${INSTALL_PREFIX}/lib"
    INCLUDE_DIR="${INSTALL_PREFIX}/include"

    # Create directories
    mkdir -p "$BIN_DIR"
    mkdir -p "$LIB_DIR/pyc/runtime"
    mkdir -p "$INCLUDE_DIR/pyc"

    # Install binary
    echo "Installing binary..."
    cp -f "${BUILD_DIR}/pyc" "${BIN_DIR}/pyc"
    chmod +x "${BIN_DIR}/pyc"
    echo "  pyc -> ${BIN_DIR}/pyc"

    # Install runtime library headers (runtime/ directory - used by generated code)
    echo "Installing runtime library headers..."
    for hdr in runtime/*.h; do
        cp -f "$hdr" "${LIB_DIR}/pyc/runtime/"
        echo "  ${hdr} -> ${LIB_DIR}/pyc/runtime/"
    done

    # Install public API headers (include/pyc/ directory)
    echo "Installing public API headers..."
    for hdr in include/pyc/*.h; do
        cp -f "$hdr" "${INCLUDE_DIR}/pyc/"
        echo "  ${hdr} -> ${INCLUDE_DIR}/pyc/"
    done

    echo ""
    echo "========================================"
    echo " Installation complete"
    echo "========================================"
    echo ""
    echo "  Binary:  ${BIN_DIR}/pyc"
    echo "  Headers: ${INCLUDE_DIR}/pyc/"
    echo "  Runtime: ${LIB_DIR}/pyc/runtime/"
    echo ""
    echo "Usage: ${BIN_DIR}/pyc <source.py> -o <output>"
fi
