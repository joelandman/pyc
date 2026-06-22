// frontend/parser.cpp - Recursive descent Python parser
// Pure C++ implementation - no external dependencies

#include "frontend/parser.h"
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace pyc::parser {

// ===== Helper Functions =====

static bool is_keyword(const std::string& name) {
    return name == "def" || name == "class" || name == "if" || name == "else" ||
           name == "elif" || name == "for" || name == "while" || name == "return" ||
           name == "break" || name == "continue" || name == "pass" || name == "try" ||
           name == "except" || name == "finally" || name == "with" || name == "as" ||
           name == "import" || name == "from" || name == "global" || name == "nonlocal" ||
           name == "assert" || name == "raise" || name == "del" || name == "lambda" ||
           name == "yield" || name == "async" || name == "await" || name == "match" ||
           name == "case";
}

static bool is_builtin(const std::string& name) {
    return name == "True" || name == "False" || name == "None" ||
           name == "print" || name == "len" || name == "range" || name == "int" ||
           name == "float" || name == "str" || name == "list" || name == "dict" ||
           name == "type" || name == "isinstance" || name == "input" ||
           name == "abs" || name == "max" || name == "min" || name == "sum" ||
           name == "sorted" || name == "reversed" || name == "enumerate" ||
           name == "zip" || name == "map" || name == "filter";
}

// ===== Parser Implementation =====

std::shared_ptr<ast::Module> Parser::parse() {
    auto stmts = parse_stmt_list();
    auto mod = std::make_shared<ast::Module>(std::move(stmts));
    mod->classify_funcs_and_classes();
    return mod;
}

std::vector<std::shared_ptr<ast::Stmt>> Parser::parse_stmt_list() {
    std::vector<std::shared_ptr<ast::Stmt>> stmts;
    while (has_more()) {
        stmts.push_back(parse_stmt());
        if (!has_more()) break;
    }
    return stmts;
}

std::shared_ptr<ast::Stmt> Parser::parse_stmt() {
    if (!has_more()) return std::make_shared<ast::PassStmt>();

    const auto& tok = current();

    // pass
    if (tok.kind == pyc::lexer::TokenType::PASS) {
        advance();
        return std::make_shared<ast::PassStmt>();
    }

    // break
    if (tok.kind == pyc::lexer::TokenType::BREAK) {
        advance();
        return std::make_shared<ast::BreakStmt>();
    }

    // continue
    if (tok.kind == pyc::lexer::TokenType::CONTINUE) {
        advance();
        return std::make_shared<ast::ContinueStmt>();
    }

    // return
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "return") {
        advance();
        std::shared_ptr<ast::Expr> val;
        if (has_more() && current().kind != pyc::lexer::TokenType::NEWLINE && current().kind != pyc::lexer::TokenType::DECREMENT && current().kind != pyc::lexer::TokenType::EOF_) {
            val = parse_expr();
        }
        return std::make_shared<ast::ReturnStmt>(std::move(val));
    }

    // if
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "if") {
        return parse_if_stmt();
    }

    // for
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "for") {
        return parse_for_stmt();
    }

    // while
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "while") {
        return parse_while_stmt();
    }

    // class
    if (tok.kind == pyc::lexer::TokenType::CLASS) {
        advance();
        return parse_class_def();
    }

    // def
    if (tok.kind == pyc::lexer::TokenType::DEF) {
        advance();
        return parse_function_def();
    }

    // import
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "import") {
        advance();
        std::vector<std::string> modules;
        while (has_more() && current().kind != pyc::lexer::TokenType::NEWLINE && current().kind != pyc::lexer::TokenType::DECREMENT) {
            if (current().kind == pyc::lexer::TokenType::NAME) {
                modules.push_back(current().value);
                advance();
            } else {
                advance();
            }
        }
        auto imp = std::make_shared<ast::ImportFrom>("", 0);
        imp->names_ = modules;  // Store all module names in names_
        return std::make_shared<ast::ImportStmt>(imp);
    }

    // from ... import ...
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "from") {
        advance();
        std::string module;
        if (has_more() && current().kind == pyc::lexer::TokenType::NAME) {
            module = current().value;
            advance();
        }
        if (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "import") {
            advance();
        }
        auto imp = std::make_shared<ast::ImportFrom>(module, 0);
        return std::make_shared<ast::ImportStmt>(imp);
    }

    // global
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "global") {
        advance();
        std::vector<std::string> names;
        while (has_more() && current().kind != pyc::lexer::TokenType::NEWLINE && current().kind != pyc::lexer::TokenType::DECREMENT) {
            if (current().kind == pyc::lexer::TokenType::NAME) {
                names.push_back(current().value);
                advance();
            } else {
                advance();
            }
        }
        return std::make_shared<ast::GlobalStmt>(names);
    }

    // nonlocal
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "nonlocal") {
        advance();
        std::vector<std::string> names;
        while (has_more() && current().kind != pyc::lexer::TokenType::NEWLINE && current().kind != pyc::lexer::TokenType::DECREMENT) {
            if (current().kind == pyc::lexer::TokenType::NAME) {
                names.push_back(current().value);
                advance();
            } else {
                advance();
            }
        }
        return std::make_shared<ast::NonlocalStmt>(names);
    }

    // assert
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "assert") {
        advance();
        auto test = parse_expr();
        std::shared_ptr<ast::Expr> msg;
        if (has_more() && current().kind != pyc::lexer::TokenType::NEWLINE && current().kind != pyc::lexer::TokenType::DECREMENT) {
            if (current().kind == pyc::lexer::TokenType::COMMA) advance();
            msg = parse_expr();
        }
        return std::make_shared<ast::AssertStmt>(test, msg);
    }

    // raise
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "raise") {
        advance();
        std::shared_ptr<ast::Expr> exc;
        if (has_more() && current().kind != pyc::lexer::TokenType::NEWLINE && current().kind != pyc::lexer::TokenType::DECREMENT) {
            exc = parse_expr();
        }
        return std::make_shared<ast::RaiseStmt>(exc, nullptr);
    }

    // del
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "del") {
        advance();
        std::vector<std::string> targets;
        while (has_more() && current().kind != pyc::lexer::TokenType::NEWLINE && current().kind != pyc::lexer::TokenType::DECREMENT) {
            if (current().kind == pyc::lexer::TokenType::NAME) {
                targets.push_back(current().value);
                advance();
            } else {
                advance();
            }
        }
        return std::make_shared<ast::DeleteStmt>(targets);
    }

    // with
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "with") {
        advance();
        std::vector<ast::WithItem> items;
        while (has_more() && current().kind != pyc::lexer::TokenType::COLON && current().kind != pyc::lexer::TokenType::NEWLINE && current().kind != pyc::lexer::TokenType::DECREMENT) {
            auto ctx = parse_expr();
            std::string var;
            if (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "as") {
                advance();
                if (has_more() && current().kind == pyc::lexer::TokenType::NAME) {
                    var = current().value;
                    advance();
                }
            }
            items.push_back(ast::WithItem(ctx, var));
            if (has_more() && current().kind == pyc::lexer::TokenType::COMMA) advance();
        }
        advance(); // skip colon
        advance(); // skip newline
        auto body = parse_block();
        return std::make_shared<ast::WithStmt>(items, body);
    }

    // try
    if (tok.kind == pyc::lexer::TokenType::NAME && tok.value == "try") {
        return parse_try_stmt();
    }

    // Assignment or expression statement
    if (tok.kind == pyc::lexer::TokenType::NAME && !is_keyword(tok.value) && !is_builtin(tok.value)) {
        return parse_assign_or_expr();
    }

      // Expression statement (skip for now)
    if (tok.kind == pyc::lexer::TokenType::INT_LITERAL || tok.kind == pyc::lexer::TokenType::FLOAT_LITERAL ||
        tok.kind == pyc::lexer::TokenType::STR_LITERAL || tok.kind == pyc::lexer::TokenType::LPAREN ||
        tok.kind == pyc::lexer::TokenType::LBRACKET || tok.kind == pyc::lexer::TokenType::NAME) {
        parse_expr();
        advance();
        return std::make_shared<ast::PassStmt>();
    }

    // Skip unknown tokens
    advance();
    return std::make_shared<ast::PassStmt>();
}

// ===== Statement Parsers =====

std::shared_ptr<ast::Stmt> Parser::parse_if_stmt() {
    advance(); // skip 'if'
    auto test = parse_expr();
    expect(pyc::lexer::TokenType::COLON);
    advance(); // skip newline
    auto body = parse_block();
    std::vector<std::shared_ptr<ast::Stmt>> orelse;

    while (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "elif") {
        advance(); // skip 'elif'
        auto elif_test = parse_expr();
        expect(pyc::lexer::TokenType::COLON);
        advance();
        auto elif_body = parse_block();
        orelse.push_back(std::make_shared<ast::IfStmt>(elif_test, elif_body, std::vector<std::shared_ptr<ast::Stmt>>()));
    }

    if (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "else") {
        advance();
        expect(pyc::lexer::TokenType::COLON);
        advance();
        orelse = parse_block();
    }

    return std::make_shared<ast::IfStmt>(test, body, orelse);
}

std::shared_ptr<ast::Stmt> Parser::parse_for_stmt() {
    advance(); // skip 'for'
    std::string target = has_more() ? current().value : "";
    if (has_more()) advance();
    expect(pyc::lexer::TokenType::NAME); // 'in'
    advance();
    auto iter = parse_expr();
    expect(pyc::lexer::TokenType::COLON);
    advance();
    auto body = parse_block();
    return std::make_shared<ast::ForStmt>(target, iter, body);
}

std::shared_ptr<ast::Stmt> Parser::parse_while_stmt() {
    advance(); // skip 'while'
    auto test = parse_expr();
    expect(pyc::lexer::TokenType::COLON);
    advance();
    auto body = parse_block();
    return std::make_shared<ast::WhileStmt>(test, body);
}

std::shared_ptr<ast::Stmt> Parser::parse_function_def() {
    if (!has_more()) throw std::runtime_error("Expected function name");
    std::string name = current().value;
    advance();

    expect(pyc::lexer::TokenType::LPAREN);
    std::vector<ast::FunctionDef::Arg> args;
    while (has_more() && current().kind != pyc::lexer::TokenType::RPAREN) {
        if (current().kind == pyc::lexer::TokenType::NAME) {
            std::string arg_name = current().value;
            advance();
            args.push_back(ast::FunctionDef::Arg{arg_name});
            if (has_more() && current().kind == pyc::lexer::TokenType::COMMA) {
                advance();
            }
        } else {
            advance();
        }
    }
    if (has_more() && current().kind == pyc::lexer::TokenType::RPAREN) advance();

    // Optional return type annotation
    if (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "->") {
        advance();
        parse_expr(); // skip return type
    }

    expect(pyc::lexer::TokenType::COLON);
    advance();
    auto body = parse_block();
    return std::make_shared<ast::FunctionDef>(name, args, body);
}

std::shared_ptr<ast::Stmt> Parser::parse_class_def() {
    if (!has_more()) throw std::runtime_error("Expected class name");
    std::string name = current().value;
    advance();

    std::vector<std::string> bases;
    if (has_more() && current().kind == pyc::lexer::TokenType::LPAREN) {
        advance();
        while (has_more() && current().kind != pyc::lexer::TokenType::RPAREN) {
            if (current().kind == pyc::lexer::TokenType::NAME) {
                bases.push_back(current().value);
                advance();
            } else {
                advance();
            }
            if (has_more() && current().kind == pyc::lexer::TokenType::COMMA) advance();
        }
        if (has_more() && current().kind == pyc::lexer::TokenType::RPAREN) advance();
    }

    expect(pyc::lexer::TokenType::COLON);
    advance();
    auto body = parse_block();
    return std::make_shared<ast::ClassDef>(name, bases, body);
}

std::shared_ptr<ast::Stmt> Parser::parse_try_stmt() {
    advance(); // skip 'try'
    expect(pyc::lexer::TokenType::COLON);
    advance();
    auto try_body = parse_block();

    std::vector<ast::ExceptClause> handlers;
    while (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "except") {
        advance();
        std::shared_ptr<ast::Expr> exc_type;
        if (has_more() && current().kind == pyc::lexer::TokenType::NAME) {
            exc_type = std::make_shared<ast::Name>(current().value);
            advance();
        }
        expect(pyc::lexer::TokenType::COLON);
        advance();
        auto body = parse_block();
        handlers.emplace_back(std::move(exc_type), std::move(body));
    }

    std::vector<std::shared_ptr<ast::Stmt>> orelse;
    std::vector<std::shared_ptr<ast::Stmt>> finalbody;
    if (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "else") {
        advance();
        expect(pyc::lexer::TokenType::COLON);
        advance();
        orelse = parse_block();
    }
    if (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "finally") {
        advance();
        expect(pyc::lexer::TokenType::COLON);
        advance();
        finalbody = parse_block();
    }

    return std::make_shared<ast::TryStmt>(try_body, handlers, orelse, finalbody);
}

std::shared_ptr<ast::Stmt> Parser::parse_assign_or_expr() {
    std::string name = current().value;
    advance();

    if (!has_more()) return std::make_shared<ast::PassStmt>();

    // Assignment
    if (current().kind == pyc::lexer::TokenType::ASSIGN) {
        advance();
        auto value = parse_expr();
        return std::make_shared<ast::AssignStmt>(std::vector<std::string>{name}, value);
    }

    // Augmented assignment
    if (current().kind == pyc::lexer::TokenType::IADD || current().kind == pyc::lexer::TokenType::ISUB ||
        current().kind == pyc::lexer::TokenType::IMUL || current().kind == pyc::lexer::TokenType::IDIV) {
        pyc::lexer::TokenType kind = current().kind;
        advance();
        auto value = parse_expr();
        ast::AugAssignStmt::Op op;
        switch (kind) {
            case pyc::lexer::TokenType::IADD: op = ast::AugAssignStmt::ADD; break;
            case pyc::lexer::TokenType::ISUB: op = ast::AugAssignStmt::SUB; break;
            case pyc::lexer::TokenType::IMUL: op = ast::AugAssignStmt::MUL; break;
            case pyc::lexer::TokenType::IDIV: op = ast::AugAssignStmt::DIV; break;
            default: op = ast::AugAssignStmt::ADD; break;
        }
        return std::make_shared<ast::AugAssignStmt>(name, op, value);
    }

    // Expression statement
    std::shared_ptr<ast::Expr> expr = std::make_shared<ast::Name>(name);
    // Check for call
    if (has_more() && current().kind == pyc::lexer::TokenType::LPAREN) {
        advance();
        std::vector<std::shared_ptr<ast::Expr>> args;
        while (has_more() && current().kind != pyc::lexer::TokenType::RPAREN) {
            args.push_back(parse_expr());
            if (has_more() && current().kind == pyc::lexer::TokenType::COMMA) advance();
        }
        if (has_more() && current().kind == pyc::lexer::TokenType::RPAREN) advance();
        expr = std::make_shared<ast::CallExpr>(expr, std::move(args));
    }
    return std::make_shared<ast::PassStmt>();
}

// ===== Block Parsing =====

std::vector<std::shared_ptr<ast::Stmt>> Parser::parse_block() {
    std::vector<std::shared_ptr<ast::Stmt>> body;
    while (has_more() && current().kind != pyc::lexer::TokenType::DECREMENT && current().kind != pyc::lexer::TokenType::EOF_) {
        if (current().kind == pyc::lexer::TokenType::NEWLINE) {
            advance();
            continue;
        }
        if (current().kind == pyc::lexer::TokenType::DECREMENT) break;
        body.push_back(parse_stmt());
        if (!has_more() || current().kind == pyc::lexer::TokenType::DECREMENT) break;
    }
    return body;
}

// ===== Expression Parsing =====

std::shared_ptr<ast::Expr> Parser::parse_expr() {
    return parse_or_expr();
}

std::shared_ptr<ast::Expr> Parser::parse_or_expr() {
    auto left = parse_and_expr();
    while (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "or") {
        advance();
        auto right = parse_and_expr();
        left = std::make_shared<ast::BinOpExpr>(ast::BinOpExpr::OR, std::move(left), std::move(right));
    }
    return left;
}

std::shared_ptr<ast::Expr> Parser::parse_and_expr() {
    auto left = parse_not_expr();
    while (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "and") {
        advance();
        auto right = parse_not_expr();
        left = std::make_shared<ast::BinOpExpr>(ast::BinOpExpr::AND, std::move(left), std::move(right));
    }
    return left;
}

std::shared_ptr<ast::Expr> Parser::parse_not_expr() {
    if (has_more() && current().kind == pyc::lexer::TokenType::NAME && current().value == "not") {
        advance();
        auto operand = parse_not_expr();
        return std::make_shared<ast::UnaryOpExpr>(ast::UnaryOpExpr::NOT, std::move(operand));
    }
    return parse_comparison_expr();
}

std::shared_ptr<ast::Expr> Parser::parse_comparison_expr() {
    auto left = parse_add_expr();
    while (has_more() && (current().kind == pyc::lexer::TokenType::LT || current().kind == pyc::lexer::TokenType::LE ||
           current().kind == pyc::lexer::TokenType::GT || current().kind == pyc::lexer::TokenType::GE ||
           current().kind == pyc::lexer::TokenType::EQ || current().kind == pyc::lexer::TokenType::NE)) {
        pyc::lexer::TokenType kind = current().kind;
        advance();
        auto right = parse_add_expr();
        ast::BinOpExpr::Op op;
        switch (kind) {
            case pyc::lexer::TokenType::LT: op = ast::BinOpExpr::LT; break;
            case pyc::lexer::TokenType::LE: op = ast::BinOpExpr::LE; break;
            case pyc::lexer::TokenType::GT: op = ast::BinOpExpr::GT; break;
            case pyc::lexer::TokenType::GE: op = ast::BinOpExpr::GE; break;
            case pyc::lexer::TokenType::EQ: op = ast::BinOpExpr::EQ; break;
            case pyc::lexer::TokenType::NE: op = ast::BinOpExpr::NE; break;
            default: op = ast::BinOpExpr::EQ; break;
        }
        left = std::make_shared<ast::BinOpExpr>(op, std::move(left), std::move(right));
    }
    return left;
}

std::shared_ptr<ast::Expr> Parser::parse_add_expr() {
    auto left = parse_mul_expr();
    while (has_more() && (current().kind == pyc::lexer::TokenType::IADD || current().kind == pyc::lexer::TokenType::ISUB)) {
        pyc::lexer::TokenType kind = current().kind;
        advance();
        auto right = parse_mul_expr();
        ast::BinOpExpr::Op op = (kind == pyc::lexer::TokenType::IADD) ? ast::BinOpExpr::ADD : ast::BinOpExpr::SUB;
        left = std::make_shared<ast::BinOpExpr>(op, std::move(left), std::move(right));
    }
    return left;
}

std::shared_ptr<ast::Expr> Parser::parse_mul_expr() {
    auto left = parse_unary_expr();
    while (has_more() && (current().kind == pyc::lexer::TokenType::IMUL || current().kind == pyc::lexer::TokenType::IDIV ||
           current().kind == pyc::lexer::TokenType::PERCENT || current().kind == pyc::lexer::TokenType::POW)) {
        pyc::lexer::TokenType kind = current().kind;
        advance();
        auto right = parse_unary_expr();
        ast::BinOpExpr::Op op;
        switch (kind) {
            case pyc::lexer::TokenType::IMUL: op = ast::BinOpExpr::MUL; break;
            case pyc::lexer::TokenType::IDIV: op = ast::BinOpExpr::DIV; break;
            case pyc::lexer::TokenType::PERCENT: op = ast::BinOpExpr::MOD; break;
            case pyc::lexer::TokenType::POW: op = ast::BinOpExpr::POW; break;
            default: op = ast::BinOpExpr::MUL; break;
        }
        left = std::make_shared<ast::BinOpExpr>(op, std::move(left), std::move(right));
    }
    return left;
}

std::shared_ptr<ast::Expr> Parser::parse_unary_expr() {
    if (has_more() && current().kind == pyc::lexer::TokenType::MINUS) {
        advance();
        auto operand = parse_unary_expr();
        return std::make_shared<ast::UnaryOpExpr>(ast::UnaryOpExpr::NEG, std::move(operand));
    }
    if (has_more() && current().kind == pyc::lexer::TokenType::PLUS) {
        advance();
        return parse_unary_expr();
    }
    return parse_primary_expr();
}

std::shared_ptr<ast::Expr> Parser::parse_primary_expr() {
    if (!has_more()) return nullptr;

    const auto& tok = current();

    // Literals
    if (tok.kind == pyc::lexer::TokenType::INT_LITERAL) {
        advance();
        return std::make_shared<ast::IntLiteral>(tok.int_val);
    }
    if (tok.kind == pyc::lexer::TokenType::FLOAT_LITERAL) {
        advance();
        double val = std::stod(tok.value);
        return std::make_shared<ast::FloatLiteral>(val);
    }
    if (tok.kind == pyc::lexer::TokenType::STR_LITERAL) {
        advance();
        std::string s = tok.value;
        if (s.size() >= 2) {
            if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))
                s = s.substr(1, s.size() - 2);
        }
        return std::make_shared<ast::StrLiteral>(std::move(s));
    }

    // Boolean/None literals
    if (tok.kind == pyc::lexer::TokenType::NAME) {
        std::string name = tok.value;
        advance();

        if (name == "True") return std::make_shared<ast::BoolLiteral>(true);
        if (name == "False") return std::make_shared<ast::BoolLiteral>(false);
        if (name == "None") return std::make_shared<ast::NoneLiteral>();

        // Name expression
        std::shared_ptr<ast::Expr> expr = std::make_shared<ast::Name>(name);

        // Check for function call
        if (has_more() && current().kind == pyc::lexer::TokenType::LPAREN) {
            advance();
            std::vector<std::shared_ptr<ast::Expr>> args;
            while (has_more() && current().kind != pyc::lexer::TokenType::RPAREN) {
                args.push_back(parse_expr());
                if (has_more() && current().kind == pyc::lexer::TokenType::COMMA) advance();
            }
            if (has_more() && current().kind == pyc::lexer::TokenType::RPAREN) advance();
            expr = std::make_shared<ast::CallExpr>(expr, std::move(args));
        }

        // Check for attribute access
        while (has_more() && current().kind == pyc::lexer::TokenType::DOT) {
            advance();
            std::string attr = has_more() ? current().value : "";
            if (has_more()) advance();
            expr = std::make_shared<ast::AttrExpr>(expr, attr);
        }

        // Check for subscript
        while (has_more() && current().kind == pyc::lexer::TokenType::LBRACKET) {
            advance();
            auto slice = parse_expr();
            if (has_more() && current().kind == pyc::lexer::TokenType::RBRACKET) advance();
            expr = std::make_shared<ast::SubscriptExpr>(expr, slice);
        }

        return expr;
    }

    // List literal
    if (tok.kind == pyc::lexer::TokenType::LBRACKET) {
        advance();
        std::vector<std::shared_ptr<ast::Expr>> elems;
        while (has_more() && current().kind != pyc::lexer::TokenType::RBRACKET) {
            elems.push_back(parse_expr());
            if (has_more() && current().kind == pyc::lexer::TokenType::COMMA) advance();
        }
        if (has_more() && current().kind == pyc::lexer::TokenType::RBRACKET) advance();
        return std::make_shared<ast::ListExpr>(std::move(elems));
    }

    // Parenthesized expression
    if (tok.kind == pyc::lexer::TokenType::LPAREN) {
        advance();
        auto e = parse_expr();
        if (has_more() && current().kind == pyc::lexer::TokenType::RPAREN) advance();
        return e;
    }

    throw std::runtime_error(std::string("Unexpected token: ") + tok.value);
    return nullptr;
}

// ===== Helper Functions =====

bool Parser::has_more() const {
    return pos_ < tokens_.size() && tokens_[pos_].kind != pyc::lexer::TokenType::EOF_;
}

const pyc::lexer::Token& Parser::current() {
    if (pos_ >= tokens_.size()) {
        static const pyc::lexer::Token eof_token{pyc::lexer::TokenType::EOF_, ""};
        return eof_token;
    }
    return tokens_[pos_];
}

void Parser::advance() {
    if (pos_ < tokens_.size()) {
        pos_++;
    }
}

void Parser::expect(pyc::lexer::TokenType type) {
    if (current().kind != type) {
        std::string type_names[] = {"NAME", "INT", "FLOAT", "STR", "IADD", "ISUB", "IMUL", "IDIV", "POW", "PERCENT", "LT", "LE", "GT", "GE", "EQ", "NE", "ASSIGN", "COLON", "COMMA", "LPAREN", "RPAREN", "LBRACKET", "RBRACKET", "DOT", "MINUS", "PLUS", "NEWLINE", "DECREMENT", "DEF", "CLASS", "PASS", "BREAK", "CONTINUE", "EOF_"};
        std::string type_name = (static_cast<int>(type) >= 0 && static_cast<int>(type) < 34) ? type_names[static_cast<int>(type)] : "UNKNOWN";
        std::string current_name = (static_cast<int>(current().kind) >= 0 && static_cast<int>(current().kind) < 34) ? type_names[static_cast<int>(current().kind)] : "UNKNOWN";
        throw std::runtime_error(std::string("Expected ") + type_name + " (" + std::to_string(static_cast<int>(type)) + ") got " + current_name + " (" + std::to_string(static_cast<int>(current().kind)) + ") value='" + current().value + "'");
    }
    advance();
}

} // namespace pyc::parser
