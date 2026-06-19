#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include "ir/ir.h"
#include "frontend/ast.h"

namespace pyc::ir::builder {

class IRBuilder {
public:
    std::unique_ptr<IRModule> module;

    IRBuilder() : module(std::make_unique<IRModule>()) {}

    // Build an IRModule from an AST Module
    void build(const ast::Module& mod);

private:
    std::unordered_map<std::string, uint32_t> locals_;
    std::string current_scope_;  // function name
    IRFunction* current_func_ = nullptr;
    IRBlock* current_block_ = nullptr;

    void build_function(const ast::FunctionDef& func);
    void build_class(const ast::ClassDef& cls);
    void build_stmt(const ast::Stmt& stmt);
    void build_if_stmt(const ast::IfStmt& ifs);
    void build_for_stmt(const ast::ForStmt& fs);
    void build_while_stmt(const ast::WhileStmt& ws);
    void build_assign_stmt(const ast::AssignStmt& as);
    void build_return_stmt(const ast::ReturnStmt& ret);
    void build_augassign_stmt(const ast::AugAssignStmt& aug);
    void build_import_stmt(const ast::ImportStmt& imp);
    
    uint32_t build_expr(const ast::Expr& expr);
    uint32_t build_int_literal(const ast::IntLiteral& lit);
    uint32_t build_float_literal(const ast::FloatLiteral& lit);
    uint32_t build_str_literal(const ast::StrLiteral& lit);
    uint32_t build_bool_literal(const ast::BoolLiteral& lit);
    uint32_t build_ellipsis_literal(const ast::EllipsisLiteral& lit);
    uint32_t build_name(const ast::Name& name);
    uint32_t build_binop(const ast::BinOpExpr& expr);
    uint32_t build_unary(const ast::UnaryOpExpr& expr);
    uint32_t build_call(const ast::CallExpr& expr);
    uint32_t build_attr(const ast::AttrExpr& expr);
    uint32_t build_list(const ast::ListExpr& expr);

    IRInstKind augment_to_binop(ast::AugAssignStmt::Op op);

    uint32_t alloc_local(std::string name);
    void store_local(uint32_t local_var, uint32_t value);
    uint32_t load_local(uint32_t local_var);
    uint32_t load_global(std::string name);
    void store_global(std::string name, uint32_t value);
};

} // namespace pyc::ir::builder
