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
    std::string module_name_;

    IRBuilder() : module(std::make_unique<IRModule>()) {}

    // Build an IRModule from an AST Module
    void build(const ast::Module& mod);
    void build(const ast::Module& mod, const std::string& mod_name);

private:
    std::unordered_map<std::string, uint32_t> locals_;
    std::string current_scope_;  // function name
    IRFunction* current_func_ = nullptr;
    IRBlock* current_block_ = nullptr;
    
    // Loop context for break/continue
    struct LoopContext {
        uint32_t cond_block;
        uint32_t merge_block;
    };
    std::vector<LoopContext> loop_stack_;

    void build_function(const ast::FunctionDef& func);
    void build_class(const ast::ClassDef& cls);
    void build_stmt(const ast::Stmt& stmt);
    void build_if_stmt(const ast::IfStmt& ifs);
    void build_for_stmt(const ast::ForStmt& fs);
    void build_while_stmt(const ast::WhileStmt& ws);
    void build_assign_stmt(const ast::AssignStmt& as);
    void build_tuple_assign_stmt(const ast::TupleAssignStmt& tas);
    void build_return_stmt(const ast::ReturnStmt& ret);
    uint32_t build_named_expr(const ast::NamedExpr& ne);
    void build_augassign_stmt(const ast::AugAssignStmt& aug);
    void build_import_stmt(const ast::ImportStmt& imp);
    void build_class_call(const ast::CallExpr& call, const std::string& class_name);
    
    void build_delete_stmt(const ast::DeleteStmt& del);
    void build_global_stmt(const ast::GlobalStmt& gl);
    void build_nonlocal_stmt(const ast::NonlocalStmt& nt);
    void build_assert_stmt(const ast::AssertStmt& asp);
    void build_raise_stmt(const ast::RaiseStmt& ra);
    void build_with_stmt(const ast::WithStmt& wt);
    void build_try_stmt(const ast::TryStmt& tr);
    void build_match_stmt(const ast::MatchStmt& ms);
    void build_break_stmt();
    void build_continue_stmt();
    
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
    uint32_t build_dict(const ast::DictLiteral& expr);
    uint32_t build_tuple(const ast::TupleExpr& expr);
    uint32_t build_subscript(const ast::SubscriptExpr& expr);
    uint32_t build_lambda_expr(const ast::LambdaExpr& expr);
    uint32_t build_list_comp(const ast::ListComp& expr);
    uint32_t build_set_comp(const ast::SetComp& expr);
    uint32_t build_gen_expr(const ast::GenExpr& expr);
    uint32_t build_dict_comp(const ast::DictComp& expr);
    uint32_t build_joined_str(const ast::JoinedStr& expr);
    uint32_t build_formatted_value(const ast::FormattedValue& expr);
    uint32_t build_yield(const ast::YieldExpr& expr);
    uint32_t build_await(const ast::AwaitExpr& expr);

    IRInstKind augment_to_binop(ast::AugAssignStmt::Op op);

    uint32_t alloc_local(std::string name);
    void store_local(uint32_t local_var, uint32_t value);
    uint32_t load_local(uint32_t local_var);
    uint32_t load_global(std::string name);
    void store_global(std::string name, uint32_t value);
};

} // namespace pyc::ir::builder
