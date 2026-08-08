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
#include <utility>

struct PyObject {
    int refcount;
    int type;   // 0=int, 1=list, 2=dict, 3=str, 4=float, 5=bool,
                // 6=cell (B5 nonlocal/closure), 10=exception, 11=function,
                // 12=exception class, 13=complex, 17=bytes, 18=bytearray,
                // 19=decimal, 20=set
    int64_t value;    // type 0 (int)
    double dvalue;    // type 4 (float)
    std::vector<PyObject*> list;
    // Insertion-ordered dict storage: a vector of (key, value) pairs with
    // linear-scan value-equality lookup (PyObject_CompareBool op==0).
    // Replaces the old std::unordered_map<PyObject*, PyObject*> which was
    // keyed by raw pointer (unused — the actual lookup was already a linear
    // value scan) and did not preserve insertion order. CPython 3.7+
    // guarantees insertion-order iteration; pyc now matches that.
    std::vector<std::pair<PyObject*, PyObject*>> dict;
    std::string str;
    PyObject* cell_content; // type 6
    // A4 homogeneous numeric lists
    int list_item_type;      // 0=general PyObject*, 1=int64, 2=double
    std::vector<int64_t> ilist;
    std::vector<double> flist;
    // Complex numbers (type 13)
    double complex_real;
    double complex_imag;
    // Set (type 20): insertion-ordered, dedup-by-value (linear scan with
    // PyObject_CompareBool, same approach as the dict container above).
    // Reuses the `list` vector for element storage to avoid a separate
    // field; set ops must never touch ilist/flist/list_item_type.
    std::vector<PyObject*> setElems;
};
