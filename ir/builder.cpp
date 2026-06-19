// ir/builder.cpp - Implementation of IRBuilder from AST
// Transforms AST nodes to intermediate representation
//
// Architecture: AST -> IR -> LLVM IR/interpreter
// This file handles AST traversal to produce IR instructions with basic type info

#include "ir/builder.h"
#include "frontend/ast.h"
#include <memory>
#include <unordered_map>
#include <variant>
#include <stdexcept>
#include <iostream>

namespace pyc::ir::builder {

// ===== Module build =====

void IRBuilder::build(const ast::Module& mod) {
    module = std::make_unique<IRModule>();
    
    for (auto& stmt : mod.body()) {
        build_stmt(*stmt);
    }
}

void IRBuilder::build_stmt(const ast::Stmt& stmt) {
    if (auto* fd = dynamic_cast<const ast::FunctionDef*>(&stmt)) {
        build_function(*fd);
    } else if (auto* cd = dynamic_cast<const ast::ClassDef*>(&stmt)) {
        build_class(*cd);
    } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&stmt)) {
        build_if_stmt(*ifs);
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&stmt)) {
        build_for_stmt(*fs);
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&stmt)) {
        build_while_stmt(*ws);
    } else if (auto* as = dynamic_cast<const ast::AssignStmt*>(&stmt)) {
        build_assign_stmt(*as);
    } else if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(&stmt)) {
        build_return_stmt(*ret);
    } else if (auto* aug = dynamic_cast<const ast::AugAssignStmt*>(&stmt)) {
        build_augassign_stmt(*aug);
    }
    // Pass/Break/Continue are handled as simple block continuations
    // Delete/Global/Nonlocal/Assert/Raise/With are no-ops for now
}

// ===== Function/class building =====

void IRBuilder::build_function(const ast::FunctionDef& func) {
    auto* func_ir = new IRFunction();
    func_ir->name = func.name();
    
    // Allocate slots for parameters
    for (auto& arg : func.args()) {
        func_ir->param_names.push_back(arg.name);
        alloc_local(arg.name);
    }
    
    // Build function body
    current_scope_ = func.name();
    current_func_ = func_ir;
    
    auto* entry_block = func_ir->new_block("entry");
    current_func_->entry_block_id = entry_block->id;
    current_block_ = entry_block;
    func_ir->blocks.push_back(entry_block);
    
    for (auto& stmt : func.body()) {
        build_stmt(*stmt);
    }
    
    // Add implicit return if last block doesn't end with RETURN
    if (!func_ir->blocks.empty()) {
        auto* last_block = func_ir->blocks.back();
        bool ends_with_return = false;
        for (auto& instr : last_block->instrs) {
            if (instr->kind == IRInstKind::RETURN) {
                ends_with_return = true;
                break;
            }
        }
        if (!ends_with_return) {
            auto* ret_inst = func_ir->new_inst(IRInstKind::RETURN, "");
            auto* zero = func_ir->new_inst(IRInstKind::LOADCONST_INT, "");
            zero->is_const = true;
            std::get<double>(zero->const_val) = 0.0;
            ret_inst->operands.push_back(zero->id);
        }
    }
    
    module->functions[func.name()] = func_ir;
    module->func_list.push_back(func_ir);
}

void IRBuilder::build_class(const ast::ClassDef& cls) {
    // Create a constructor function for the class
    auto* ctor_ir = new IRFunction();
    ctor_ir->name = cls.name() + ".__init__";
    ctor_ir->param_names.push_back("self");
    alloc_local("self");
    
    auto* entry_block = ctor_ir->new_block("entry");
    ctor_ir->entry_block_id = entry_block->id;
    current_func_ = ctor_ir;
    current_block_ = entry_block;
    ctor_ir->blocks.push_back(entry_block);
    
    // Build class body methods as separate functions
    for (auto& stmt : cls.body()) {
        if (auto* fd = dynamic_cast<const ast::FunctionDef*>(stmt.get())) {
            if (fd->name() != "__init__") {
                auto* method_ir = new IRFunction();
                method_ir->name = cls.name() + "." + fd->name();
                method_ir->param_names.push_back("self");
                alloc_local("self");
                for (auto& arg : fd->args()) {
                    method_ir->param_names.push_back(arg.name);
                    alloc_local(arg.name);
                }
                
                auto* method_block = method_ir->new_block("entry");
                method_ir->entry_block_id = method_block->id;
                current_func_ = method_ir;
                current_block_ = method_block;
                method_ir->blocks.push_back(method_block);
                
                for (auto& body_stmt : fd->body()) {
                    build_stmt(*body_stmt);
                }
                
                // Implicit return
                auto* ret_inst = method_ir->new_inst(IRInstKind::RETURN, "");
                auto* zero = method_ir->new_inst(IRInstKind::LOADCONST_INT, "");
                zero->is_const = true;
                std::get<double>(zero->const_val) = 0.0;
                ret_inst->operands.push_back(zero->id);
                
                module->functions[cls.name() + "." + fd->name()] = method_ir;
                module->func_list.push_back(method_ir);
            }
        }
    }
    
    module->functions[cls.name() + ".__init__"] = ctor_ir;
    module->func_list.push_back(ctor_ir);
}

// ===== Statement building =====

void IRBuilder::build_if_stmt(const ast::IfStmt& ifs) {
    uint32_t test_val = build_expr(*ifs.test());
    
    auto* cond_true_blk = current_func_->new_block("if_true");
    auto* cond_false_blk = current_func_->new_block("if_false");
    auto* merge_blk = current_func_->new_block("if_merge");
    
    cond_true_blk->successors.push_back(merge_blk->id);
    cond_false_blk->successors.push_back(merge_blk->id);
    
    // Conditional branch
    auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "if_branch");
    branch_inst->operands = {test_val, cond_true_blk->id, cond_false_blk->id};
    current_block_->successors.push_back(cond_true_blk->id);
    current_block_->successors.push_back(cond_false_blk->id);
    
    // True branch
    current_block_ = cond_true_blk;
    for (auto& s : ifs.body()) {
        build_stmt(*s);
    }
    
    // False branch
    current_block_ = cond_false_blk;
    for (auto& s : ifs.orelse()) {
        build_stmt(*s);
    }
    
    // Merge block
    current_block_ = merge_blk;
}

void IRBuilder::build_for_stmt(const ast::ForStmt& fs) {
    uint32_t iter_slot = alloc_local("__for_iter__");
    
    // Create loop blocks
    auto loop_cond_blk = current_func_->new_block("for_cond");
    auto loop_body_blk = current_func_->new_block("for_body");
    auto loop_merge_blk = current_func_->new_block("for_merge");
    
    loop_cond_blk->successors.push_back(loop_body_blk->id);
    loop_cond_blk->successors.push_back(loop_merge_blk->id);
    loop_body_blk->successors.push_back(loop_cond_blk->id);
    
    // Load the iterator expression
    auto loop_val = build_expr(*fs.iter());
    
    // Store iterator in local
    auto* store_inst = current_func_->new_inst(IRInstKind::STORELOCAL, "__for_iter__");
    store_inst->operands.push_back(iter_slot);
    store_inst->operands.push_back(loop_val);
    
    // Set up loop condition block
    current_block_ = loop_cond_blk;
    
    // Get next item from iterator (stub: just use the iterator value)
    auto* next_call = current_func_->new_inst(IRInstKind::CALL, "next");
    next_call->operands.push_back(iter_slot);
    
    // Check if iteration is done (branch based on result)
    auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "for_branch");
    branch_inst->operands.push_back(next_call->id);
    branch_inst->operands.push_back(loop_body_blk->id);
    branch_inst->operands.push_back(loop_merge_blk->id);
    current_block_->successors.push_back(loop_body_blk->id);
    current_block_->successors.push_back(loop_merge_blk->id);
    
    // Body block
    current_block_ = loop_body_blk;
    auto body_alloc = alloc_local(fs.target());
    auto* assign_inst = current_func_->new_inst(IRInstKind::STORELOCAL, fs.target());
    assign_inst->operands.push_back(body_alloc);
    assign_inst->operands.push_back(next_call->id);
    
    for (auto& s : fs.body()) {
        build_stmt(*s);
    }
    
    // Merge block
    current_block_ = loop_merge_blk;
}

void IRBuilder::build_while_stmt(const ast::WhileStmt& ws) {
    auto cond_blk = current_func_->new_block("while_cond");
    auto body_blk = current_func_->new_block("while_body");
    auto merge_blk = current_func_->new_block("while_merge");
    
    cond_blk->successors.push_back(body_blk->id);
    cond_blk->successors.push_back(merge_blk->id);
    body_blk->successors.push_back(cond_blk->id);
    
    // Evaluate test expression
    current_block_ = cond_blk;
    auto test_val = build_expr(*ws.test());
    
    // Branch based on test result
    auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "while_branch");
    branch_inst->operands.push_back(test_val);
    branch_inst->operands.push_back(body_blk->id);
    branch_inst->operands.push_back(merge_blk->id);
    current_block_->successors.push_back(body_blk->id);
    current_block_->successors.push_back(merge_blk->id);
    
    // Body block
    current_block_ = body_blk;
    for (auto& s : ws.body()) {
        build_stmt(*s);
    }
    
    // Merge block
    current_block_ = merge_blk;
}

void IRBuilder::build_assign_stmt(const ast::AssignStmt& as) {
    auto val = build_expr(*as.value());
    
    for (auto& target : as.targets()) {
        auto slot = alloc_local(target);
        auto* store = current_func_->new_inst(IRInstKind::STORELOCAL, target);
        store->operands.push_back(slot);
        store->operands.push_back(val);
    }
}

void IRBuilder::build_return_stmt(const ast::ReturnStmt& ret) {
    uint32_t val = UINT32_MAX;
    if (ret.has_value()) {
        val = build_expr(*ret.value());
    }
    
    auto* ret_inst = current_func_->new_inst(IRInstKind::RETURN, "return");
    ret_inst->operands.push_back(val);
}

void IRBuilder::build_augassign_stmt(const ast::AugAssignStmt& aug) {
    auto op = augment_to_binop(aug.op_);
    auto slot = alloc_local(aug.target());
    
    auto* load = current_func_->new_inst(IRInstKind::LOADLOCAL, aug.target());
    load->operands.push_back(slot);
    
    auto rhs = build_expr(*aug.value());
    
    auto* binop = current_func_->new_inst(op);
    binop->operands.push_back(load->id);
    binop->operands.push_back(rhs);
    
    auto* store = current_func_->new_inst(IRInstKind::STORELOCAL, aug.target());
    store->operands.push_back(slot);
    store->operands.push_back(binop->id);
}

// ===== Expression building =====

uint32_t IRBuilder::build_expr(const ast::Expr& expr) {
    if (auto* lit = dynamic_cast<const ast::IntLiteral*>(&expr)) return build_int_literal(*lit);
    if (auto* lit = dynamic_cast<const ast::FloatLiteral*>(&expr)) return build_float_literal(*lit);
    if (auto* lit = dynamic_cast<const ast::StrLiteral*>(&expr)) return build_str_literal(*lit);
    if (auto* lit = dynamic_cast<const ast::BoolLiteral*>(&expr)) return build_bool_literal(*lit);
    if (auto* lit = dynamic_cast<const ast::EllipsisLiteral*>(&expr)) return build_ellipsis_literal(*lit);
    if (auto* n = dynamic_cast<const ast::Name*>(&expr)) return build_name(*n);
    if (auto* b = dynamic_cast<const ast::BinOpExpr*>(&expr)) return build_binop(*b);
    if (auto* u = dynamic_cast<const ast::UnaryOpExpr*>(&expr)) return build_unary(*u);
    if (auto* c = dynamic_cast<const ast::CallExpr*>(&expr)) return build_call(*c);
    if (auto* a = dynamic_cast<const ast::AttrExpr*>(&expr)) return build_attr(*a);
    if (auto* l = dynamic_cast<const ast::ListExpr*>(&expr)) return build_list(*l);
    return UINT32_MAX;
}

uint32_t IRBuilder::build_int_literal(const ast::IntLiteral& lit) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_INT, "const");
    inst->is_const = true;
    std::get<double>(inst->const_val) = static_cast<double>(lit.value());
    return inst->id;
}

uint32_t IRBuilder::build_float_literal(const ast::FloatLiteral& lit) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_FLOAT, "const");
    inst->is_const = true;
    std::get<double>(inst->const_val) = lit.value();
    return inst->id;
}

uint32_t IRBuilder::build_str_literal(const ast::StrLiteral& lit) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_STR, "const");
    inst->is_const = true;
    inst->const_val = lit.value();
    return inst->id;
}

uint32_t IRBuilder::build_bool_literal(const ast::BoolLiteral& lit) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_INT, "const");
    inst->is_const = true;
    std::get<double>(inst->const_val) = lit.value() ? 1.0 : 0.0;
    return inst->id;
}

uint32_t IRBuilder::build_ellipsis_literal(const ast::EllipsisLiteral& lit) {
    (void)lit;
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_INT, "const");
    inst->is_const = true;
    std::get<double>(inst->const_val) = -1.0;
    return inst->id;
}

uint32_t IRBuilder::build_name(const ast::Name& name) {
    auto& name_str = name.id();
    auto it = locals_.find(name_str);
    if (it != locals_.end()) {
        auto* inst = current_func_->new_inst(IRInstKind::LOADLOCAL, name_str);
        inst->operands.push_back(it->second);
        return inst->id;
    }
    auto* inst = current_func_->new_inst(IRInstKind::LOADGLOBAL, name_str);
    return inst->id;
}

uint32_t IRBuilder::build_binop(const ast::BinOpExpr& expr) {
    auto lhs = build_expr(*expr.left());
    auto rhs = build_expr(*expr.right());
    
    IRInstKind op_kind;
    switch (expr.op()) {
        case ast::BinOpExpr::ADD: op_kind = IRInstKind::ADD; break;
        case ast::BinOpExpr::SUB: op_kind = IRInstKind::SUB; break;
        case ast::BinOpExpr::MUL: op_kind = IRInstKind::MUL; break;
        case ast::BinOpExpr::DIV: op_kind = IRInstKind::DIV; break;
        case ast::BinOpExpr::FLOOR_DIV: op_kind = IRInstKind::DIV; break;
        case ast::BinOpExpr::MOD: op_kind = IRInstKind::MOD; break;
        case ast::BinOpExpr::POW:  op_kind = IRInstKind::POW; break;
        case ast::BinOpExpr::EQ:   op_kind = IRInstKind::EQ; break;
        case ast::BinOpExpr::NE:   op_kind = IRInstKind::NE; break;
        case ast::BinOpExpr::LT:   op_kind = IRInstKind::LT; break;
        case ast::BinOpExpr::GT:   op_kind = IRInstKind::GT; break;
        case ast::BinOpExpr::LE:   op_kind = IRInstKind::LE; break;
        case ast::BinOpExpr::GE:   op_kind = IRInstKind::GE; break;
        case ast::BinOpExpr::AND:  op_kind = IRInstKind::AND; break;
        case ast::BinOpExpr::OR:   op_kind = IRInstKind::OR; break;
        default: op_kind = IRInstKind::ADD; break;
    }
    
    auto* inst = current_func_->new_inst(op_kind);
    inst->operands.push_back(lhs);
    inst->operands.push_back(rhs);
    return inst->id;
}

uint32_t IRBuilder::build_unary(const ast::UnaryOpExpr& expr) {
    auto operand = build_expr(*expr.operand());
    
    IRInstKind op_kind;
    switch (expr.op()) {
        case ast::UnaryOpExpr::NEG: op_kind = IRInstKind::SUB; break;
        case ast::UnaryOpExpr::NOT: op_kind = IRInstKind::NOT; break;
        case ast::UnaryOpExpr::UPLUS: op_kind = IRInstKind::ADD; break;
        default: op_kind = IRInstKind::LOADLOCAL; break;
    }
    
    auto* inst = current_func_->new_inst(op_kind);
    inst->operands.push_back(operand);
    return inst->id;
}

uint32_t IRBuilder::build_call(const ast::CallExpr& expr) {
    auto* inst = current_func_->new_inst(IRInstKind::CALL, "");
    
    auto func_id = build_expr(*expr.func());
    inst->operands.push_back(func_id);
    
    for (auto& arg : expr.args()) {
        auto arg_id = build_expr(*arg);
        inst->operands.push_back(arg_id);
    }
    
    return inst->id;
}

uint32_t IRBuilder::build_attr(const ast::AttrExpr& expr) {
    auto obj_id = build_expr(*expr.obj());
    
    auto* inst = current_func_->new_inst(IRInstKind::GETATTR, expr.attr());
    inst->operands.push_back(obj_id);
    
    return inst->id;
}

uint32_t IRBuilder::build_list(const ast::ListExpr& expr) {
    auto* inst = current_func_->new_inst(IRInstKind::MAKE_LIST, "list");
    
    for (auto& elem : expr.elems()) {
        auto elem_id = build_expr(*elem);
        inst->operands.push_back(elem_id);
    }
    
    return inst->id;
}

// ===== Helper methods =====

IRInstKind IRBuilder::augment_to_binop(ast::AugAssignStmt::Op op) {
    switch (op) {
        case ast::AugAssignStmt::ADD: return IRInstKind::ADD;
        case ast::AugAssignStmt::SUB: return IRInstKind::SUB;
        case ast::AugAssignStmt::MUL: return IRInstKind::MUL;
        case ast::AugAssignStmt::DIV: return IRInstKind::DIV;
        default: return IRInstKind::ADD;
    }
}

uint32_t IRBuilder::alloc_local(std::string name) {
    auto local_id = static_cast<uint32_t>(locals_.size());
    locals_[name] = local_id;
    return local_id;
}

void IRBuilder::store_local(uint32_t local_var, uint32_t value) {
    auto* inst = current_func_->new_inst(IRInstKind::STORELOCAL);
    inst->operands.push_back(local_var);
    inst->operands.push_back(value);
}

uint32_t IRBuilder::load_local(uint32_t local_var) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADLOCAL);
    inst->operands.push_back(local_var);
    return inst->id;
}

uint32_t IRBuilder::load_global(std::string name) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADGLOBAL, name);
    return inst->id;
}

void IRBuilder::store_global(std::string name, uint32_t value) {
    auto* inst = current_func_->new_inst(IRInstKind::STOREGLOBAL, name);
    inst->operands.push_back(value);
}

} // namespace pyc::ir::builder
