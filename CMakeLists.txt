cmake_minimum_required(VERSION 3.20)
project(pyc LANGUAGES CXX C)

find_package(LLVM REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Development Interpreter)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_COMPILER clang++)

include_directories(${LLVM_INCLUDE_DIRS} ${Python3_INCLUDE_DIRS} include)
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

llvm_map_components_to_libnames(llvm_libs 
    core support irreader analysis executionengine native 
    bitwriter codegen mcjit orcjit target x86codegen x86asmparser
    scalaropts instcombine aggressiveinstcombine vectorize transformutils passes
)

add_executable(pyc 
    src/main.cpp
    src/frontend/PythonParser.cpp
    src/Compiler.cpp
    src/ir/IR.cpp
    src/codegen/Codegen.cpp
)

target_link_libraries(pyc PRIVATE ${llvm_libs} Python3::Python pyc_runtime)
target_include_directories(pyc PRIVATE include)

install(TARGETS pyc DESTINATION bin)

add_library(pyc_runtime STATIC src/runtime/Runtime.cpp)
set_target_properties(pyc_runtime PROPERTIES OUTPUT_NAME "pycrt")
install(TARGETS pyc_runtime DESTINATION lib)
install(FILES include/pyc/runtime.h DESTINATION include/pyc)

# Testing support
enable_testing()

# Convenience target: make check (or make pyc-check)
add_custom_target(check
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runner.py
  DEPENDS pyc
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running pyc test suite"
  USES_TERMINAL
)

# Also register with CTest so "make test" / ctest works
add_test(
  NAME pyc_basic_tests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runner.py
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
