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
    // Name lookup in a CLASS BODY: the namespace under construction, then
    // globals, then builtins (CPython's LOAD_NAME). args[0] is the namespace
    // dict, text is the name.
    LoadClassName,
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
    // Pointer identity. NOT a C-API call: `is` is defined as object identity,
    // so it is a machine comparison and cannot raise. (Py_Is exists but has no
    // recorded ownership contract, so §4 would refuse it anyway.)
    Is,
    // Logical negation of a machine int, for `not in` and `is not`.
    IntNot,
    // `del name` at module scope; `text` is the name (a C string).
    DelGlobal,
    // A machine integer (not a PyObject), for C-API integer parameters.
    IntConst,
    // A null PyObject*. Not a Python value: it is the "no pending exception"
    // and "no pending return" marker that try/finally's dispatch tests, and
    // the one thing a phi over those paths needs that no Python constant can
    // express. Never incref'd or decref'd -- its ownership is AlwaysNull.
    ConstNull,
    // A complex literal. `text` is "<real> <imag>"; both are raw doubles,
    // so this cannot go through the generic PyObject* call path.
    ConstComplex,
    // Unpack a value into exactly `imm` items, yielding a tuple.
    Unpack,
    // Raise an exception; always transfers to the error edge.
    Raise,
    // Build a class from a populated namespace dict. `text` is the class
    // name; args are (bases_tuple, namespace).
    BuildClass,
    // Import a module by name. A dedicated op because the C-API entry points
    // take a C STRING, not a PyObject*, so they do not fit the generic call
    // path. `imm` selects which: 0 imports the named module itself (the leaf
    // of a dotted path), 1 imports it and yields the TOP-LEVEL package, which
    // is what plain `import a.b` binds.
    ImportModule,
    // Build a generator expression (rebuild/GENERATORS.md). `text` is the
    // marshalled code object CPython produced at build time; args are the
    // closure tuple of cells and the eagerly-evaluated outer ITERATOR.
    // pyc does not implement suspension: the linked interpreter runs the body.
    MakeGenexp,
    // Cell access. A cell variable's slot holds a PyCell, so reading it is
    // two steps, not one.
    CellNew, CellGet, CellSet,
    // Advance an iterator. THREE-way: PyIter_Next returns NULL both at
    // exhaustion (no exception) and on error (exception set), so the two are
    // distinguished by PyErr_Occurred. `target` is the body, `target_else`
    // the exhausted path, `on_error` the landing pad.
    IterNext,
    // SSA merge. `args` are the incoming values and `phi_blocks` the matching
    // predecessor block indices, positionally. Real phis rather than allocas
    // because §3 specifies typed SSA and lowering knows its predecessors
    // exactly -- it created them.
    Phi,
    // Build a callable from a lowered function and bind it in the enclosing
    // scope. Codegen turns this into a PyCFunction over the emitted C entry.
    MakeFunction,
};

const char* op_name(Op op);

// THE definition of a terminator. Lowering uses it to drop unreachable
// instructions, codegen to decide whether a block needs an `unreachable`.
// Keeping two copies has now cost four false results (ret, phi, iter.next,
// raise), so there is exactly one.
//
// check_ir_wellformed.py still carries its own regex, because it reads printed
// text rather than linking this header. That copy is documented there.
inline bool is_terminator(Op op) {
    return op == Op::Br || op == Op::CondBr || op == Op::Return
        || op == Op::ReturnErr || op == Op::IterNext || op == Op::Raise;
}

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
    // Phi only: predecessor block per incoming value, positional with args.
    std::vector<std::uint32_t> phi_blocks;

    // Operand value ids this call STEALS. A stolen reference is released by
    // the callee, so no decref appears for it -- which reads as a leak to
    // anything auditing the IR. Recorded from §4's table so the dump is
    // self-describing and the checker needs no second copy of the steal list.
    std::vector<std::uint32_t> stolen;

    std::int64_t imm = 0;
    // Whether `imm` is meaningful. Without this, an immediate of 0 -- Py_LT,
    // or index 0 of a container -- prints as absent, so `a < b` and a call
    // with no immediate look identical in the IR dump.
    bool has_imm = false;
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

    // Locals held in CELLS because a nested function reads them, and names
    // read FROM an enclosing function's cells. Both occupy ordinary local
    // slots; the difference is that the slot holds a cell rather than a value.
    // Placed last so existing aggregate initialisers keep their meaning.
    std::vector<std::string> cellvars;
    std::vector<std::string> freevars;

    Value fresh(Type t = {}) { return Value{next_value++, std::move(t)}; }
};

struct Module {
    std::string source_file;
    std::vector<Function> functions;   // functions[0] is the module body

};

std::string to_string(const Module& m);

}  // namespace pyc::ir
