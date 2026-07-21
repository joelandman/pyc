#pragma once

// Full PyObject layout shared between Runtime.cpp and Codegen.cpp.
// This header ensures both see the identical struct definition so LLVM
// IR can safely inline runtime functions that touch PyObject fields.
//
// This file is compiled with C++ (Runtime.cpp) and included from C++
// (Codegen.cpp) via a C++ helper that creates an llvm::StructType
// from its fields. The LLVM IR itself only accesses fields 0-3
// (refcount, type, value, dvalue); fields 4+ are C++ stdlib objects
// managed entirely in the runtime.

#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

struct PyObject {
    int refcount;
    int type;   // 0=int, 1=list, 2=dict, 3=str, 4=float, 5=bool,
                // 6=cell (B5 nonlocal/closure), 10=exception, 11=function,
                // 12=exception class, 13=complex
    int64_t value;    // type 0 (int)
    double dvalue;    // type 4 (float)
    std::vector<PyObject*> list;
    std::unordered_map<PyObject*, PyObject*> dict;
    std::string str;
    PyObject* cell_content; // type 6
    // A4 homogeneous numeric lists
    int list_item_type;      // 0=general PyObject*, 1=int64, 2=double
    std::vector<int64_t> ilist;
    std::vector<double> flist;
    // Complex numbers (type 13)
    double complex_real;
    double complex_imag;
};
