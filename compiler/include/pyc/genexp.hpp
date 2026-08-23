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

}  // namespace pyc
