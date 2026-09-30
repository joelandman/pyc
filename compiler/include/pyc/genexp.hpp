#pragma once

// Generator expressions: code objects CPython compiled at BUILD time.
//
// See rebuild/GENERATORS.md. pyc does not implement suspension. Each generator
// expression arrives from the parse stage as a marshalled code object plus the
// free variables it captures, keyed by source position, and lowering hands
// those to the interpreter the binary already links.
//
// The table is separate from the AST on purpose: the typed AST is generated
// from CPython's own ASDL and stays faithful to it, so a synthetic field has
// no place in it.

#include <cstdint>
#include <string>
#include <vector>

namespace pyc {

struct GenexpEntry {
    int line = 0;
    int col = 0;
    std::string code;                    // marshalled code object, raw bytes
    std::vector<std::string> freevars;   // in co_freevars order
};

// One ordinary function's code object, compiled by the target interpreter.
// The native body still runs; this object is what `func.__code__` and the
// frame's f_code must be. `locals` is localsplus order (varnames, then cell
// names that are not locals, then free vars).
struct FuncCodeEntry {
    std::string qual;
    int line = 0;
    int end_col = 0;
    std::string code;
    std::vector<std::string> locals;
    std::vector<std::string> freevars;
};

}  // namespace pyc
