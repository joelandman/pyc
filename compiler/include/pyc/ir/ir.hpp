#pragma once
// Typed SSA IR (INTERFACES.md §3).
//
// Refcount operations are EXPLICIT instructions, not an implicit convention.
// That is the whole point: §4 records each C-API symbol's ownership contract,
// and making IncRef/DecRef first-class lets a verifier prove balance instead
// of trusting that lowering remembered. The old tree's leaks came from exactly
// the opposite arrangement.
#include "pyc/diagnostics.hpp"
#include "pyc/rt/capi.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pyc::ir {

using rt::Ownership;

// The lattice. Top is a boxed PyObject* of unknown type; everything below is a
// refinement A3 has PROVED. Codegen may assume nothing lowering did not prove.
//
// Only Boxed is produced today: the first lowering is deliberately all-C-API,
// because I2 means the boxed path must stay correct underneath every future
// unboxing. Optimising a correct baseline is safe; the reverse is not.
struct Type {
    enum class Kind { Top, Boxed, Int64, Float64, Bool, ExactType, Bottom };
    Kind kind = Kind::Boxed;
    std::string exact_type;          // valid iff kind == ExactType
    bool is_boxed() const { return kind == Kind::Top || kind == Kind::Boxed
                                || kind == Kind::ExactType; }
};

struct Value {
    std::uint32_t id = 0;
    Type type;
    bool valid() const { return id != 0; }   // 0 is the null value
};

enum class Op {
    // constants -- ConstInt carries DECIMAL TEXT, never an int64: materialising
    // a literal into a machine word is how the old tree wrapped factorial(25).
    ConstInt, ConstFloat, ConstStr, ConstBytes, ConstBool, ConstNone,
    // names
    LoadGlobal, StoreGlobal, LoadLocal, StoreLocal,
    // calls
    CallCApi,        // symbol from §4's table, with its ownership contract
    CallObject,      // PyObject_Vectorcall on a runtime callable
    // refcounting, explicit and verifiable
    IncRef, DecRef,
    // control flow
    Br, CondBr, Return,
    // Return failure with a Python exception already set. The C-API
    // convention (INTERFACES §3), not setjmp/longjmp: the old tree's
    // jump-based frames are a documented frame-leak source.
    ReturnErr,
    // truthiness for a branch predicate: PyObject_IsTrue, -1 on error
    IsTrue,
    // Build a callable from a lowered function and bind it in the enclosing
    // scope. Codegen turns this into a PyCFunction over the emitted C entry.
    MakeFunction,
};

const char* op_name(Op op);

struct Instr {
    Op op = Op::ConstNone;
    std::vector<Value> args;
    std::optional<Value> result;
    Ownership result_ownership = Ownership::NotAnObject;

    std::string text;                // literal text / name / C-API symbol
    std::uint32_t target = 0;        // Br / CondBr: block index
    std::uint32_t target_else = 0;   // CondBr
    SourceLoc loc;

    // Set when the instruction can fail. INTERFACES §3: a call that may raise
    // MUST have somewhere to go when it does, so codegen can always emit the
    // check. Absent here means "provably cannot fail".
    std::optional<std::uint32_t> on_error;

    // An integer operand that is NOT a Python object: a container size, a
    // slot index. Kept separate from args so codegen never has to guess which
    // operands are PyObject* and which are machine integers. Last in the
    // struct so existing aggregate initialisers keep their meaning.
    std::int64_t imm = 0;
};

struct Block {
    std::string label;
    std::vector<Instr> instrs;
};

struct Function {
    std::string name;
    std::vector<std::string> params;
    // Fast locals, CPython-style: a name assigned anywhere in the body is
    // local throughout, so the slot set is fixed before lowering begins.
    std::vector<std::string> locals;
    std::vector<Block> blocks;
    std::uint32_t next_value = 1;    // 0 reserved for "no value"

    Value fresh(Type t = {}) { return Value{next_value++, std::move(t)}; }
};

struct Module {
    std::string source_file;
    std::vector<Function> functions;   // functions[0] is the module body

};

std::string to_string(const Module& m);

}  // namespace pyc::ir
