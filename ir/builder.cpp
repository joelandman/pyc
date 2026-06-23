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

// Helper to set constant values on an instruction
static void set_const_double(std::variant<int, double, std::string>& val, double value) {
    val = value;
}

// ===== Module build =====

void IRBuilder::build(const ast::Module& mod) {
    module = std::make_unique<IRModule>();
    for (auto& stmt : mod.body()) {
        build_stmt(*stmt);
    }
}

void IRBuilder::build(const ast::Module& mod, const std::string& mod_name) {
    module_name_ = mod_name;
    build(mod);
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
    } else if (auto* tas = dynamic_cast<const ast::TupleAssignStmt*>(&stmt)) {
        build_tuple_assign_stmt(*tas);
    } else if (auto* ne = dynamic_cast<const ast::NamedExpr*>(&stmt)) {
        build_named_expr(*ne);
    } else if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(&stmt)) {
        build_return_stmt(*ret);
    } else if (auto* aug = dynamic_cast<const ast::AugAssignStmt*>(&stmt)) {
        build_augassign_stmt(*aug);
    } else if (auto* imp = dynamic_cast<const ast::ImportStmt*>(&stmt)) {
        build_import_stmt(*imp);
    } else if (auto* del = dynamic_cast<const ast::DeleteStmt*>(&stmt)) {
        build_delete_stmt(*del);
    } else if (auto* gl = dynamic_cast<const ast::GlobalStmt*>(&stmt)) {
        build_global_stmt(*gl);
    } else if (auto* nt = dynamic_cast<const ast::NonlocalStmt*>(&stmt)) {
        build_nonlocal_stmt(*nt);
    } else if (auto* asp = dynamic_cast<const ast::AssertStmt*>(&stmt)) {
        build_assert_stmt(*asp);
    } else if (auto* ra = dynamic_cast<const ast::RaiseStmt*>(&stmt)) {
        build_raise_stmt(*ra);
    } else if (auto* wt = dynamic_cast<const ast::WithStmt*>(&stmt)) {
        build_with_stmt(*wt);
    } else if (auto* tr = dynamic_cast<const ast::TryStmt*>(&stmt)) {
        build_try_stmt(*tr);
    } else if (auto* ms = dynamic_cast<const ast::MatchStmt*>(&stmt)) {
        build_match_stmt(*ms);
    } else if (auto* br = dynamic_cast<const ast::BreakStmt*>(&stmt)) {
        build_break_stmt();
    } else if (auto* co = dynamic_cast<const ast::ContinueStmt*>(&stmt)) {
        build_continue_stmt();
    }
    // Pass is a no-op
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
            set_const_double(zero->const_val, 0.0);
            ret_inst->operands.push_back(zero->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(ret_inst));
        }
    }
    
     module->functions[func.name()] = func_ir;
    module->func_list.push_back(func_ir);
    
    // Apply decorators
    for (auto& dec_expr : func.decorators()) {
        auto dec_func_id = build_expr(*dec_expr);
        auto* dec_call = current_func_->new_inst(IRInstKind::CALL, "decorator_apply");
        
        dec_call->operands.push_back(dec_func_id);
        
        // Load the function being decorated
        auto* func_load = current_func_->new_inst(IRInstKind::LOADGLOBAL, func.name());
        dec_call->operands.push_back(func_load->id);
        
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(dec_call));
        
        // Store decorated function back
        auto* store = current_func_->new_inst(IRInstKind::STOREGLOBAL, func.name());
        store->operands.push_back(dec_call->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
    }
}

void IRBuilder::build_class(const ast::ClassDef& cls) {
    // Create a class object using NEWOBJ instruction
    // The class object will store methods and base classes
    
    // Create the class constructor function
    auto* ctor_ir = new IRFunction();
    ctor_ir->name = cls.name() + ".__init__";
    ctor_ir->param_names.push_back("self");
    alloc_local("self");
    
    // Store base class names as class attributes
    for (auto& base_name : cls.bases()) {
        auto* load_str = current_func_->new_inst(IRInstKind::LOADCONST_STR, "base");
        load_str->is_const = true;
        load_str->const_val = base_name;
        
        auto* self_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "self");
        auto self_slot = locals_["self"];
        self_load->operands.push_back(self_slot);
        
        auto* setattr = current_func_->new_inst(IRInstKind::SETATTR, "__bases__");
        setattr->operands.push_back(self_load->id);
        setattr->operands.push_back(load_str->id);
        
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(setattr));
    }
    
    auto* entry_block = ctor_ir->new_block("entry");
    ctor_ir->entry_block_id = entry_block->id;
    current_func_ = ctor_ir;
    current_block_ = entry_block;
    
    // Build class body methods as separate functions
    for (auto& stmt : cls.body()) {
        if (auto* fd = dynamic_cast<const ast::FunctionDef*>(stmt.get())) {
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
            
            for (auto& body_stmt : fd->body()) {
                build_stmt(*body_stmt);
            }
            
            // Implicit return
            auto* ret_inst = method_ir->new_inst(IRInstKind::RETURN, "");
            auto* zero = method_ir->new_inst(IRInstKind::LOADCONST_INT, "");
            zero->is_const = true;
            set_const_double(zero->const_val, 0.0);
            ret_inst->operands.push_back(zero->id);
            
            module->functions[cls.name() + "." + fd->name()] = method_ir;
            module->func_list.push_back(method_ir);
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
    uint32_t index_slot = alloc_local("__for_index__");
    
    // Create loop blocks
    auto loop_cond_blk = current_func_->new_block("for_cond");
    auto loop_body_blk = current_func_->new_block("for_body");
    auto loop_merge_blk = current_func_->new_block("for_merge");
    
    loop_cond_blk->successors.push_back(loop_body_blk->id);
    loop_cond_blk->successors.push_back(loop_merge_blk->id);
    loop_body_blk->successors.push_back(loop_cond_blk->id);
    
    // Push loop context
    loop_stack_.push_back({loop_cond_blk->id, loop_merge_blk->id});
    
    // Load the iterator expression
    auto loop_val = build_expr(*fs.iter());
    
    // Store iterator in local
    auto* store_inst = current_func_->new_inst(IRInstKind::STORELOCAL, "__for_iter__");
    store_inst->operands.push_back(iter_slot);
    store_inst->operands.push_back(loop_val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(store_inst));
    
    // Initialize index to 0
    auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "index_init");
    zero->is_const = true;
    set_const_double(zero->const_val, 0.0);
    auto* index_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__for_index__");
    index_store->operands.push_back(index_slot);
    index_store->operands.push_back(zero->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(index_store));
    
    // Set up loop condition block
    current_block_ = loop_cond_blk;
    
    // Load current index
    auto* index_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "index_load");
    index_load->operands.push_back(index_slot);
    
    // Load iterator (list)
    auto* iter_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "iter_load");
    iter_load->operands.push_back(iter_slot);
    
    // Get length of list via INTRINSIC_LEN
    auto* len_inst = current_func_->new_inst(IRInstKind::INTRINSIC_LEN, "list_len");
    len_inst->operands.push_back(iter_load->id);
    
    // Compare: index < len
    auto* cmp_inst = current_func_->new_inst(IRInstKind::LT, "index_lt_len");
    cmp_inst->operands.push_back(index_load->id);
    cmp_inst->operands.push_back(len_inst->id);
    
    // Branch based on comparison
    auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "for_branch");
    branch_inst->operands.push_back(cmp_inst->id);
    branch_inst->operands.push_back(loop_body_blk->id);
    branch_inst->operands.push_back(loop_merge_blk->id);
    current_block_->successors.push_back(loop_body_blk->id);
    current_block_->successors.push_back(loop_merge_blk->id);
    
    // Body block
    current_block_ = loop_body_blk;
    
    // Get element at current index via LIST_GET
    auto body_alloc = alloc_local(fs.target());
    auto* list_get = current_func_->new_inst(IRInstKind::LIST_GET, "get_item");
    list_get->operands.push_back(iter_load->id);
    list_get->operands.push_back(index_load->id);
    
    auto* body_store = current_func_->new_inst(IRInstKind::STORELOCAL, fs.target());
    body_store->operands.push_back(body_alloc);
    body_store->operands.push_back(list_get->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(body_store));
    
    for (auto& s : fs.body()) {
        build_stmt(*s);
    }
    
    // Increment index: index = index + 1
    auto* one = current_func_->new_inst(IRInstKind::LOADCONST_INT, "one");
    one->is_const = true;
    set_const_double(one->const_val, 1.0);
    auto* add_inst = current_func_->new_inst(IRInstKind::ADD, "index_inc");
    add_inst->operands.push_back(index_load->id);
    add_inst->operands.push_back(one->id);
    auto* inc_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__for_index__");
    inc_store->operands.push_back(index_slot);
    inc_store->operands.push_back(add_inst->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inc_store));
    
    // Jump back to condition
    auto* jump_inst = current_func_->new_inst(IRInstKind::JUMP, "for_jump");
    jump_inst->operands.push_back(loop_cond_blk->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump_inst));
    
    // Pop loop context
    loop_stack_.pop_back();
    
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
    
    // Push loop context
    loop_stack_.push_back({cond_blk->id, merge_blk->id});
    
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
    
    // Pop loop context
    loop_stack_.pop_back();
    
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
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
    }
}

void IRBuilder::build_tuple_assign_stmt(const ast::TupleAssignStmt& tas) {
    auto val = build_expr(*tas.value());
    
    // The value should be a tuple/list, extract elements by index
    for (size_t i = 0; i < tas.targets().size(); i++) {
        auto* target = tas.targets()[i].get();
        if (auto* name = dynamic_cast<const ast::Name*>(target)) {
            auto slot = alloc_local(name->id());
            
            // Emit LIST_GET to extract element i from the tuple
            auto* list_get = current_func_->new_inst(IRInstKind::LIST_GET, "tuple_get");
            list_get->operands.push_back(val);
            
            // Create index constant
            auto* idx_const = current_func_->new_inst(IRInstKind::LOADCONST_INT, "idx");
            idx_const->is_const = true;
            idx_const->const_val = static_cast<double>(i);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(idx_const));
            list_get->operands.push_back(idx_const->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(list_get));
            
            auto* store = current_func_->new_inst(IRInstKind::STORELOCAL, name->id());
            store->operands.push_back(slot);
            store->operands.push_back(list_get->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
        }
    }
}

void IRBuilder::build_named_expr(const ast::NamedExpr& ne) {
    auto val = build_expr(*ne.value());
    
    // Store in local variable and return the value
    auto slot = alloc_local(ne.name());
    auto* store = current_func_->new_inst(IRInstKind::STORELOCAL, ne.name());
    store->operands.push_back(slot);
    store->operands.push_back(val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
    
    // Return the stored value
    auto* load = current_func_->new_inst(IRInstKind::LOADLOCAL, ne.name());
    load->operands.push_back(slot);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(load));
}

void IRBuilder::build_return_stmt(const ast::ReturnStmt& ret) {
    uint32_t val = UINT32_MAX;
    if (ret.has_value()) {
        val = build_expr(*ret.value());
    }
    
    auto* ret_inst = current_func_->new_inst(IRInstKind::RETURN, "return");
    ret_inst->operands.push_back(val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(ret_inst));
}

void IRBuilder::build_augassign_stmt(const ast::AugAssignStmt& aug) {
    auto op = augment_to_binop(aug.op_);
    auto slot = alloc_local(aug.target());
    
    auto* load = current_func_->new_inst(IRInstKind::LOADLOCAL, aug.target());
    load->operands.push_back(slot);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(load));
    
    auto rhs = build_expr(*aug.value());
    
    auto* binop = current_func_->new_inst(op);
    binop->operands.push_back(load->id);
    binop->operands.push_back(rhs);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(binop));
    
    auto* store = current_func_->new_inst(IRInstKind::STORELOCAL, aug.target());
    store->operands.push_back(slot);
    store->operands.push_back(binop->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
}

  void IRBuilder::build_import_stmt(const ast::ImportStmt& imp) {
    auto* from_imp = imp.from_import_ptr();
    if (!from_imp) {
        return;
    }
    
    // Handle simple import: import module1, module2, ...
    // names_ contains the module names for simple imports
    if (!from_imp->names_.empty()) {
        for (auto& module_name : from_imp->names_) {
            if (module_name.empty()) {
                continue;
            }
            
            // Call pyc_import_module(module_name)
            auto* import_call = current_func_->new_inst(IRInstKind::CALL, "pyc_import_module");
            
            // Create string constant for module name
            auto* str_const = current_func_->new_inst(IRInstKind::LOADCONST_STR, "module_name");
            str_const->is_const = true;
            str_const->const_val = module_name;
            import_call->operands.push_back(str_const->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(str_const));
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(import_call));
            
            // Store result in global variable with module name
            auto* store = current_func_->new_inst(IRInstKind::STOREGLOBAL, module_name);
            store->operands.push_back(import_call->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
        }
        return;
    }
    
    // Handle relative imports: from . import x, from ..pkg import y
    if (from_imp->level > 0) {
        // Determine parent module name based on current function's module context
        // The parent module is the module being compiled (module_name_)
        std::string parent_module = module_name_.empty() ? "" : module_name_;
        
        // Call pyc_import_relative(module_name, level, parent_module)
        auto* import_call = current_func_->new_inst(IRInstKind::CALL, "pyc_import_relative");
        
        // Module name (may be empty for "from . import x")
        auto* mod_const = current_func_->new_inst(IRInstKind::LOADCONST_STR, "rel_module");
        mod_const->is_const = true;
        mod_const->const_val = from_imp->module_name_;
        import_call->operands.push_back(mod_const->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(mod_const));
        
        // Level (relative import depth)
        auto* level_const = current_func_->new_inst(IRInstKind::LOADCONST_INT, "rel_level");
        level_const->is_const = true;
        level_const->const_val = from_imp->level;
        import_call->operands.push_back(level_const->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(level_const));
        
        // Parent module name
        auto* parent_const = current_func_->new_inst(IRInstKind::LOADCONST_STR, "rel_parent");
        parent_const->is_const = true;
        parent_const->const_val = parent_module;
        import_call->operands.push_back(parent_const->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(parent_const));
        
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(import_call));
        
        // For each imported name, get it from the result and store in global
        std::string result_var = "__rel_import_result__";
        auto* module_store = current_func_->new_inst(IRInstKind::STOREGLOBAL, result_var);
        module_store->operands.push_back(import_call->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(module_store));
        
        for (auto& name : from_imp->names_) {
            auto* module_load = current_func_->new_inst(IRInstKind::LOADGLOBAL, result_var);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(module_load));
            
            auto* attr_const = current_func_->new_inst(IRInstKind::LOADCONST_STR, "attr_name");
            attr_const->is_const = true;
            attr_const->const_val = name;
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(attr_const));
            
            auto* getattr_call = current_func_->new_inst(IRInstKind::CALL, "pyc_getattr");
            getattr_call->operands.push_back(module_load->id);
            getattr_call->operands.push_back(attr_const->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(getattr_call));
            
            auto* store = current_func_->new_inst(IRInstKind::STOREGLOBAL, name);
            store->operands.push_back(getattr_call->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
        }
        return;
    }
    
    std::string module_name = from_imp->module_name_;
    if (module_name.empty()) {
        // Empty module name, skip
        return;
    }
    
    // Handle from X import Y, Z, W
    // Import the module first
    auto* import_call = current_func_->new_inst(IRInstKind::CALL, "pyc_import_module");
    
    // Create string constant for module name
    auto* str_const = current_func_->new_inst(IRInstKind::LOADCONST_STR, "module_name");
    str_const->is_const = true;
    str_const->const_val = module_name;
    import_call->operands.push_back(str_const->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(str_const));
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(import_call));
    
    // Store module in a temporary global
    std::string module_var = "__imported_module__";
    auto* module_store = current_func_->new_inst(IRInstKind::STOREGLOBAL, module_var);
    module_store->operands.push_back(import_call->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(module_store));
    
    // For each imported name, get it from the module and store in global
    for (auto& name : from_imp->names_) {
        // Load the module
        auto* module_load = current_func_->new_inst(IRInstKind::LOADGLOBAL, module_var);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(module_load));
        
        // Create string constant for attribute name
        auto* attr_const = current_func_->new_inst(IRInstKind::LOADCONST_STR, "attr_name");
        attr_const->is_const = true;
        attr_const->const_val = name;
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(attr_const));
        
        // Call pyc_getattr(module, name)
        auto* getattr_call = current_func_->new_inst(IRInstKind::CALL, "pyc_getattr");
        getattr_call->operands.push_back(module_load->id);
        getattr_call->operands.push_back(attr_const->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(getattr_call));
        
        // Store in global variable with the imported name
        auto* store = current_func_->new_inst(IRInstKind::STOREGLOBAL, name);
        store->operands.push_back(getattr_call->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
    }
}

void IRBuilder::build_delete_stmt(const ast::DeleteStmt& del) {
    for (auto& target : del.targets()) {
        auto slot = alloc_local(target);
        auto* store = current_func_->new_inst(IRInstKind::STORELOCAL, target);
        store->operands.push_back(slot);
        auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "");
        zero->is_const = true;
        set_const_double(zero->const_val, 0.0);
        store->operands.push_back(zero->id);
    }
}

void IRBuilder::build_global_stmt(const ast::GlobalStmt& gl) {
    for (auto& name : gl.names()) {
        auto* inst = current_func_->new_inst(IRInstKind::LOADGLOBAL, name);
        (void)inst;
    }
}

void IRBuilder::build_nonlocal_stmt(const ast::NonlocalStmt& nt) {
    for (auto& name : nt.names()) {
        auto* inst = current_func_->new_inst(IRInstKind::LOADGLOBAL, name);
        (void)inst;
    }
}

void IRBuilder::build_assert_stmt(const ast::AssertStmt& asp) {
    auto test_val = build_expr(*asp.test());
    
    auto* cond_true_blk = current_func_->new_block("assert_true");
    auto* cond_false_blk = current_func_->new_block("assert_false");
    auto* merge_blk = current_func_->new_block("assert_merge");
    
    cond_true_blk->successors.push_back(merge_blk->id);
    cond_false_blk->successors.push_back(merge_blk->id);
    
    auto* branch = current_func_->new_inst(IRInstKind::BRANCH, "assert_branch");
    branch->operands = {test_val, cond_true_blk->id, cond_false_blk->id};
    current_block_->successors.push_back(cond_true_blk->id);
    current_block_->successors.push_back(cond_false_blk->id);
    
    current_block_ = cond_true_blk;
    
    current_block_ = cond_false_blk;
    if (asp.msg()) {
        build_expr(*asp.msg());
    }
    current_block_->successors.push_back(merge_blk->id);
    auto* jump = current_func_->new_inst(IRInstKind::JUMP, "assert_jump");
    jump->operands.push_back(merge_blk->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump));
    
    current_block_ = merge_blk;
}

void IRBuilder::build_raise_stmt(const ast::RaiseStmt& ra) {
    // Raise the exception object
    if (ra.exc()) {
        auto exc_val = build_expr(*ra.exc());
        
        // Call pyc_raise_exception(exc)
        auto* raise_call = current_func_->new_inst(IRInstKind::CALL, "pyc_raise_exception");
        raise_call->operands.push_back(exc_val);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(raise_call));
    } else {
        // Re-raise current exception (no operand)
        auto* raise_call = current_func_->new_inst(IRInstKind::CALL, "pyc_raise_exception");
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(raise_call));
    }
    
    // Return 0 after raise (unreachable but needed for IR validity)
    auto* ret_inst = current_func_->new_inst(IRInstKind::RETURN, "raise_return");
    auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "");
    zero->is_const = true;
    set_const_double(zero->const_val, 0.0);
    ret_inst->operands.push_back(zero->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(ret_inst));
    current_block_ = nullptr;
}

void IRBuilder::build_with_stmt(const ast::WithStmt& wt) {
    for (auto& item : wt.items()) {
        auto ctx_val = build_expr(*item.context_);
        if (!item.optional_vars_.empty()) {
            auto slot = alloc_local(item.optional_vars_);
            auto* store = current_func_->new_inst(IRInstKind::STORELOCAL, item.optional_vars_);
            store->operands.push_back(slot);
            store->operands.push_back(ctx_val);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(store));
        }
    }
    
    for (auto& s : wt.body()) {
        build_stmt(*s);
    }
}

void IRBuilder::build_try_stmt(const ast::TryStmt& tr) {
    auto try_blk = current_func_->new_block("try_body");
    auto except_blk = current_func_->new_block("except_body");
    auto finally_blk = current_func_->new_block("finally_body");
    auto merge_blk = current_func_->new_block("try_merge");
    
    bool has_finally = !tr.finalbody().empty();
    
    // Store exception in a local variable for propagation
    auto exc_slot = alloc_local("__current_exception__");
    auto* exc_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__current_exception__");
    exc_store->operands.push_back(exc_slot);
    exc_store->operands.push_back(UINT32_MAX);  // Will be updated when exception occurs
    
    try_blk->successors.push_back(except_blk->id);
    if (has_finally) {
        try_blk->successors.push_back(finally_blk->id);
    } else {
        try_blk->successors.push_back(merge_blk->id);
    }
    except_blk->successors.push_back(finally_blk->id);
    except_blk->successors.push_back(merge_blk->id);
    finally_blk->successors.push_back(merge_blk->id);
    
    current_block_ = try_blk;
    for (auto& s : tr.body()) {
        build_stmt(*s);
        
        // Check for exception after each statement
        auto* check_exc = current_func_->new_inst(IRInstKind::CALL, "pyc_get_exception");
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(check_exc));
        
        // Store exception in local variable
        auto* exc_store_inst = current_func_->new_inst(IRInstKind::STORELOCAL, "__current_exception__");
        exc_store_inst->operands.push_back(exc_slot);
        exc_store_inst->operands.push_back(check_exc->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(exc_store_inst));
        
        auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "try_branch");
        branch_inst->operands.push_back(check_exc->id);
        branch_inst->operands.push_back(except_blk->id);
        if (has_finally) {
            branch_inst->operands.push_back(finally_blk->id);
        } else {
            branch_inst->operands.push_back(merge_blk->id);
        }
        current_block_->successors.push_back(except_blk->id);
        if (has_finally) {
            current_block_->successors.push_back(finally_blk->id);
        } else {
            current_block_->successors.push_back(merge_blk->id);
        }
        
        auto* jump_inst = current_func_->new_inst(IRInstKind::JUMP, "try_continue");
        jump_inst->operands.push_back(has_finally ? finally_blk->id : merge_blk->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump_inst));
    }
    
    current_block_ = except_blk;
    for (auto& handler : tr.handlers()) {
        // Store exception type if handler has a type
        if (handler.type_) {
            auto type_slot = alloc_local("__exception_type__");
            auto* type_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__exception_type__");
            type_store->operands.push_back(type_slot);
            auto type_id = build_expr(*handler.type_);
            type_store->operands.push_back(type_id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(type_store));
        }
        
        // Store exception value in local variable
        auto* exc_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "__current_exception__");
        exc_load->operands.push_back(exc_slot);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(exc_load));
        
        for (auto& s : handler.body_) {
            build_stmt(*s);
        }
    }
    auto* jump_to_finally_or_merge = current_func_->new_inst(IRInstKind::JUMP, "except_jump");
    jump_to_finally_or_merge->operands.push_back(has_finally ? finally_blk->id : merge_blk->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump_to_finally_or_merge));
    
    if (has_finally) {
        current_block_ = finally_blk;
        for (auto& s : tr.finalbody()) {
            build_stmt(*s);
        }
        auto* jump_to_merge = current_func_->new_inst(IRInstKind::JUMP, "finally_jump");
        jump_to_merge->operands.push_back(merge_blk->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump_to_merge));
    }
    
    current_block_ = merge_blk;
}

void IRBuilder::build_match_stmt(const ast::MatchStmt& ms) {
    // Translate match/case to nested if/elif/else
    auto subject_id = build_expr(*ms.subject());
    
    auto merge_blk = current_func_->new_block("match_merge");
    
    for (size_t i = 0; i < ms.cases().size(); i++) {
        auto& case_ = ms.cases()[i];
        auto case_blk = current_func_->new_block("case_" + std::to_string(i));
        auto case_body_blk = current_func_->new_block("case_body_" + std::to_string(i));
        
        // For simple value patterns, check equality
        uint32_t cmp_id = UINT32_MAX;
        if (auto* pat = dynamic_cast<const ast::IntLiteral*>(case_.patterns[0].get())) {
            auto* expected = current_func_->new_inst(IRInstKind::LOADCONST_INT, "expected");
            expected->is_const = true;
            expected->const_val = pat->value();
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(expected));
            
            auto* cmp = current_func_->new_inst(IRInstKind::EQ, "match_cmp");
            cmp->operands.push_back(subject_id);
            cmp->operands.push_back(expected->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(cmp));
            cmp_id = cmp->id;
        } else if (auto* pat = dynamic_cast<const ast::StrLiteral*>(case_.patterns[0].get())) {
            auto* expected = current_func_->new_inst(IRInstKind::LOADCONST_STR, "expected");
            expected->is_const = true;
            expected->const_val = pat->value();
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(expected));
            
            auto* cmp = current_func_->new_inst(IRInstKind::EQ, "match_cmp");
            cmp->operands.push_back(subject_id);
            cmp->operands.push_back(expected->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(cmp));
            cmp_id = cmp->id;
        } else {
            // For other patterns, just fall through (treat as always true)
            auto* true_const = current_func_->new_inst(IRInstKind::LOADCONST_INT, "true");
            true_const->is_const = true;
            true_const->const_val = 1.0;
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(true_const));
            cmp_id = true_const->id;
        }
        
        auto* branch = current_func_->new_inst(IRInstKind::BRANCH, "match_branch");
        branch->operands.push_back(cmp_id);
        branch->operands.push_back(case_body_blk->id);
        branch->operands.push_back(i + 1 < ms.cases().size() ? case_blk->id : merge_blk->id);
        current_block_->successors.push_back(case_body_blk->id);
        current_block_->successors.push_back(i + 1 < ms.cases().size() ? case_blk->id : merge_blk->id);
        
        current_block_ = case_body_blk;
        
        // Evaluate guard if present
        if (case_.guard) {
            auto guard_id = build_expr(*case_.guard);
            auto* guard_branch = current_func_->new_inst(IRInstKind::BRANCH, "guard_branch");
            guard_branch->operands.push_back(guard_id);
            guard_branch->operands.push_back(case_body_blk->id);
            guard_branch->operands.push_back(merge_blk->id);
            current_block_->successors.push_back(case_body_blk->id);
            current_block_->successors.push_back(merge_blk->id);
        }
        
        for (auto& s : case_.body) {
            build_stmt(*s);
        }
        
        auto* jump = current_func_->new_inst(IRInstKind::JUMP, "match_jump");
        jump->operands.push_back(merge_blk->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump));
        
        if (i + 1 < ms.cases().size()) {
            current_block_ = case_blk;
        }
    }
    
    current_block_ = merge_blk;
}

void IRBuilder::build_break_stmt() {
    if (!loop_stack_.empty()) {
        auto* jump = current_func_->new_inst(IRInstKind::JUMP, "break");
        jump->operands.push_back(loop_stack_.back().merge_block);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump));
    }
    current_block_ = nullptr;
}

void IRBuilder::build_continue_stmt() {
    if (!loop_stack_.empty()) {
        auto* jump = current_func_->new_inst(IRInstKind::JUMP, "continue");
        jump->operands.push_back(loop_stack_.back().cond_block);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump));
    }
    current_block_ = nullptr;
}

void IRBuilder::build_class_call(const ast::CallExpr& call, const std::string& class_name) {
    // Create a new instance of the class using NEWOBJ
    auto* new_obj = current_func_->new_inst(IRInstKind::NEWOBJ, class_name);
    
    // Get constructor function: ClassName.__init__
    std::string ctor_name = class_name + ".__init__";
    
    // Create instance (self)
    auto* alloc_inst = current_func_->new_inst(IRInstKind::ALLOC, "instance");
    new_obj->operands.push_back(alloc_inst->id);
    
    // Call __init__(self, *args)
    auto* call_inst = current_func_->new_inst(IRInstKind::CALL, ctor_name);
    call_inst->operands.push_back(alloc_inst->id); // self
    
    // Add arguments
    for (auto& arg : call.args()) {
        auto arg_id = build_expr(*arg);
        call_inst->operands.push_back(arg_id);
    }
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
    if (auto* d = dynamic_cast<const ast::DictLiteral*>(&expr)) return build_dict(*d);
    if (auto* t = dynamic_cast<const ast::TupleExpr*>(&expr)) return build_tuple(*t);
    if (auto* s = dynamic_cast<const ast::SubscriptExpr*>(&expr)) return build_subscript(*s);
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&expr)) return build_lambda_expr(*lam);
    if (auto* comp = dynamic_cast<const ast::ListComp*>(&expr)) return build_list_comp(*comp);
    if (auto* comp = dynamic_cast<const ast::SetComp*>(&expr)) return build_set_comp(*comp);
    if (auto* comp = dynamic_cast<const ast::GenExpr*>(&expr)) return build_gen_expr(*comp);
    if (auto* comp = dynamic_cast<const ast::DictComp*>(&expr)) return build_dict_comp(*comp);
   if (auto* j = dynamic_cast<const ast::JoinedStr*>(&expr)) return build_joined_str(*j);
    if (auto* fv = dynamic_cast<const ast::FormattedValue*>(&expr)) return build_formatted_value(*fv);
    if (auto* y = dynamic_cast<const ast::YieldExpr*>(&expr)) return build_yield(*y);
    if (auto* a = dynamic_cast<const ast::AwaitExpr*>(&expr)) return build_await(*a);
    return UINT32_MAX;
}

uint32_t IRBuilder::build_int_literal(const ast::IntLiteral& lit) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_INT, "const");
    inst->is_const = true;
    set_const_double(inst->const_val, static_cast<double>(lit.value()));
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_float_literal(const ast::FloatLiteral& lit) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_FLOAT, "const");
    inst->is_const = true;
    set_const_double(inst->const_val, lit.value());
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_str_literal(const ast::StrLiteral& lit) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_STR, "const");
    inst->is_const = true;
    inst->const_val = lit.value();
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_bool_literal(const ast::BoolLiteral& lit) {
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_INT, "const");
    inst->is_const = true;
    set_const_double(inst->const_val, lit.value() ? 1.0 : 0.0);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_ellipsis_literal(const ast::EllipsisLiteral& lit) {
    (void)lit;
    auto* inst = current_func_->new_inst(IRInstKind::LOADCONST_INT, "const");
    inst->is_const = true;
    set_const_double(inst->const_val, -1.0);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_name(const ast::Name& name) {
    auto& name_str = name.id();
    auto it = locals_.find(name_str);
    if (it != locals_.end()) {
        auto* inst = current_func_->new_inst(IRInstKind::LOADLOCAL, name_str);
        inst->operands.push_back(it->second);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
        return inst->id;
    }
    auto* inst = current_func_->new_inst(IRInstKind::LOADGLOBAL, name_str);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_binop(const ast::BinOpExpr& expr) {
    // Handle short-circuit evaluation for AND and OR
    if (expr.op() == ast::BinOpExpr::AND || expr.op() == ast::BinOpExpr::OR) {
        auto* func_ir = current_func_;
        auto* current_blk = current_block_;
        
        // Create blocks for short-circuit evaluation
        auto* start_block = func_ir->new_block("and_start");
        auto* short_circuit_block = func_ir->new_block("and_short");
        auto* end_block = func_ir->new_block("and_end");
        
        // Switch to start block
        current_block_ = start_block;
        
        // Evaluate left operand
        auto lhs = build_expr(*expr.left());
        
        // Create PHI node for result
        auto* phi = func_ir->new_inst(IRInstKind::PHI, "and_or_result");
        phi->is_phi = true;
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(phi));
        
        if (expr.op() == ast::BinOpExpr::AND) {
            // For AND: if lhs is 0, jump to short_circuit (result=0)
            auto* cmp = func_ir->new_inst(IRInstKind::EQ, "and_cmp");
            cmp->operands.push_back(lhs);
            auto* zero = func_ir->new_inst(IRInstKind::LOADCONST_INT, "");
            zero->is_const = true;
            set_const_double(zero->const_val, 0.0);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(zero));
            cmp->operands.push_back(zero->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(cmp));
            
            // Add PHI operands: start_block -> cmp result, short_block -> 0
            phi->operands.push_back(cmp->id);
            phi->operands.push_back(start_block->id);
            
            // Branch: if cmp is true (lhs==0), jump to short_circuit
            auto* branch = func_ir->new_inst(IRInstKind::BRANCH, "and_branch");
            branch->operands.push_back(cmp->id);
            branch->operands.push_back(short_circuit_block->id);
            branch->operands.push_back(end_block->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(branch));
        } else {
            // For OR: if lhs is non-zero, jump to end (result=lhs)
            auto* cmp = func_ir->new_inst(IRInstKind::NE, "or_cmp");
            cmp->operands.push_back(lhs);
            auto* zero = func_ir->new_inst(IRInstKind::LOADCONST_INT, "");
            zero->is_const = true;
            set_const_double(zero->const_val, 0.0);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(zero));
            cmp->operands.push_back(zero->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(cmp));
            
            // Add PHI operands: start_block -> lhs, short_block -> result
            phi->operands.push_back(lhs);
            phi->operands.push_back(start_block->id);
            
            // Branch: if cmp is true (lhs!=0), jump to end
            auto* branch = func_ir->new_inst(IRInstKind::BRANCH, "or_branch");
            branch->operands.push_back(cmp->id);
            branch->operands.push_back(end_block->id);
            branch->operands.push_back(short_circuit_block->id);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(branch));
        }
        
        // Switch to short_circuit block
        current_block_ = short_circuit_block;
        
        // Evaluate right operand
        auto rhs = build_expr(*expr.right());
        
        // Add PHI operand for short_circuit block
        phi->operands.push_back(rhs);
        phi->operands.push_back(short_circuit_block->id);
        
        // Jump to end
        auto* jump = func_ir->new_inst(IRInstKind::JUMP, "and_jump");
        jump->operands.push_back(end_block->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump));
        
        // Switch to end block
        current_block_ = end_block;
        
        // Load the PHI result
        auto* load = func_ir->new_inst(IRInstKind::LOAD, "and_or_load");
        load->operands.push_back(phi->id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(load));
        
        return load->id;
    }
    
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
        case ast::BinOpExpr::IN:   op_kind = IRInstKind::IN; break;
        case ast::BinOpExpr::IS:   op_kind = IRInstKind::IS; break;
        default: op_kind = IRInstKind::ADD; break;
    }
    
    auto* inst = current_func_->new_inst(op_kind);
    inst->operands.push_back(lhs);
    inst->operands.push_back(rhs);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_unary(const ast::UnaryOpExpr& expr) {
    auto operand = build_expr(*expr.operand());
    
    IRInstKind op_kind;
    switch (expr.op()) {
        case ast::UnaryOpExpr::NEG: {
            auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "");
            zero->is_const = true;
            set_const_double(zero->const_val, 0.0);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(zero));
            auto* inst = current_func_->new_inst(IRInstKind::SUB, "neg");
            inst->operands.push_back(zero->id);
            inst->operands.push_back(operand);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
            return inst->id;
        }
        case ast::UnaryOpExpr::NOT: op_kind = IRInstKind::NOT; break;
        case ast::UnaryOpExpr::UPLUS: return operand;
        default: op_kind = IRInstKind::LOADLOCAL; break;
    }
    
    auto* inst = current_func_->new_inst(op_kind);
    inst->operands.push_back(operand);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_call(const ast::CallExpr& expr) {
    auto* func_expr = expr.func();
    auto* name_expr = dynamic_cast<const ast::Name*>(func_expr);
    if (name_expr && module->functions.count(name_expr->id() + ".__init__") > 0) {
        build_class_call(expr, name_expr->id());
        auto* load = current_func_->new_inst(IRInstKind::LOADGLOBAL, name_expr->id());
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(load));
        return load->id;
    }
    
    auto* inst = current_func_->new_inst(IRInstKind::CALL, "");
    
    auto func_id = build_expr(*expr.func());
    inst->operands.push_back(func_id);
    
    for (auto& arg : expr.args()) {
        auto arg_id = build_expr(*arg);
        inst->operands.push_back(arg_id);
    }
    
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_attr(const ast::AttrExpr& expr) {
    auto obj_id = build_expr(*expr.obj());
    
    auto* inst = current_func_->new_inst(IRInstKind::GETATTR, expr.attr());
    inst->operands.push_back(obj_id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    
    return inst->id;
}

uint32_t IRBuilder::build_list(const ast::ListExpr& expr) {
    auto* inst = current_func_->new_inst(IRInstKind::MAKE_LIST, "list");
    
    for (auto& elem : expr.elems()) {
        auto elem_id = build_expr(*elem);
        inst->operands.push_back(elem_id);
    }
    
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_dict(const ast::DictLiteral& expr) {
    auto* inst = current_func_->new_inst(IRInstKind::MAKE_DICT, "dict");
    
    for (auto& [key_expr, val_expr] : expr.pairs()) {
        auto key_id = build_expr(*key_expr);
        auto val_id = build_expr(*val_expr);
        inst->operands.push_back(key_id);
        inst->operands.push_back(val_id);
    }
    
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_tuple(const ast::TupleExpr& expr) {
    auto* inst = current_func_->new_inst(IRInstKind::MAKE_TUPLE, "tuple");
    
    for (auto& elem : expr.elems()) {
        auto elem_id = build_expr(*elem);
        inst->operands.push_back(elem_id);
    }
    
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    return inst->id;
}

uint32_t IRBuilder::build_subscript(const ast::SubscriptExpr& expr) {
    auto obj_id = build_expr(*expr.obj());
    
    auto* slice = expr.slice();
    if (auto* slice_expr = dynamic_cast<const ast::SliceExpr*>(slice)) {
        // Slice notation: a[start:stop:step]
        auto* inst = current_func_->new_inst(IRInstKind::SLICE_GET, "slice");
        inst->operands.push_back(obj_id);
        
        if (slice_expr->start()) {
            inst->operands.push_back(build_expr(*slice_expr->start()));
        } else {
            auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "slice_start");
            zero->is_const = true;
            zero->const_val = 0.0;
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(zero));
            inst->operands.push_back(zero->id);
        }
        
        if (slice_expr->stop()) {
            inst->operands.push_back(build_expr(*slice_expr->stop()));
        } else {
            auto* none = current_func_->new_inst(IRInstKind::LOADCONST_INT, "slice_stop");
            none->is_const = true;
            none->const_val = -1.0;  // -1 means no limit
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(none));
            inst->operands.push_back(none->id);
        }
        
        if (slice_expr->step()) {
            inst->operands.push_back(build_expr(*slice_expr->step()));
        } else {
            auto* one = current_func_->new_inst(IRInstKind::LOADCONST_INT, "slice_step");
            one->is_const = true;
            one->const_val = 1.0;
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(one));
            inst->operands.push_back(one->id);
        }
        
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
        return inst->id;
    }
    
    // Regular index
    auto slice_id = build_expr(*expr.slice());
    
    auto* inst = current_func_->new_inst(IRInstKind::LIST_GET, "subscript");
    inst->operands.push_back(obj_id);
    inst->operands.push_back(slice_id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inst));
    
    return inst->id;
}

uint32_t IRBuilder::build_lambda_expr(const ast::LambdaExpr& expr) {
    static uint32_t lambda_counter = 0;
    std::string lambda_name = "lambda_" + std::to_string(lambda_counter++);
    
    auto* lambda_fn = new IRFunction();
    lambda_fn->name = lambda_name;
    
    // Collect lambda parameter names
    std::vector<std::string> lambda_param_names;
    for (auto& arg : expr.args()) {
        lambda_fn->param_names.push_back(arg.name);
        lambda_param_names.push_back(arg.name);
        alloc_local(arg.name);
    }
    
    auto* entry_block = lambda_fn->new_block("entry");
    lambda_fn->entry_block_id = entry_block->id;
    
    auto* saved_block = current_block_;
    auto* saved_func = current_func_;
    
    current_func_ = lambda_fn;
    current_block_ = entry_block;
    
    // Build the lambda body
    auto body_val = build_expr(*expr.body());
    
    auto* ret_inst = lambda_fn->new_inst(IRInstKind::RETURN, "lambda_return");
    ret_inst->operands.push_back(body_val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(ret_inst));
    
    module->functions[lambda_name] = lambda_fn;
    module->func_list.push_back(lambda_fn);
    
    current_func_ = saved_func;
    current_block_ = saved_block;
    
    // Create CALL instruction for the lambda
    auto* call_inst = current_func_->new_inst(IRInstKind::CALL, lambda_name);
    
    // Load lambda arguments from enclosing scope's locals
    for (auto& arg : expr.args()) {
        auto it = locals_.find(arg.name);
        if (it != locals_.end()) {
            auto* load = current_func_->new_inst(IRInstKind::LOADLOCAL, arg.name);
            load->operands.push_back(it->second);
            current_block_->instrs.push_back(std::unique_ptr<IRInst>(load));
            call_inst->operands.push_back(load->id);
        }
    }
    
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(call_inst));
    
    return call_inst->id;
}

uint32_t IRBuilder::build_list_comp(const ast::ListComp& expr) {
    // Translate [expr for target in iterable] to:
    // result = []
    // for target in iterable:
    //     result.append(expr)
    // result
    
    auto* result_list = current_func_->new_inst(IRInstKind::MAKE_LIST, "list_comp_result");
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(result_list));
    
    auto result_slot = alloc_local("__list_comp_result__");
    auto* result_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__list_comp_result__");
    result_store->operands.push_back(result_slot);
    result_store->operands.push_back(result_list->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(result_store));
    
    // Create loop blocks
    auto loop_cond_blk = current_func_->new_block("list_comp_cond");
    auto loop_body_blk = current_func_->new_block("list_comp_body");
    auto loop_merge_blk = current_func_->new_block("list_comp_merge");
    
    loop_cond_blk->successors.push_back(loop_body_blk->id);
    loop_cond_blk->successors.push_back(loop_merge_blk->id);
    loop_body_blk->successors.push_back(loop_cond_blk->id);
    
    loop_stack_.push_back({loop_cond_blk->id, loop_merge_blk->id});
    
    // Load iterable
    auto iter_val = build_expr(*expr.comprehensions_[0].iterable);
    auto iter_slot = alloc_local("__list_comp_iter__");
    auto* iter_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__list_comp_iter__");
    iter_store->operands.push_back(iter_slot);
    iter_store->operands.push_back(iter_val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(iter_store));
    
    current_block_ = loop_cond_blk;
    
    // Index-based iteration
    auto index_slot = alloc_local("__list_comp_index__");
    auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "zero");
    zero->is_const = true;
    set_const_double(zero->const_val, 0.0);
    auto* index_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__list_comp_index__");
    index_store->operands.push_back(index_slot);
    index_store->operands.push_back(zero->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(index_store));
    
    auto* index_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "idx_load");
    index_load->operands.push_back(index_slot);
    
    auto* iter_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "iter_load");
    iter_load->operands.push_back(iter_slot);
    
    auto* len_inst = current_func_->new_inst(IRInstKind::INTRINSIC_LEN, "list_len");
    len_inst->operands.push_back(iter_load->id);
    
    auto* cmp_inst = current_func_->new_inst(IRInstKind::LT, "idx_lt_len");
    cmp_inst->operands.push_back(index_load->id);
    cmp_inst->operands.push_back(len_inst->id);
    
    auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "list_comp_branch");
    branch_inst->operands.push_back(cmp_inst->id);
    branch_inst->operands.push_back(loop_body_blk->id);
    branch_inst->operands.push_back(loop_merge_blk->id);
    current_block_->successors.push_back(loop_body_blk->id);
    current_block_->successors.push_back(loop_merge_blk->id);
    
    current_block_ = loop_body_blk;
    
    // Get element from iterable
    auto* get_elem = current_func_->new_inst(IRInstKind::LIST_GET, "get_elem");
    get_elem->operands.push_back(iter_load->id);
    get_elem->operands.push_back(index_load->id);
    
    // Store target variable
    auto target_slot = alloc_local(expr.comprehensions_[0].target);
    auto* target_store = current_func_->new_inst(IRInstKind::STORELOCAL, expr.comprehensions_[0].target);
    target_store->operands.push_back(target_slot);
    target_store->operands.push_back(get_elem->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(target_store));
    
    // Evaluate expression
    auto expr_val = build_expr(*expr.elt());
    
    // Build append call: result.append(elem)
    auto* append_call = current_func_->new_inst(IRInstKind::CALL, "append");
    auto* result_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "result_load");
    result_load->operands.push_back(result_slot);
    append_call->operands.push_back(result_load->id);
    append_call->operands.push_back(get_elem->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(append_call));
    
    // Increment index
    auto* one = current_func_->new_inst(IRInstKind::LOADCONST_INT, "one");
    one->is_const = true;
    set_const_double(one->const_val, 1.0);
    auto* add_inst = current_func_->new_inst(IRInstKind::ADD, "idx_inc");
    add_inst->operands.push_back(index_load->id);
    add_inst->operands.push_back(one->id);
    auto* inc_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__list_comp_index__");
    inc_store->operands.push_back(index_slot);
    inc_store->operands.push_back(add_inst->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inc_store));
    
    auto* jump_inst = current_func_->new_inst(IRInstKind::JUMP, "list_comp_jump");
    jump_inst->operands.push_back(loop_cond_blk->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump_inst));
    
    loop_stack_.pop_back();
    current_block_ = loop_merge_blk;
    
    // Load result list
    auto* final_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "final_result");
    final_load->operands.push_back(result_slot);
    
    return final_load->id;
}

uint32_t IRBuilder::build_set_comp(const ast::SetComp& expr) {
    // Translate {expr for target in iterable} to:
    // result = []
    // for target in iterable:
    //     result.append(expr)
    // result (returned as list, sets not fully supported)
    
    auto* result_list = current_func_->new_inst(IRInstKind::MAKE_LIST, "set_comp_result");
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(result_list));
    
    auto result_slot = alloc_local("__set_comp_result__");
    auto* result_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__set_comp_result__");
    result_store->operands.push_back(result_slot);
    result_store->operands.push_back(result_list->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(result_store));
    
    auto loop_cond_blk = current_func_->new_block("set_comp_cond");
    auto loop_body_blk = current_func_->new_block("set_comp_body");
    auto loop_merge_blk = current_func_->new_block("set_comp_merge");
    
    loop_cond_blk->successors.push_back(loop_body_blk->id);
    loop_cond_blk->successors.push_back(loop_merge_blk->id);
    loop_body_blk->successors.push_back(loop_cond_blk->id);
    
    loop_stack_.push_back({loop_cond_blk->id, loop_merge_blk->id});
    
    auto iter_val = build_expr(*expr.comprehensions_[0].iterable);
    auto iter_slot = alloc_local("__set_comp_iter__");
    auto* iter_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__set_comp_iter__");
    iter_store->operands.push_back(iter_slot);
    iter_store->operands.push_back(iter_val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(iter_store));
    
    current_block_ = loop_cond_blk;
    
    auto index_slot = alloc_local("__set_comp_index__");
    auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "zero");
    zero->is_const = true;
    set_const_double(zero->const_val, 0.0);
    auto* index_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__set_comp_index__");
    index_store->operands.push_back(index_slot);
    index_store->operands.push_back(zero->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(index_store));
    
    auto* index_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "idx_load");
    index_load->operands.push_back(index_slot);
    
    auto* iter_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "iter_load");
    iter_load->operands.push_back(iter_slot);
    
    auto* len_inst = current_func_->new_inst(IRInstKind::INTRINSIC_LEN, "list_len");
    len_inst->operands.push_back(iter_load->id);
    
    auto* cmp_inst = current_func_->new_inst(IRInstKind::LT, "idx_lt_len");
    cmp_inst->operands.push_back(index_load->id);
    cmp_inst->operands.push_back(len_inst->id);
    
    auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "set_comp_branch");
    branch_inst->operands.push_back(cmp_inst->id);
    branch_inst->operands.push_back(loop_body_blk->id);
    branch_inst->operands.push_back(loop_merge_blk->id);
    current_block_->successors.push_back(loop_body_blk->id);
    current_block_->successors.push_back(loop_merge_blk->id);
    
    current_block_ = loop_body_blk;
    
    auto* get_elem = current_func_->new_inst(IRInstKind::LIST_GET, "get_elem");
    get_elem->operands.push_back(iter_load->id);
    get_elem->operands.push_back(index_load->id);
    
    auto target_slot = alloc_local(expr.comprehensions_[0].target);
    auto* target_store = current_func_->new_inst(IRInstKind::STORELOCAL, expr.comprehensions_[0].target);
    target_store->operands.push_back(target_slot);
    target_store->operands.push_back(get_elem->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(target_store));
    
    auto expr_val = build_expr(*expr.elt());
    
    auto* append_call = current_func_->new_inst(IRInstKind::CALL, "append");
    auto* result_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "result_load");
    result_load->operands.push_back(result_slot);
    append_call->operands.push_back(result_load->id);
    append_call->operands.push_back(get_elem->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(append_call));
    
    auto* one = current_func_->new_inst(IRInstKind::LOADCONST_INT, "one");
    one->is_const = true;
    set_const_double(one->const_val, 1.0);
    auto* add_inst = current_func_->new_inst(IRInstKind::ADD, "idx_inc");
    add_inst->operands.push_back(index_load->id);
    add_inst->operands.push_back(one->id);
    auto* inc_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__set_comp_index__");
    inc_store->operands.push_back(index_slot);
    inc_store->operands.push_back(add_inst->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inc_store));
    
    auto* jump_inst = current_func_->new_inst(IRInstKind::JUMP, "set_comp_jump");
    jump_inst->operands.push_back(loop_cond_blk->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump_inst));
    
    loop_stack_.pop_back();
    current_block_ = loop_merge_blk;
    
    auto* final_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "final_result");
    final_load->operands.push_back(result_slot);
    
    return final_load->id;
}

uint32_t IRBuilder::build_gen_expr(const ast::GenExpr& expr) {
    // Translate (expr for target in iterable) to list (generators not fully supported)
    // Same implementation as list comp but with different naming
    
    auto* result_list = current_func_->new_inst(IRInstKind::MAKE_LIST, "gen_expr_result");
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(result_list));
    
    auto result_slot = alloc_local("__gen_expr_result__");
    auto* result_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__gen_expr_result__");
    result_store->operands.push_back(result_slot);
    result_store->operands.push_back(result_list->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(result_store));
    
    auto loop_cond_blk = current_func_->new_block("gen_expr_cond");
    auto loop_body_blk = current_func_->new_block("gen_expr_body");
    auto loop_merge_blk = current_func_->new_block("gen_expr_merge");
    
    loop_cond_blk->successors.push_back(loop_body_blk->id);
    loop_cond_blk->successors.push_back(loop_merge_blk->id);
    loop_body_blk->successors.push_back(loop_cond_blk->id);
    
    loop_stack_.push_back({loop_cond_blk->id, loop_merge_blk->id});
    
    auto iter_val = build_expr(*expr.comprehensions_[0].iterable);
    auto iter_slot = alloc_local("__gen_expr_iter__");
    auto* iter_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__gen_expr_iter__");
    iter_store->operands.push_back(iter_slot);
    iter_store->operands.push_back(iter_val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(iter_store));
    
    current_block_ = loop_cond_blk;
    
    auto index_slot = alloc_local("__gen_expr_index__");
    auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "zero");
    zero->is_const = true;
    set_const_double(zero->const_val, 0.0);
    auto* index_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__gen_expr_index__");
    index_store->operands.push_back(index_slot);
    index_store->operands.push_back(zero->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(index_store));
    
    auto* index_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "idx_load");
    index_load->operands.push_back(index_slot);
    
    auto* iter_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "iter_load");
    iter_load->operands.push_back(iter_slot);
    
    auto* len_inst = current_func_->new_inst(IRInstKind::INTRINSIC_LEN, "list_len");
    len_inst->operands.push_back(iter_load->id);
    
    auto* cmp_inst = current_func_->new_inst(IRInstKind::LT, "idx_lt_len");
    cmp_inst->operands.push_back(index_load->id);
    cmp_inst->operands.push_back(len_inst->id);
    
    auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "gen_expr_branch");
    branch_inst->operands.push_back(cmp_inst->id);
    branch_inst->operands.push_back(loop_body_blk->id);
    branch_inst->operands.push_back(loop_merge_blk->id);
    current_block_->successors.push_back(loop_body_blk->id);
    current_block_->successors.push_back(loop_merge_blk->id);
    
    current_block_ = loop_body_blk;
    
    auto* get_elem = current_func_->new_inst(IRInstKind::LIST_GET, "get_elem");
    get_elem->operands.push_back(iter_load->id);
    get_elem->operands.push_back(index_load->id);
    
    auto target_slot = alloc_local(expr.comprehensions_[0].target);
    auto* target_store = current_func_->new_inst(IRInstKind::STORELOCAL, expr.comprehensions_[0].target);
    target_store->operands.push_back(target_slot);
    target_store->operands.push_back(get_elem->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(target_store));
    
    auto expr_val = build_expr(*expr.elt());
    
    auto* append_call = current_func_->new_inst(IRInstKind::CALL, "append");
    auto* result_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "result_load");
    result_load->operands.push_back(result_slot);
    append_call->operands.push_back(result_load->id);
    append_call->operands.push_back(get_elem->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(append_call));
    
    auto* one = current_func_->new_inst(IRInstKind::LOADCONST_INT, "one");
    one->is_const = true;
    set_const_double(one->const_val, 1.0);
    auto* add_inst = current_func_->new_inst(IRInstKind::ADD, "idx_inc");
    add_inst->operands.push_back(index_load->id);
    add_inst->operands.push_back(one->id);
    auto* inc_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__gen_expr_index__");
    inc_store->operands.push_back(index_slot);
    inc_store->operands.push_back(add_inst->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inc_store));
    
    auto* jump_inst = current_func_->new_inst(IRInstKind::JUMP, "gen_expr_jump");
    jump_inst->operands.push_back(loop_cond_blk->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump_inst));
    
    loop_stack_.pop_back();
    current_block_ = loop_merge_blk;
    
    auto* final_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "final_result");
    final_load->operands.push_back(result_slot);
    
    return final_load->id;
}

uint32_t IRBuilder::build_dict_comp(const ast::DictComp& expr) {
    // Translate {k: v for target in iterable} to:
    // result = {}
    // for target in iterable:
    //     result[key] = value
    // result
    
    auto* result_dict = current_func_->new_inst(IRInstKind::MAKE_LIST, "dict_comp_result");
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(result_dict));
    
    auto result_slot = alloc_local("__dict_comp_result__");
    auto* result_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__dict_comp_result__");
    result_store->operands.push_back(result_slot);
    result_store->operands.push_back(result_dict->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(result_store));
    
    auto loop_cond_blk = current_func_->new_block("dict_comp_cond");
    auto loop_body_blk = current_func_->new_block("dict_comp_body");
    auto loop_merge_blk = current_func_->new_block("dict_comp_merge");
    
    loop_cond_blk->successors.push_back(loop_body_blk->id);
    loop_cond_blk->successors.push_back(loop_merge_blk->id);
    loop_body_blk->successors.push_back(loop_cond_blk->id);
    
    loop_stack_.push_back({loop_cond_blk->id, loop_merge_blk->id});
    
    auto iter_val = build_expr(*expr.comprehensions_[0].iterable);
    auto iter_slot = alloc_local("__dict_comp_iter__");
    auto* iter_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__dict_comp_iter__");
    iter_store->operands.push_back(iter_slot);
    iter_store->operands.push_back(iter_val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(iter_store));
    
    current_block_ = loop_cond_blk;
    
    auto index_slot = alloc_local("__dict_comp_index__");
    auto* zero = current_func_->new_inst(IRInstKind::LOADCONST_INT, "zero");
    zero->is_const = true;
    set_const_double(zero->const_val, 0.0);
    auto* index_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__dict_comp_index__");
    index_store->operands.push_back(index_slot);
    index_store->operands.push_back(zero->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(index_store));
    
    auto* index_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "idx_load");
    index_load->operands.push_back(index_slot);
    
    auto* iter_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "iter_load");
    iter_load->operands.push_back(iter_slot);
    
    auto* len_inst = current_func_->new_inst(IRInstKind::INTRINSIC_LEN, "list_len");
    len_inst->operands.push_back(iter_load->id);
    
    auto* cmp_inst = current_func_->new_inst(IRInstKind::LT, "idx_lt_len");
    cmp_inst->operands.push_back(index_load->id);
    cmp_inst->operands.push_back(len_inst->id);
    
    auto* branch_inst = current_func_->new_inst(IRInstKind::BRANCH, "dict_comp_branch");
    branch_inst->operands.push_back(cmp_inst->id);
    branch_inst->operands.push_back(loop_body_blk->id);
    branch_inst->operands.push_back(loop_merge_blk->id);
    current_block_->successors.push_back(loop_body_blk->id);
    current_block_->successors.push_back(loop_merge_blk->id);
    
    current_block_ = loop_body_blk;
    
    auto* get_elem = current_func_->new_inst(IRInstKind::LIST_GET, "get_elem");
    get_elem->operands.push_back(iter_load->id);
    get_elem->operands.push_back(index_load->id);
    
    auto target_slot = alloc_local(expr.comprehensions_[0].target);
    auto* target_store = current_func_->new_inst(IRInstKind::STORELOCAL, expr.comprehensions_[0].target);
    target_store->operands.push_back(target_slot);
    target_store->operands.push_back(get_elem->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(target_store));
    
    auto key_val = build_expr(*expr.key());
    auto val_val = build_expr(*expr.value());
    
    auto* setitem_call = current_func_->new_inst(IRInstKind::CALL, "setitem");
    auto* result_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "result_load");
    result_load->operands.push_back(result_slot);
    setitem_call->operands.push_back(result_load->id);
    setitem_call->operands.push_back(key_val);
    setitem_call->operands.push_back(val_val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(setitem_call));
    
    auto* one = current_func_->new_inst(IRInstKind::LOADCONST_INT, "one");
    one->is_const = true;
    set_const_double(one->const_val, 1.0);
    auto* add_inst = current_func_->new_inst(IRInstKind::ADD, "idx_inc");
    add_inst->operands.push_back(index_load->id);
    add_inst->operands.push_back(one->id);
    auto* inc_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__dict_comp_index__");
    inc_store->operands.push_back(index_slot);
    inc_store->operands.push_back(add_inst->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(inc_store));
    
    auto* jump_inst = current_func_->new_inst(IRInstKind::JUMP, "dict_comp_jump");
    jump_inst->operands.push_back(loop_cond_blk->id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(jump_inst));
    
    loop_stack_.pop_back();
    current_block_ = loop_merge_blk;
    
    auto* final_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "final_result");
    final_load->operands.push_back(result_slot);
    
    return final_load->id;
}

uint32_t IRBuilder::build_formatted_value(const ast::FormattedValue& expr) {
    auto expr_id = build_expr(*expr.expr());
    
    // Convert value to string if needed
    auto* str_call = current_func_->new_inst(IRInstKind::CALL, "str");
    str_call->operands.push_back(expr_id);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(str_call));
    
    return str_call->id;
}

uint32_t IRBuilder::build_joined_str(const ast::JoinedStr& expr) {
    // Build f-string by concatenating all parts
    if (expr.parts().empty()) {
        auto* empty_str = current_func_->new_inst(IRInstKind::LOADCONST_STR, "empty");
        empty_str->is_const = true;
        empty_str->const_val = "";
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(empty_str));
        return empty_str->id;
    }
    
    // Start with first part
    uint32_t result_id = build_expr(*expr.parts()[0]);
    
     // Concatenate remaining parts
    for (size_t i = 1; i < expr.parts().size(); i++) {
        auto* concat = current_func_->new_inst(IRInstKind::CALL, "pyc_str_concat");
        auto* left_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "concat_left");
        auto left_slot = alloc_local("__concat_left__");
        auto* left_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__concat_left__");
        left_store->operands.push_back(left_slot);
        left_store->operands.push_back(result_id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(left_store));
        left_load->operands.push_back(left_slot);
        concat->operands.push_back(left_load->id);
        
        auto right_id = build_expr(*expr.parts()[i]);
        auto* right_load = current_func_->new_inst(IRInstKind::LOADLOCAL, "concat_right");
        auto right_slot = alloc_local("__concat_right__");
        auto* right_store = current_func_->new_inst(IRInstKind::STORELOCAL, "__concat_right__");
        right_store->operands.push_back(right_slot);
        right_store->operands.push_back(right_id);
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(right_store));
        right_load->operands.push_back(right_slot);
        concat->operands.push_back(right_load->id);
        
        current_block_->instrs.push_back(std::unique_ptr<IRInst>(concat));
        result_id = concat->id;
    }
    
    return result_id;
}

uint32_t IRBuilder::build_yield(const ast::YieldExpr& expr) {
    uint32_t val = UINT32_MAX;
    if (expr.value()) {
        val = build_expr(*expr.value());
    }
    
    auto* yield_inst = current_func_->new_inst(IRInstKind::CALL, "pyc_yield");
    if (val != UINT32_MAX) {
        yield_inst->operands.push_back(val);
    }
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(yield_inst));
    
    return yield_inst->id;
}

uint32_t IRBuilder::build_await(const ast::AwaitExpr& expr) {
    // Simplified: treat await as a regular call
    auto* await_inst = current_func_->new_inst(IRInstKind::CALL, "pyc_await");
    auto val = build_expr(*expr.value());
    await_inst->operands.push_back(val);
    current_block_->instrs.push_back(std::unique_ptr<IRInst>(await_inst));
    
    return await_inst->id;
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
