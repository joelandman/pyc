// ir/interpreter.cpp - Python IR-based bytecode interpreter
// Executes IR directly (without LLVM) using a frame stack and value slots

#include "ir/interpreter.h"
#include <stdexcept>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstdlib>

namespace pyc::ir {

// ===== Static builtins =====

static PyValue builtin_print_impl(std::vector<PyValue> args) {
    for (const auto& v : args) {
        if (std::holds_alternative<int64_t>(v))
            std::cout << std::get<int64_t>(v);
        else if (std::holds_alternative<double>(v))
            std::cout << std::get<double>(v);
        else if (std::holds_alternative<std::string>(v))
            std::cout << std::get<std::string>(v);
        std::cout << " ";
    }
    std::cout << std::endl;
    return PyValue(int64_t(0));
}

// ===== Interpreter Constructor =====

Interpreter::Interpreter() : last_result_(PyValue(int64_t(0))) {}

// ===== Helper: downcast variant =====

int64_t Interpreter::as_int(const PyValue& v) {
    if (auto* p = std::get_if<int64_t>(&v)) return *p;
    if (auto* p = std::get_if<double>(&v)) return (int64_t)(*p);
    throw RuntimeError("Cannot convert to int: " + fmt_value(v));
}

double Interpreter::as_float(const PyValue& v) {
    if (auto* p = std::get_if<double>(&v)) return *p;
    if (auto* p = std::get_if<int64_t>(&v)) return (double)(*p);
    throw RuntimeError("Cannot convert to float: " + fmt_value(v));
}

const std::string& Interpreter::as_string(const PyValue& v) {
    static std::string empty_str;
    if (auto* p = std::get_if<std::string>(&v)) return *p;
    throw RuntimeError("Cannot convert to string: " + fmt_value(v));
}

bool Interpreter::is_int(const PyValue& v) {
    return std::holds_alternative<int64_t>(v);
}

bool Interpreter::is_float(const PyValue& v) {
    return std::holds_alternative<double>(v);
}

std::string Interpreter::fmt_value(const PyValue& v) {
    if (std::holds_alternative<int64_t>(v))
        return std::to_string(std::get<int64_t>(v));
    if (std::holds_alternative<double>(v))
        return std::to_string(std::get<double>(v));
    if (auto* s = std::get_if<std::string>(&v))
        return '"' + *s + '"';
    return "None";
}

// ===== Slot helpers =====

PyValue Interpreter::get_slot(CallFrame* frame, uint64_t slot) {
    if (slot < frame->slots.size())
        return frame->slots[slot];
    return PyValue(int64_t(0));
}

void Interpreter::set_slot(CallFrame* frame, uint64_t slot, PyValue value) {
    if (slot >= frame->slots.size())
        frame->slots.resize(slot + 1);
    frame->slots[slot] = value;
}

// ===== Frame management =====

void Interpreter::push_frame(std::unique_ptr<CallFrame> frame) {
    frame_stack_.push_back(std::move(frame));
}

std::unique_ptr<CallFrame>& Interpreter::current_frame() {
    if (frame_stack_.empty())
        throw RuntimeError("No current frame");
    return frame_stack_.back();
}

std::unique_ptr<CallFrame>& Interpreter::top_frame() {
    return current_frame();
}

CallFrame* Interpreter::current_frame_ptr() const {
    if (frame_stack_.empty())
        throw RuntimeError("No current frame");
    return frame_stack_.back().get();
}

// ===== Resolve value from slot or literal =====

PyValue Interpreter::resolve_value(CallFrame* frame, uint32_t val_ref, const IRFunction& func) {
    // First check name_to_slot cache for direct slot access
    for (auto& [name, idx] : frame->name_to_slot) {
        if (static_cast<uint32_t>(idx) == val_ref)
            return get_slot(frame, idx);
    }

    // Check if val_ref is a constant in any instruction
    for (auto* blk : func.blocks) {
        for (auto& instr : blk->instrs) {
            if (instr->id == val_ref && instr->is_const) {
                return resolve_literal(instr->const_val);
            }
        }
    }

    // Otherwise, look up the instruction result in the slot map
    for (auto& [name, idx] : frame->name_to_slot) {
        if (static_cast<uint32_t>(idx) == val_ref)
            return get_slot(frame, idx);
    }

    return PyValue(int64_t(0));  // Default fallback
}

PyValue Interpreter::resolve_literal(const std::variant<int, double, std::string>& lit) {
    return std::visit([](auto&& val) -> PyValue {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, int>) return PyValue((int64_t)val);
        else if constexpr (std::is_same_v<T, double>) return PyValue(val);
        else return PyValue(val);
    }, lit);
}

// ===== Core execution =====

PyValue Interpreter::run(const std::shared_ptr<IRModule>& module) {
    module_ = module;

    // Find and execute the 'main' function, or first function
    std::string entry_name = "main";
    auto mit = module_->functions.find(entry_name);
    if (mit == module_->functions.end()) {
        if (!module_->func_list.empty()) {
            entry_name = module_->func_list[0]->name;
        } else {
            throw RuntimeError("No functions to execute");
        }
    }

    auto fn = module_->functions[entry_name];
    if (!fn) {
        throw RuntimeError("Function '" + entry_name + "' not found");
    }

    // Execute the function
    auto result = call_function_impl(entry_name, {});
    last_result_ = result;
    return result;
}

PyValue Interpreter::call_function(const std::string& name, const std::vector<PyValue>& args) {
    return call_function_impl(name, args);
}

PyValue Interpreter::call_function_impl(const std::string& name, const std::vector<PyValue>& args_copy) {
    auto it = module_->functions.find(name);
    if (it == module_->functions.end()) {
        // Check if it's a builtin
        auto bit = builtins_.find(name);
        if (bit != builtins_.end()) {
            auto result = bit->second(args_copy);
            return result;
        }
        throw RuntimeError("Function '" + name + "' not found");
    }

    auto* fn = it->second;

    // Create call frame using unique_ptr for proper ownership
    auto frame = std::make_unique<CallFrame>(fn);

    // Set up parameters
    for (size_t i = 0; i < std::min(fn->param_names.size(), args_copy.size()); ++i) {
        auto slot_it = frame->name_to_slot.find(fn->param_names[i]);
        if (slot_it != frame->name_to_slot.end()) {
            set_slot(frame.get(), slot_it->second, args_copy[i]);
        }
    }

    push_frame(std::move(frame));

    // Set current context for intrinsics
    current_func_ = fn;
    current_block_ = nullptr;

    // Execute blocks with proper control flow tracking
    uint32_t current_block_idx = 0;
    uint32_t current_inst_idx = 0;
    bool should_return = false;

    while (current_block_idx < frame_stack_.back()->func->blocks.size() && !should_return) {
        auto& frame_ref = frame_stack_.back();
        auto* fn = frame_ref->func;
        
        if (current_block_idx >= fn->blocks.size()) break;
        
        auto* block = fn->blocks[current_block_idx];
        current_block_ = block;
        if (!block) {
            current_block_idx++;
            current_inst_idx = 0;
            continue;
        }

        // Execute instructions in this block
        while (current_inst_idx < block->instrs.size() && !should_return) {
            auto* inst = block->instrs[current_inst_idx].get();
            if (!inst) {
                current_inst_idx++;
                continue;
            }

            auto result = execute_instruction(frame_ref, *inst, current_block_idx, current_inst_idx);
            
            // Check if instruction returned a control flow signal
            if (std::holds_alternative<int64_t>(result)) {
                int64_t control = std::get<int64_t>(result);
                if (control == -1) {
                    // Return signal
                    should_return = true;
                } else if (control == -2) {
                    // Jump signal - target stored in value
                    current_block_idx = static_cast<uint32_t>(std::get<int64_t>(last_result_));
                    current_inst_idx = 0;
                    continue;
                } else if (control == -3) {
                    // Branch signal - true/false targets in result
                    // The actual value was stored in last_result_
                    current_inst_idx++;
                    continue;
                }
            }
            
            current_inst_idx++;
        }

        // Move to next block (fallthrough)
        current_block_idx++;
        current_inst_idx = 0;
    }

    // Pop frame
    if (!frame_stack_.empty()) {
        frame_stack_.pop_back();
    }

    return last_result_;
}

// ===== Execute one instruction =====

PyValue Interpreter::execute_instruction(std::unique_ptr<CallFrame>& frame, IRInst& inst,
                                           uint32_t& block_idx, uint32_t& inst_idx) {
    auto kind = inst.kind;
    PyValue result;

    switch (kind) {
        case IRInstKind::LOADCONST_INT:
        case IRInstKind::LOADCONST_FLOAT:
        case IRInstKind::LOADCONST_STR:
            result = handle_load_const(frame, inst);
            break;

        case IRInstKind::LOADLOCAL:
            result = handle_load_local(frame, inst);
            break;

        case IRInstKind::STORELOCAL:
            result = handle_store_local(frame, inst);
            break;

        case IRInstKind::LOADGLOBAL:
            result = handle_load_global(frame, inst);
            break;

        case IRInstKind::STOREGLOBAL:
            result = handle_store_global(frame, inst);
            break;

        case IRInstKind::GETATTR:
            result = handle_getattr(frame, inst);
            break;

        case IRInstKind::SETATTR:
            result = handle_setattr(frame, inst);
            break;

        case IRInstKind::CALL:
            result = handle_call(frame, inst);
            break;

        case IRInstKind::RETURN:
            result = handle_return(frame, inst);
            break;

        case IRInstKind::ADD:
            result = handle_add(frame, inst);
            break;

        case IRInstKind::SUB:
            result = handle_sub(frame, inst);
            break;

        case IRInstKind::MUL:
            result = handle_mul(frame, inst);
            break;

        case IRInstKind::DIV:
            result = handle_div(frame, inst);
            break;

        case IRInstKind::MOD:
            result = handle_mod(frame, inst);
            break;

        case IRInstKind::POW:
            result = handle_pow(frame, inst);
            break;

        case IRInstKind::LT:
            result = handle_lt(frame, inst);
            break;

        case IRInstKind::LE:
            result = handle_le(frame, inst);
            break;

        case IRInstKind::GT:
            result = handle_gt(frame, inst);
            break;

        case IRInstKind::GE:
            result = handle_ge(frame, inst);
            break;

        case IRInstKind::EQ:
            result = handle_eq(frame, inst);
            break;

        case IRInstKind::NE:
            result = handle_ne(frame, inst);
            break;

        case IRInstKind::AND:
            result = handle_and(frame, inst);
            break;

        case IRInstKind::OR:
            result = handle_or(frame, inst);
            break;

        case IRInstKind::NOT:
            result = handle_not(frame, inst);
            break;

        case IRInstKind::JUMP:
            result = handle_jump(frame, inst, block_idx, inst_idx);
            break;

        case IRInstKind::BRANCH:
            result = handle_branch(frame, inst, block_idx, inst_idx);
            break;

        case IRInstKind::PHI:
            result = handle_phi(frame, inst);
            break;

        case IRInstKind::ALLOC:
            result = handle_alloc(frame, inst);
            break;

        case IRInstKind::STORE:
            result = handle_store(frame, inst);
            break;

        case IRInstKind::LOAD:
            result = handle_load(frame, inst);
            break;

        case IRInstKind::BINOP:
            result = handle_binop(frame, inst);
            break;

        case IRInstKind::CMP:
            result = handle_cmp_op(frame, inst);
            break;

        case IRInstKind::INTRINSIC_PRINT:
            result = handle_intrinsic_print(frame, inst);
            break;

        case IRInstKind::INTRINSIC_RANGE:
            result = handle_intrinsic_range(frame, inst);
            break;

        case IRInstKind::INTRINSIC_TYPE:
            result = handle_intrinsic_type(frame, inst);
            break;

        case IRInstKind::INTRINSIC_LEN:
            result = handle_intrinsic_len(frame, inst);
            break;

        case IRInstKind::INTRINSIC_INIT:
            result = handle_intrinsic_init(frame, inst);
            break;

        default:
            // Unknown instruction - treat as no-op
            result = PyValue(int64_t(0));
            break;
    }
    
    // Cache instruction result for O(1) lookup
    frame->cache_result(inst.id, result);
    
    return result;
}

// ===== Instruction handlers =====

PyValue Interpreter::handle_load_const(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.is_const) {
        return resolve_literal(inst.const_val);
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_load_local(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    // Try to find the slot for this variable
    auto slot_it = frame->name_to_slot.find(inst.name);
    if (slot_it != frame->name_to_slot.end()) {
        return get_slot(frame.get(), slot_it->second);
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_store_local(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() >= 2) {
        uint64_t slot = inst.operands[0];
        PyValue value = get_slot(frame.get(), inst.operands[1]);

        auto slot_it = frame->name_to_slot.find(inst.name);
        if (slot_it != frame->name_to_slot.end()) {
            set_slot(frame.get(), slot_it->second, value);
        } else {
            auto new_slot = frame->alloc_slot();
            frame->name_to_slot[inst.name] = new_slot;
            set_slot(frame.get(), new_slot, value);
        }
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_load_global(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    auto it = global_vars_.find(inst.name);
    if (it != global_vars_.end()) {
        return it->second;
    }
    // Also check builtins
    auto bit = builtins_.find(inst.name);
    if (bit != builtins_.end()) {
        return PyValue(int64_t(0));
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_store_global(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() >= 1) {
        auto val = get_slot(frame.get(), inst.operands[0]);
        global_vars_[inst.name] = val;
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_getattr(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.empty()) return PyValue(int64_t(0));
    auto obj_val = get_slot(frame.get(), inst.operands[0]);
    std::string attr_name = inst.name;
    
    std::string key = "instance_attr_" + std::to_string(inst.operands[0]) + "_" + attr_name;
    auto it = global_vars_.find(key);
    if (it != global_vars_.end()) {
        return it->second;
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_setattr(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    std::string attr_name = inst.name;
    std::string key = "instance_attr_" + std::to_string(inst.operands[0]) + "_" + attr_name;
    global_vars_[key] = get_slot(frame.get(), inst.operands[1]);
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_call(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.empty()) {
        return PyValue(int64_t(0));
    }

    // Get function name
    auto fn_name = inst.name;

    // Get arguments
    std::vector<PyValue> call_args;
    for (size_t i = 1; i < inst.operands.size(); ++i) {
        auto val = get_slot(frame.get(), inst.operands[i]);
        call_args.push_back(val);
    }

    // Call the function
    return call_function_impl(fn_name, call_args);
}

PyValue Interpreter::handle_return(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.empty()) {
        last_result_ = PyValue(int64_t(0));
    } else {
        uint64_t slot = inst.operands[0];
        last_result_ = get_slot(frame.get(), slot);
    }

    // Signal return with special value
    return PyValue(int64_t(-1));
}

PyValue Interpreter::handle_add(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) { return a + b; });
}

PyValue Interpreter::handle_sub(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) { return a - b; });
}

PyValue Interpreter::handle_mul(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) { return a * b; });
}

PyValue Interpreter::handle_div(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    if (auto* p = std::get_if<double>(&rhs)) {
        if (*p == 0.0) throw RuntimeError("Division by zero");
    }
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) { return a / b; });
}

PyValue Interpreter::handle_mod(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) { return (int64_t)a % (int64_t)b; });
}

PyValue Interpreter::handle_pow(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    double base = 0.0, exp = 0.0;
    if (auto* p = std::get_if<int64_t>(&lhs)) base = static_cast<double>(*p);
    else if (auto* p = std::get_if<double>(&lhs)) base = *p;
    if (auto* p = std::get_if<int64_t>(&rhs)) exp = static_cast<double>(*p);
    else if (auto* p = std::get_if<double>(&rhs)) exp = *p;
    return PyValue(std::pow(base, exp));
}

PyValue Interpreter::handle_lt(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) -> int64_t { return a < b ? 1 : 0; });
}

PyValue Interpreter::handle_le(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) -> int64_t { return a <= b ? 1 : 0; });
}

PyValue Interpreter::handle_gt(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) -> int64_t { return a > b ? 1 : 0; });
}

PyValue Interpreter::handle_ge(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    return wrap_numeric(lhs, rhs, [](auto&& a, auto&& b) -> int64_t { return a >= b ? 1 : 0; });
}

PyValue Interpreter::handle_eq(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);

    if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<int64_t>(rhs)) {
        return PyValue((std::get<int64_t>(lhs) == std::get<int64_t>(rhs)) ? int64_t(1) : int64_t(0));
    }
    if (std::holds_alternative<double>(lhs) && std::holds_alternative<double>(rhs)) {
        return PyValue((std::get<double>(lhs) == std::get<double>(rhs)) ? int64_t(1) : int64_t(0));
    }
    if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
        return PyValue((std::get<std::string>(lhs) == std::get<std::string>(rhs)) ? int64_t(1) : int64_t(0));
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_ne(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);

    if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<int64_t>(rhs)) {
        return PyValue((std::get<int64_t>(lhs) != std::get<int64_t>(rhs)) ? int64_t(1) : int64_t(0));
    }
    if (std::holds_alternative<double>(lhs) && std::holds_alternative<double>(rhs)) {
        return PyValue((std::get<double>(lhs) != std::get<double>(rhs)) ? int64_t(1) : int64_t(0));
    }
    if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
        return PyValue((std::get<std::string>(lhs) != std::get<std::string>(rhs)) ? int64_t(1) : int64_t(0));
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_and(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    bool lv = std::holds_alternative<int64_t>(lhs) ? std::get<int64_t>(lhs) : false;
    bool rv = std::holds_alternative<int64_t>(rhs) ? std::get<int64_t>(rhs) : false;
    return PyValue(int64_t(lv && rv ? 1 : 0));
}

PyValue Interpreter::handle_or(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() < 2) return PyValue(int64_t(0));
    auto lhs = get_slot(frame.get(), inst.operands[0]);
    auto rhs = get_slot(frame.get(), inst.operands[1]);
    bool lv = std::holds_alternative<int64_t>(lhs) ? std::get<int64_t>(lhs) : false;
    bool rv = std::holds_alternative<int64_t>(rhs) ? std::get<int64_t>(rhs) : false;
    return PyValue(int64_t(lv || rv ? 1 : 0));
}

PyValue Interpreter::handle_not(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.empty()) return PyValue(int64_t(1));
    auto val = get_slot(frame.get(), inst.operands[0]);
    bool v = std::holds_alternative<int64_t>(val) ? std::get<int64_t>(val) : false;
    return PyValue(int64_t(v ? 0 : 1));
}

PyValue Interpreter::handle_jump(std::unique_ptr<CallFrame>& frame, IRInst& inst,
                                  uint32_t& block_idx, uint32_t& inst_idx) {
    (void)frame;
    if (inst.operands.empty()) return PyValue(int64_t(0));
    
    // Jump to target block
    uint32_t target = inst.operands[0];
    block_idx = target;
    inst_idx = 0;
    last_result_ = PyValue(int64_t(target));
    return PyValue(int64_t(-2));  // Signal: jump
}

PyValue Interpreter::handle_branch(std::unique_ptr<CallFrame>& frame, IRInst& inst,
                                    uint32_t& block_idx, uint32_t& inst_idx) {
    if (inst.operands.size() < 3) return PyValue(int64_t(0));
    auto cond = get_slot(frame.get(), inst.operands[0]);
    uint32_t true_blk = inst.operands[1];
    uint32_t false_blk = inst.operands[2];

    bool cond_val = false;
    if (auto* p = std::get_if<int64_t>(&cond)) {
        cond_val = *p != 0;
    } else if (auto* p = std::get_if<double>(&cond)) {
        cond_val = *p != 0.0;
    }

    if (cond_val) {
        block_idx = true_blk;
    } else {
        block_idx = false_blk;
    }
    inst_idx = 0;
    last_result_ = PyValue(int64_t(block_idx));
    return PyValue(int64_t(-2));  // Signal: branch taken
}

PyValue Interpreter::handle_phi(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.empty()) return PyValue(int64_t(0));
    auto val = get_slot(frame.get(), inst.operands[0]);
    return val;
}

PyValue Interpreter::handle_alloc(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    auto slot = frame->alloc_slot();
    frame->name_to_slot[inst.name] = slot;
    return PyValue(int64_t(slot));
}

PyValue Interpreter::handle_store(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() >= 2) {
        uint64_t slot_idx = inst.operands[0];
        uint64_t val_idx = inst.operands[1];
        PyValue val = get_slot(frame.get(), val_idx);
        set_slot(frame.get(), slot_idx, val);
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_load(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.size() >= 1) {
        uint64_t slot_idx = inst.operands[0];
        return get_slot(frame.get(), slot_idx);
    }
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_binop(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    return PyValue(int64_t(0));  // stub - dispatch to specific op handlers
}

PyValue Interpreter::handle_cmp_op(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    return PyValue(int64_t(0));  // stub - dispatch to specific comparison handlers
}

PyValue Interpreter::handle_intrinsic_print(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.empty()) return PyValue(int64_t(0));

    std::vector<PyValue> args;
    for (auto op : inst.operands) {
        args.push_back(get_slot(frame.get(), op));
    }
    return builtin_print_impl(args);
}

PyValue Interpreter::handle_intrinsic_range(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    (void)frame;
    (void)inst;
    return PyValue(int64_t(0));
}

PyValue Interpreter::handle_intrinsic_type(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    (void)frame; (void)inst;
    return PyValue(int64_t(0));  // stub
}

PyValue Interpreter::handle_intrinsic_len(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    if (inst.operands.empty()) return PyValue(int64_t(0));
    auto val = get_slot(frame.get(), inst.operands[0]);
    if (auto* s = std::get_if<std::string>(&val)) {
        return PyValue((int64_t)s->size());
    }
    return PyValue(int64_t(0));  // stub for other types
}

PyValue Interpreter::handle_intrinsic_init(std::unique_ptr<CallFrame>& frame, IRInst& inst) {
    (void)frame; (void)inst;
    return PyValue(int64_t(0));  // stub
}

// ===== Globals =====

void Interpreter::set_global(const std::string& name, PyValue value) {
    global_vars_[name] = value;
}

std::optional<PyValue> Interpreter::get_global(const std::string& name) {
    auto it = global_vars_.find(name);
    if (it != global_vars_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::unordered_map<std::string, PyValue>& Interpreter::globals() {
    return global_vars_;
}

// ===== Builtins =====

void Interpreter::register_builtin(const std::string& name, std::function<PyValue(std::vector<PyValue>)> fn) {
    builtins_[name] = fn;
}

} // namespace pyc::ir
