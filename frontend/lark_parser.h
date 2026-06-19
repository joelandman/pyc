#pragma once
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <functional>
#include "frontend/ast.h"

// Minimal JSON AST types (no Lark dependency)
namespace json_ast {
    struct Value {
        std::string type;       // node type (e.g., "module", "stmt_func")
        std::string value;      // leaf value (for tokens/literals)
        std::vector<std::shared_ptr<Value>> children;  // nested nodes

        Value() = default;
        explicit Value(std::string v) : type(std::move(v)) {}
    };
}

namespace pyc::parser {

// Bring Value into parser namespace
using Value = json_ast::Value;

class LarkParser {
public:
    // Parse source code and return AST
    std::shared_ptr<ast::Module> parse(const std::string& source);
    std::string get_error(const std::string& source, int line, int column) const;

private:
    using ParsedResult = struct {
        std::shared_ptr<ast::Module> module;
        int error_line = 0;
        int error_column = 0;
        std::string error_type = "";
        std::string error_message = "";
    };

    ParsedResult parse_with_lark(const std::string& source);
    struct ExceptClause {
        std::shared_ptr<ast::Expr> type;
        std::shared_ptr<ast::Expr> value;
        std::vector<std::shared_ptr<ast::Stmt>> body;
        bool is_star = false;
        std::string var_name;
    };
    struct TryStmt : public ast::Stmt {
        std::vector<std::shared_ptr<ast::Stmt>> body;
        std::vector<ExceptClause> handlers;
        std::vector<std::shared_ptr<ast::Stmt>> orelse;
        std::vector<std::shared_ptr<ast::Stmt>> finalbody;
    };
    struct MatchStmt : public ast::Stmt {
        std::shared_ptr<ast::Expr> subject;
        struct Case {
            std::vector<std::shared_ptr<ast::Expr>> patterns;
            std::shared_ptr<ast::Expr> guard;
            std::vector<std::shared_ptr<ast::Stmt>> body;
        };
        std::vector<Case> cases;
    };
    struct ImportFrom {
        std::string module_name;
        std::vector<std::pair<std::string, std::string>> names;
        int level = 0;
    };
    struct ImportStmt : public ast::Stmt {
        std::vector<std::string> modules;
        std::shared_ptr<ImportFrom> from_import;
    };
    struct WithItem {
        std::shared_ptr<ast::Expr> context;
        std::string optional_vars;
    };
    struct WithStmt : public ast::Stmt {
        std::vector<WithItem> items;
        std::vector<std::shared_ptr<ast::Stmt>> body;
    };
    struct RaiseStmt : public ast::Stmt {
        std::shared_ptr<ast::Expr> exc;
        std::shared_ptr<ast::Expr> cause;
    };
    struct LambdaExpr : public ast::Expr {
        struct Arg {
            std::string name;
        };
        std::vector<Arg> args;
        std::shared_ptr<ast::Expr> body;
    };
    struct ListComp : public ast::Expr {
        struct Comprehension {
            std::string target;
            std::shared_ptr<ast::Expr> iterable;
            std::vector<std::shared_ptr<ast::Expr>> ifs;
        };
        std::shared_ptr<ast::Expr> elt;
        std::vector<Comprehension> comprehensions;
    };
    struct DeleteStmt : public ast::Stmt {
        std::vector<std::string> targets;
    };
    struct GlobalStmt : public ast::Stmt {
        std::vector<std::string> names;
    };
    struct NonlocalStmt : public ast::Stmt {
        std::vector<std::string> names;
    };
    struct AssertStmt : public ast::Stmt {
        std::shared_ptr<ast::Expr> test;
        std::shared_ptr<ast::Expr> msg;
    };
    
    // Expression visitors (JSON parsing via lark_bridge.py)
    std::shared_ptr<ast::Expr> visit_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_test(const Value& node);
    std::shared_ptr<ast::Expr> visit_or_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_and_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_not_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_comparison(const Value& node);
    std::shared_ptr<ast::Expr> visit_bitwise_or(const Value& node);
    std::shared_ptr<ast::Expr> visit_xor_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_shift_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_add_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_mul_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_power_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_unary_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_primary_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_atom(const Value& node);
    std::shared_ptr<ast::Expr> visit_arglist(const Value& node);
    std::shared_ptr<ast::Expr> visit_list_comprehension(const Value& node);
    std::shared_ptr<ast::Expr> visit_comp_for(const Value& node);
    std::shared_ptr<ast::Expr> visit_lambda_expr(const Value& node);
    std::shared_ptr<ast::Expr> visit_subscript(const Value& node);
    std::shared_ptr<ast::Expr> visit_dict_or_set_literal(const Value& node);
    std::shared_ptr<ast::Expr> visit_list_literal(const Value& node);
    std::shared_ptr<ast::Expr> visit_argument(const Value& node);
    std::shared_ptr<ast::Expr> visit_testlist(const Value& node);
    std::shared_ptr<ast::Expr> visit_exprlist(const Value& node);
    
    // Statement visitors (JSON parsing via lark_bridge.py)
    std::shared_ptr<ast::Stmt> visit_stmt(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_func(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_class(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_if(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_for(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_while(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_try(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_with(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_match(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_import(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_assign(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_augassign(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_return(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_raise(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_delete(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_global(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_nonlocal(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_assert(const Value& node);
    std::shared_ptr<ast::Stmt> visit_stmt_expr(const Value& node);
    
    // Helper functions
    std::shared_ptr<ast::BinOpExpr> create_binop(ast::BinOpExpr::Op op, 
                                                    std::shared_ptr<ast::Expr> left,
                                                    std::shared_ptr<ast::Expr> right);
};

} // namespace pyc::parser
