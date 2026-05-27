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

target_link_libraries(pyc PRIVATE ${llvm_libs} Python3::Python)
target_include_directories(pyc PRIVATE include)

install(TARGETS pyc DESTINATION bin)
