#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <variant>

namespace pyc::ir {

// Types
enum class TypeKind { I32, I64, F64, PBYTE, PYOBJ, VOID, PYTYPE, STRUCT };

class Type {
    TypeKind kind_;
    struct { Type* elem; int size; } array_;    // for PYOBJ arrays
    struct { std::string name; std::vector<std::pair<std::string, Type*>> fields; } struct_;
public:
    explicit Type(TypeKind k = TypeKind::VOID) : kind_(k) {}
    Type(TypeKind k, Type* e, int s) : kind_(k) { array_ = {e, s}; }
    Type(TypeKind k, const std::string& n, std::vector<std::pair<std::string, Type*>> f)
        : kind_(k), struct_{n, std::move(f)} {}

    TypeKind kind() const { return kind_; }
    TypeKind elem_type() const { return array_.elem->kind(); }
    int array_size() const { return array_.size; }
    const std::string& struct_name() const { return struct_.name; }
    const auto& struct_fields() const { return struct_.fields; }

    static Type i32() { return Type(TypeKind::I32); }
    static Type i64() { return Type(TypeKind::I64); }
    static Type f64() { return Type(TypeKind::F64); }
    static Type pbyte() { return Type(TypeKind::PBYTE); }
    static Type pyobj() { return Type(TypeKind::PYOBJ); }
    static Type pytype() { return Type(TypeKind::PYTYPE); }
    static Type Void() { return Type(TypeKind::VOID); }
    static Type* make_int() { static Type t(TypeKind::I64); return &t; }
    static Type* make_float() { static Type t(TypeKind::F64); return &t; }
    static Type* make_pbyte() { static Type t(TypeKind::PBYTE); return &t; }
    static Type* make_pyobj() { static Type t(TypeKind::PYOBJ); return &t; }
    static Type* make_void() { static Type t(TypeKind::VOID); return &t; }
};

// Instructions
enum class IRInstKind { ALLOC, STORE, LOAD, PHI, LOADGLOBAL, STOREGLOBAL, LOADLOCAL, STORELOCAL,
    GETATTR, SETATTR, CALL, RETURN, BINOP, CMP, LOADCONST_INT, LOADCONST_FLOAT, LOADCONST_STR,
    NEWTYPE, MAKE_LIST, LIST_GET, LIST_SET, NEWOBJ, ISINSTANCE,
    ADD, SUB, MUL, DIV, MOD, POW, LT, LE, GT, GE, EQ, NE, AND, OR, NOT,
    JUMP, BRANCH, LABEL, INTRINSIC_PRINT, INTRINSIC_RANGE, INTRINSIC_TYPE, INTRINSIC_LEN,
    INTRINSIC_INIT, LOAD_ATTR };

struct IRInst {
    uint32_t id;
    IRInstKind kind;
    std::string name;           // for named values (phi, allocs)
    std::vector<uint32_t> operands; // indices of other instructions/constants
    std::variant<int, double, std::string> const_val;
    bool is_const = false;
    bool is_phi = false;

    IRInst(IRInstKind k, std::string n = "")
        : id(0), kind(k), name(std::move(n)) {}
};

// IR block
struct IRBlock {
    uint32_t id;
    std::string name;
    std::vector<std::unique_ptr<IRInst>> instrs;
    std::vector<uint32_t> successors;
    uint32_t phi_target = UINT32_MAX;  // which function/loop this is a phi target of

    static IRBlock label(const std::string& name) {
        static uint32_t next_id = 1;
        return {next_id++, name, {}, {}, UINT32_MAX};
    }
};

// Function
struct IRFunction {
    std::string name;
    std::vector<std::string> param_names;
    std::vector<IRBlock*> blocks;
    uint32_t entry_block_id;
    std::vector<IRInst*> pending_;

    IRInst* new_inst(IRInstKind kind, std::string name = "") {
        auto* inst = new IRInst(kind, std::move(name));
        inst->id = next_inst_id_++;
        pending_.push_back(inst);
        return inst;
    }

    IRBlock* new_block(const std::string& name = "") {
        auto* blk = new IRBlock{next_block_id_++, name, {}, {}};
        blocks.push_back(blk);
        return blk;
    }

    static uint32_t next_func_id;
    uint32_t next_inst_id_ = 1;
    uint32_t next_block_id_ = 1;
};

// Module
struct IRModule {
    std::unordered_map<std::string, IRFunction*> functions;
    std::unordered_map<std::string, IRFunction*> builtins;
    std::vector<IRFunction*> func_list;
    bool has_main = false;
};

} // namespace pyc::ir
