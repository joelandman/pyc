#pragma once
#include <memory>
#include "ir/ir.h"
#include <functional>
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>

namespace pyc::ir {

// Opaque type for all Python object values - the interpreter works with these
// In a real implementation, this would be PyObject*; here we use a variant of
// primitive types for simplicity (int64, double, string)
using PyValue = std::variant<std::monostate, int64_t, double, std::string>;

class RuntimeError {
public:
    std::string msg;
    int line = -1;
    RuntimeError(std::string m, int l = -1) : msg(std::move(m)), line(l) {}
};

// A call frame: one activation of a function
struct CallFrame {
    IRFunction* func;
    uint32_t block_idx;   // which block we are in
    uint32_t inst_idx;    // index into block->instrs
    std::unordered_map<std::string, uint64_t> name_to_slot;

    // We store actual values into a slots vector
    std::vector<PyValue> slots;
    uint64_t slot_count = 0;

    uint64_t alloc_slot() {
        auto n = slot_count++;
        slots.resize(n + 1);
        return n;
    }

    CallFrame(IRFunction* f) : func(f), block_idx(0), inst_idx(0) {
        // Allocate slots for function parameters
        for (auto& name : f->param_names) {
            auto s = alloc_slot();
            name_to_slot[name] = s;
        }
    }
};

class Interpreter {
public:
    Interpreter();

    // Entry point: run the module starting from 'main' function
    PyValue run(const std::shared_ptr<IRModule>& module);

    // Call a specific function by name with given args
    PyValue call_function(const std::string& name, const std::vector<PyValue>& args);

    // Set/get global variables
    void set_global(const std::string& name, PyValue value);
    std::optional<PyValue> get_global(const std::string& name);
    std::unordered_map<std::string, PyValue>& globals();

    // Set up builtin function
    void register_builtin(const std::string& name, std::function<PyValue(std::vector<PyValue>)> fn);

private:
    std::shared_ptr<IRModule> module_;
    std::vector<std::unique_ptr<CallFrame>> frame_stack_;
    std::unordered_map<std::string, std::function<PyValue(std::vector<PyValue>)>> builtins_;
    std::unordered_map<std::string, PyValue> global_vars_;
    PyValue last_result_;

    // Internal helpers
    void push_frame(std::unique_ptr<CallFrame> frame);
    std::unique_ptr<CallFrame>& current_frame();
    std::unique_ptr<CallFrame>& top_frame();
    CallFrame* current_frame_ptr() const;

    // Execute one instruction, returns control signal (-1=return, -2=jump, -3=branch)
    PyValue execute_instruction(std::unique_ptr<CallFrame>& frame, IRInst& inst,
                                 uint32_t& block_idx, uint32_t& inst_idx);

    // Resolve a value from a slot or constant
    PyValue resolve_value(CallFrame* frame, uint32_t val_ref, const IRFunction& func);
    PyValue resolve_literal(const std::variant<int, double, std::string>& lit);

    // Helper: downcast variant
    int64_t as_int(const PyValue& v);
    double as_float(const PyValue& v);
    const std::string& as_string(const PyValue& v);

    // Helper: check if variant is int-like
    bool is_int(const PyValue& v);
    bool is_float(const PyValue& v);

    // Core instruction implementations
    PyValue handle_load_const(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_load_local(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_store_local(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_load_global(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_store_global(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_getattr(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_setattr(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_call(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_return(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_add(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_sub(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_mul(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_div(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_mod(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_pow(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_lt(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_le(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_gt(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_ge(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_eq(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_ne(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_and(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_or(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_not(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_jump(std::unique_ptr<CallFrame>& frame, IRInst& inst,
                         uint32_t& block_idx, uint32_t& inst_idx);
    PyValue handle_branch(std::unique_ptr<CallFrame>& frame, IRInst& inst,
                           uint32_t& block_idx, uint32_t& inst_idx);
    PyValue handle_phi(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_alloc(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_store(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_load(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_binop(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_cmp_op(std::unique_ptr<CallFrame>& frame, IRInst& inst);

    // Intrinsic handlers
    PyValue handle_intrinsic_print(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_intrinsic_range(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_intrinsic_type(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_intrinsic_len(std::unique_ptr<CallFrame>& frame, IRInst& inst);
    PyValue handle_intrinsic_init(std::unique_ptr<CallFrame>& frame, IRInst& inst);

    // Helper: build value from slots
    PyValue get_slot(CallFrame* frame, uint64_t slot);
    void set_slot(CallFrame* frame, uint64_t slot, PyValue value);

    // Helper: print helper
    static std::string fmt_value(const PyValue& v);

    // Internal call implementation
    PyValue call_function_impl(const std::string& name, const std::vector<PyValue>& args);
};

// Utility: check if any value in a pair is int/float
inline bool is_numeric(const PyValue& v) {
    return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v);
}

inline PyValue wrap_numeric(PyValue a, PyValue b, auto op) {
    if (std::holds_alternative<int64_t>(a) && std::holds_alternative<int64_t>(b)) {
        return PyValue(int64_t(op(std::get<int64_t>(a), std::get<int64_t>(b))));
    }
    double da = std::holds_alternative<int64_t>(a) ? std::get<int64_t>(a) : std::get<double>(a);
    double db = std::holds_alternative<int64_t>(b) ? std::get<int64_t>(b) : std::get<double>(b);
    return PyValue(op(da, db));
}

} // namespace pyc::ir
