#pragma once
#include <string>
#include <vector>
#include <memory>
#include "frontend/ast.h"
#include "frontend/lexer.h"

namespace pyc::parser {

class Parser {
    const std::vector<pyc::lexer::Token>& tokens_;
    size_t pos_ = 0;

public:
    explicit Parser(const std::vector<pyc::lexer::Token>& tokens) : tokens_(tokens) {}

    std::shared_ptr<ast::Module> parse();
    std::vector<std::shared_ptr<ast::Stmt>> parse_stmt_list();
    std::shared_ptr<ast::Stmt> parse_stmt();
    std::shared_ptr<ast::Stmt> parse_assign_or_expr();
    std::shared_ptr<ast::Expr> parse_expr();

private:
    std::shared_ptr<ast::Stmt> parse_if_stmt();
    std::shared_ptr<ast::Stmt> parse_for_stmt();
    std::shared_ptr<ast::Stmt> parse_while_stmt();
    std::shared_ptr<ast::Stmt> parse_function_def();
    std::shared_ptr<ast::Stmt> parse_class_def();
    std::shared_ptr<ast::Stmt> parse_try_stmt();
    std::shared_ptr<ast::Stmt> parse_match_stmt();
    std::vector<std::shared_ptr<ast::Stmt>> parse_block();

    std::shared_ptr<ast::Expr> parse_or_expr();
    std::shared_ptr<ast::Expr> parse_and_expr();
    std::shared_ptr<ast::Expr> parse_not_expr();
    std::shared_ptr<ast::Expr> parse_comparison_expr();
    std::shared_ptr<ast::Expr> parse_add_expr();
    std::shared_ptr<ast::Expr> parse_mul_expr();
    std::shared_ptr<ast::Expr> parse_unary_expr();
    std::shared_ptr<ast::Expr> parse_primary_expr();

    bool has_more() const;
    const pyc::lexer::Token& current();
    void advance();
    void expect(pyc::lexer::TokenType type);
};

} // namespace pyc::parser
