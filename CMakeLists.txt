cmake_minimum_required(VERSION 3.20)

# Disable problematic LLVM library checks
set(LLVM_ENABLE_FFI OFF CACHE BOOL "Use FFI" FORCE)
set(USE_LIBEDIT OFF CACHE BOOL "Use LibEdit" FORCE)

project(pyc_compiler C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

# Find dependencies
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_VERSION_MAJOR}")

# LLVM include and library directories
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

# Source files
set(SOURCES
    frontend/lexer.cpp
    frontend/ast.cpp
    frontend/lark_parser.cpp
    frontend/parser.cpp
    ir/ir.cpp
    ir/builder.cpp
    ir/interpreter.cpp
    codegen/ir2ll.cpp
    codegen/llir_gen.cpp
    runtime/object.cpp
    runtime/object_registry.cpp
    runtime/libpyc_runtime.cpp
    runtime/builtins.cpp
    runtime/gc.cpp
    main.cpp
)

# Header files
set(HEADERS
    frontend/ast.h
    frontend/lexer.h
    frontend/parser.h
    frontend/lark_parser.h
    ir/ir.h
    ir/builder.h
    ir/interpreter.h
    codegen/ir2ll.h
    codegen/llir_gen.h
    runtime/object.h
)

# Create executable
add_executable(pyc ${SOURCES})

# Add project root as include directory so "frontend/..." works
target_include_directories(pyc PRIVATE ${CMAKE_SOURCE_DIR})

# Link LLVM
target_include_directories(pyc PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(pyc PRIVATE ${LLVM_LIBRARIES} LLVM)

# Install target
install(TARGETS pyc DESTINATION bin)

# Test suite
enable_testing()
add_test(NAME test_lexer COMMAND pyc --test lexer)
add_test(NAME test_parser COMMAND pyc --test parser)
add_test(NAME test_ir COMMAND pyc --test ir)
add_test(NAME test_codegen COMMAND pyc --test codegen)
