#pragma once
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <optional>
#include "frontend/ast.h"
#include "frontend/lexer.h"

namespace pyc::parser {

template<class T> struct is_smart_pointer_v : std::false_type {};
template<class T, class A> struct is_smart_pointer_v<std::unique_ptr<T, A>> : std::true_type {};
template<class T, class A> struct is_smart_pointer_v<std::shared_ptr<T, A>> : std::true_type {};
template<class T> inline constexpr bool is_smart_pointer = is_smart_pointer_v<T>::value;

// Type-erased function type for visitor pattern
using ASTFunc = std::function<void(
    ast::IntLiteral&, ast::FloatLiteral&, ast::StrLiteral&, ast::BoolLiteral&, ast::NoneLiteral&,
    ast::Name&, ast::BinOpExpr&, ast::UnaryOpExpr&, ast::CallExpr&, ast::AttrExpr&,
    ast::SubscriptExpr&, ast::ListExpr,
    ast::AssignStmt&, ast::AugAssignStmt&, ast::ReturnStmt&, ast::IfStmt&, ast::ForStmt&,
    ast::WhileStmt&, ast::FunctionDef&, ast::ClassDef&, ast::PassStmt&, ast::BreakStmt&,
    ast::ContinueStmt&
)>;

// Visit each AST variant. Pass a function that handles each variant.
template<class Visitor>
void visit(const ast::Node& node, Visitor&& v) {
    if (auto* p = dynamic_cast<const ast::IntLiteral*>(&node))    { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::FloatLiteral*>(&node))  { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::StrLiteral*>(&node))    { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::BoolLiteral*>(&node))   { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::NoneLiteral*>(&node))   { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::Name*>(&node))          { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::BinOpExpr*>(&node))     { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::UnaryOpExpr*>(&node))   { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::CallExpr*>(&node))      { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::AttrExpr*>(&node))      { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::SubscriptExpr*>(&node)) { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::ListExpr*>(&node))      { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::AssignStmt*>(&node))    { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::AugAssignStmt*>(&node)) { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::ReturnStmt*>(&node))    { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::IfStmt*>(&node))        { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::ForStmt*>(&node))       { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::WhileStmt*>(&node))     { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::FunctionDef*>(&node))   { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::ClassDef*>(&node))      { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::PassStmt*>(&node))      { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::BreakStmt*>(&node))     { v(*p); return; }
    if (auto* p = dynamic_cast<const ast::ContinueStmt*>(&node))  { v(*p); return; }
}

class Parser {
    const std::vector<Token>& tokens_;
    size_t pos_ = 0;

public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    std::shared_ptr<ast::Module> parse() {
        auto stmts = parse_stmt_list();
        auto mod = std::make_shared<ast::Module>(std::move(stmts));
        mod->classify_funcs_and_classes();
        return mod;
    }

    std::vector<std::shared_ptr<ast::Stmt>> parse_stmt_list() {
        std::vector<std::shared_ptr<ast::Stmt>> stmts;
        while (has_more()) {
            stmts.push_back(parse_stmt());
            if (!has_more()) break;
        }
        return stmts;
    }

    std::shared_ptr<ast::Stmt> parse_stmt() {
        if (!has_more()) return nullptr;

        if (current().kind == Token_TokenType::PASS) {
            auto tok = peek(); advance(); return std::make_shared<ast::PassStmt>();
        }
        if (current().kind == Token_TokenType::BREAK) {
            auto tok = peek(); advance(); return std::make_shared<ast::BreakStmt>();
        }
        if (current().kind == Token_TokenType::CONTINUE) {
            auto tok = peek(); advance(); return std::make_shared<ast::ContinueStmt>();
        }

        if (current().kind == Token_TokenType::NAME && peek().value == "return") {
            return parse_return();
        }
        if (current().kind == Token_TokenType::NAME && peek().value == "if") {
            consume(); consume(); // skip 'if'
            return parse_if_body();
        }
        if (current().kind == Token_TokenType::NAME && peek().value == "for") {
            consume(); consume(); // skip 'for'
            return parse_for();
        }
        if (current().kind == Token_TokenType::NAME && peek().value == "while") {
            consume(); consume(); // skip 'while'
            return parse_while();
        }

        // Check for function/class def
        if (current().kind == Token_TokenType::CLASS) {
            consume(); // skip 'class'
            return parse_class();
        }
        if (current().kind == Token_TokenType::DEF) {
            consume(); // skip 'def'
            return parse_function();
        }

        // Must be assignment (identifier = ... or identifier := ...)
        return parse_assign();
    }

    std::shared_ptr<ast::Stmt> parse_return() {
        consume(); consume(); // skip 'return'
        std::shared_ptr<ast::Expr> val;
        if (has_more() && current().kind != Token_TokenType::NEWLINE &&
            current().kind != Token_TokenType::DECREMENT &&
            current().kind != Token_TokenType::EOF_) {
            val = parse_expr();
        }
        return std::make_shared<ast::ReturnStmt>(std::move(val));
    }

    struct IfBranch {
        std::shared_ptr<ast::Expr> test;
        std::vector<std::shared_ptr<ast::Stmt>> body;
    };

    std::shared_ptr<ast::Stmt> parse_if_body() {
        auto branches = std::vector<IfBranch>{};
        auto orelse = std::vector<std::shared_ptr<ast::Stmt>>{};

        while (has_more()) {
            IfBranch br{};
            br.test = parse_comparison();
            expect(Token_TokenType::COLON);
            advance(); // newline
            br.body = parse_block();

            // Check for elif/else
            if (has_more() && current().kind == Token_TokenType::NAME &&
                peek().value == "elif") {
                consume(); consume(); // skip 'elif'
                continue; // start another iteration
            }
            else if (has_more() && current().kind == Token_TokenType::NAME &&
                     peek().value == "else") {
                consume(); consume(); // skip 'else'
                expect(Token_TokenType::COLON);
                advance(); // newline
                orelse = parse_block();
                break;
            }
            else {
                branches.push_back(br);
                break;
            }
        }

        if (branches.size() == 1 && orelse.empty()) {
            return std::make_shared<ast::IfStmt>(
                branches[0].test, branches[0].body, {});
        }

        return std::make_shared<ast::IfStmt>(
            branches[0].test, branches[0].body, orelse);
    }

    std::vector<std::shared_ptr<ast::Stmt>> parse_block() {
        std::vector<std::shared_ptr<ast::Stmt>> body;
        while (has_block_stmt()) {
            body.push_back(parse_stmt());
            if (!has_block_stmt()) break;
        }
        return body;
    }

    bool has_block_stmt() { return has_more() && current().kind != Token_TokenType::EOF_; }

    std::shared_ptr<ast::Stmt> parse_for() {
        auto target = current().value;
        consume(); advance(); advance(); advance();
        consume(); consume(); // skip 'in'
        auto iter = parse_expr();
        expect(Token_TokenType::COLON);
        advance(); // newline
        auto body = parse_block();
        return std::make_shared<ast::ForStmt>(target, iter, body);
    }

    std::shared_ptr<ast::Stmt> parse_while() {
        auto test = parse_expr();
        expect(Token_TokenType::COLON);
        advance(); // newline
        auto body = parse_block();
        return std::make_shared<ast::WhileStmt>(test, body);
    }

    std::shared_ptr<ast::Stmt> parse_function() {
        auto name = current().value;
        consume(); advance(); advance(); advance();
        expect(Token_TokenType::LPAREN);
        consume(); // '('
        std::vector<ast::FunctionDef::Arg> args;
        while (current().kind != Token_TokenType::RPAREN) {
            std::string arg_name = current().value;
            consume(); advance(); advance(); advance();
            args.push_back(ast::FunctionDef::Arg{arg_name});
            if (current().kind == Token_TokenType::COMMA) {
                consume(); advance(); advance(); advance();
            }
        }
        if (current().kind == Token_TokenType::RPAREN) {
            consume(); advance(); // skip RPAREN
        }
        else { consume(); advance(); advance(); advance(); }

        expect(Token_TokenType::COLON);
        consume(); advance(); advance(); advance();

        auto body = parse_block();
        return std::make_shared<ast::FunctionDef>(name, args, body);
    }

    std::shared_ptr<ast::Stmt> parse_class() {
        auto name = current().value;
        consume(); advance(); advance(); advance();

        std::vector<std::string> bases;
        if (current().kind == Token_TokenType::LPAREN) {
            consume(); advance(); advance(); // skip '('
            while (current().kind != Token_TokenType::RPAREN && has_more()) {
                std::string base = current().value;
                consume(); advance(); advance(); advance();
                bases.push_back(base);
                if (current().kind == Token_TokenType::COMMA) {
                    consume(); advance(); advance(); advance();
                }
            }
            if (current().kind == Token_TokenType::RPAREN) {
                consume(); advance();
            }
        }

        expect(Token_TokenType::COLON);
        consume(); advance(); advance(); advance();
        auto body = parse_block();
        return std::make_shared<ast::ClassDef>(name, bases, body);
    }

    struct AssignTarget {
        std::string name;
    };

    std::shared_ptr<ast::Stmt> parse_assign() {
        AssignTarget target;
        target.name = current().value;
        consume(); advance(); advance(); advance();

        if (current().kind == Token_TokenType::ASSIGN) {
            consume(); advance();
            auto value = parse_expr();
            auto target_list = std::vector<std::string>{target.name};
            return std::make_shared<ast::AssignStmt>(target_list, value);
        }
        else if (current().kind == Token_TokenType::IADD ||
                 current().kind == Token_TokenType::ISUB ||
                 current().kind == Token_TokenType::IMUL ||
                 current().kind == Token_TokenType::IDIV) {
            auto op = TokenType_ADD;
            if (current().kind == Token_TokenType::IADD) op = TokenType_ADD;
            else if (current().kind == Token_TokenType::ISUB) op = TokenType_SUB;
            else if (current().kind == Token_TokenType::IMUL) op = TokenType_MUL;
            else if (current().kind == Token_TokenType::IDIV) op = TokenType_DIV;

            consume(); advance();
            auto value = parse_expr();

            return std::make_shared<ast::AugAssignStmt>(target.name, op, value);
        }

        // Not an assignment? Skip to newline
        advance_to_newline();
        return std::make_shared<ast::PassStmt>();
    }

    std::shared_ptr<ast::Expr> parse_expr() {
        return parse_or();
    }

    std::shared_ptr<ast::Expr> parse_or() {
        auto left = parse_and();
        while (has_more() && current().kind == Token_TokenType::NAME && current().value == "or") {
            consume(); advance();
            auto right = parse_and();
            left = std::make_shared<ast::BinOpExpr>(ast::BinOpExpr::OR, std::move(left), std::move(right));
        }
        return left;
    }

    std::shared_ptr<ast::Expr> parse_and() {
        auto left = parse_not();
        while (has_more() && current().kind == Token_TokenType::NAME && current().value == "and") {
            consume(); advance();
            auto right = parse_not();
            left = std::make_shared<ast::BinOpExpr>(ast::BinOpExpr::AND, std::move(left), std::move(right));
        }
        return left;
    }

    std::shared_ptr<ast::Expr> parse_not() {
        if (has_more() && current().kind == Token_TokenType::NAME && current().value == "not") {
            consume(); advance();
            auto operand = parse_not();
            return std::make_shared<ast::UnaryOpExpr>(ast::UnaryOpExpr::NOT, std::move(operand));
        }
        return parse_comparison();
    }

    std::shared_ptr<ast::Expr> parse_comparison() {
        auto left = parse_add_sub();
        while (has_more() && (current().kind == Token_TokenType::LT ||
               current().kind == Token_TokenType::LE ||
               current().kind == Token_TokenType::GT ||
               current().kind == Token_TokenType::GE ||
               current().kind == Token_TokenType::EQ ||
               current().kind == Token_TokenType::NE)) {
            TokenType kind = current().kind;
            consume(); advance();
            auto right = parse_add_sub();
            ast::BinOpExpr::Op op;
            switch (kind) {
                case Token_LT: op = ast::BinOpExpr::LT; break;
                case Token_LE: op = ast::BinOpExpr::LE; break;
                case Token_GT: op = ast::BinOpExpr::GT; break;
                case Token_GE: op = ast::BinOpExpr::GE; break;
                case Token_EQ: op = ast::BinOpExpr::EQ; break;
                case Token_NE: op = ast::BinOpExpr::NE; break;
                default: op = ast::BinOpExpr::EQ; break;
            }
            left = std::make_shared<ast::BinOpExpr>(op, std::move(left), std::move(right));
        }
        return left;
    }

    std::shared_ptr<ast::Expr> parse_add_sub() {
        auto left = parse_mul_div();
        while (has_more() && (current().kind == Token_TokenType::IADD ||
               current().kind == Token_TokenType::ISUB)) {
            TokenType kind = current().kind;
            consume(); advance();
            auto right = parse_mul_div();
            ast::BinOpExpr::Op op = (kind == Token_IADD) ? ast::BinOpExpr::ADD : ast::BinOpExpr::SUB;
            left = std::make_shared<ast::BinOpExpr>(op, std::move(left), std::move(right));
        }
        return left;
    }

    std::shared_ptr<ast::Expr> parse_mul_div() {
        auto left = parse_unary();
        while (has_more() && current().kind != Token_TokenType::EOF_ && (
               current().kind == Token_TokenType::IMUL ||
               current().kind == Token_TokenType::IDIV ||
               current().kind == Token_TokenType::PERCENT ||
               current().kind == Token_TokenType::POW)) {
            TokenType kind = current().kind;
            consume(); advance();
            auto right = parse_unary();
            ast::BinOpExpr::Op op;
            switch (kind) {
                case Token_IMUL: op = ast::BinOpExpr::MUL; break;
                case Token_IDIV: op = ast::BinOpExpr::DIV; break;
                case Token_PERCENT: op = ast::BinOpExpr::MOD; break;
                case Token_POW: op = ast::BinOpExpr::POW; break;
                default: op = ast::BinOpExpr::MUL; break;
            }
            left = std::make_shared<ast::BinOpExpr>(op, std::move(left), std::move(right));
        }
        return left;
    }

    std::shared_ptr<ast::Expr> parse_unary() {
        if (has_more() && current().kind == Token_TokenType::MINUS) {
            consume(); advance();
            auto operand = parse_unary();
            return std::make_shared<ast::UnaryOpExpr>(ast::UnaryOpExpr::NEG, std::move(operand));
        }

        if (has_more() && current().kind == Token_TokenType::NAME && current().value == "True") {
            consume(); advance();
            return std::make_shared<ast::BoolLiteral>(true);
        }
        if (has_more() && current().kind == Token_TokenType::NAME && current().value == "False") {
            consume(); advance();
            return std::make_shared<ast::BoolLiteral>(false);
        }
        if (has_more() && current().kind == Token_TokenType::NAME && current().value == "None") {
            consume(); advance();
            return std::make_shared<ast::NoneLiteral>();
        }

        return parse_primary();
    }

    std::shared_ptr<ast::Expr> parse_primary() {
        if (!has_more()) return nullptr;

        const auto& tok = current();

        // Integer literal
        if (tok.kind == Token_INT_LITERAL) {
            consume(); advance();
            return std::make_shared<ast::IntLiteral>(tok.int_val);
        }
        // Float literal
        if (tok.kind == Token_FLOAT_LITERAL) {
            consume(); advance();
            double val = 0;
            std::from_chars(tok.value.c_str(), tok.value.c_str() + tok.value.size(), val);
            return std::make_shared<ast::FloatLiteral>(val);
        }
        // String literal
        if (tok.kind == Token_STR_LITERAL) {
            consume(); advance();
            std::string s = tok.value;
            // Strip quotes
            if (s.size() >= 2) {
                if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))
                    s = s.substr(1, s.size() - 2);
            }
            return std::make_shared<ast::StrLiteral>(std::move(s));
        }
        // Name
        if (tok.kind == Token_NAME) {
            std::string name = tok.value;
            consume(); advance();

            // Name(...) function call
            if (has_more() && current().kind == Token_LPAREN) {
                advance(); // skip LPAREN
                std::vector<std::shared_ptr<ast::Expr>> args;
                while (has_more() && current().kind != Token_RPAREN) {
                    auto arg = parse_expr();
                    args.push_back(arg);
                    if (has_more() && current().kind == Token_COMMA) {
                        advance();
                    }
                }
                // Consume RPAREN
                if (has_more()) advance(); // skip RPAREN
                return std::make_shared<ast::CallExpr>(
                    std::make_shared<ast::Name>(name), std::move(args));
            }
            // Name[...] subscript
            if (has_more() && current().kind == Token_LBRACKET) {
                advance(); advance(); advance(); advance(); advance(); advance(); advance();
                // TODO
            }
            // Name.attr attribute
            if (has_more() && current().kind == Token_DOT) {
                advance(); // skip DOT
                std::string attr = current().value;
                advance(); advance(); advance();
                return std::make_shared<ast::AttrExpr>(
                    std::make_shared<ast::Name>(name), attr);
            }

            return std::make_shared<ast::Name>(name);
        }

        // (expression)
        if (tok.kind == Token_LPAREN) {
            advance(); // skip LPAREN
            auto e = parse_expr();
            if (has_more()) advance(); // skip RPAREN
            return e;
        }

        throw std::runtime_error(std::string("Unexpected token: ") + tok.value);
        return nullptr;
    }

private:
    bool has_more() const { return pos_ < tokens_.size() && tokens_[pos_].kind != Token_EOF_; }
    const Token& current() { return tokens_[pos_]; }
    const Token& peek() const { return pos_ + 1 < tokens_.size() ? tokens_[pos_ + 1] : current(); }

    void consume() { ++pos_; }
    void advance() {
        while (has_more() && (current().kind == Token_NEWLINE || current().kind == Token_DECREMENT)) {
            ++pos_;
        }
        if (has_more()) ++pos_;
    }

    Token_TokenType expect(TokenType type) {
        if (current().kind != type) {
            throw std::runtime_error(std::string("Expected ") + std::to_string(static_cast<int>(type)) +
                " got " + current().value);
        }
        return type;
    }

    void advance_to_newline() {
        while (has_more() && current().kind != Token_NEWLINE && current().kind != Token_DECREMENT) {
            advance();
        }
    }
};

} // namespace pyc::parser
