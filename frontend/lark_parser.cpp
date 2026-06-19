// lark_parser.cpp - C++ bridge for Python Lark grammar
// Architecture: Python (lark_bridge.py) parses .py → outputs JSON → C++ reads JSON → builds AST
// The actual parsing happens in Python; this reads the JSON AST output

#include "frontend/lark_parser.h"
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <filesystem>

using Value = json_ast::Value;

namespace pyc::parser {

namespace {

// Forward declarations
std::shared_ptr<ast::Expr> build_expr(const std::shared_ptr<Value>& node);
ast::Module build_module(const std::shared_ptr<Value>& node);

// Simple JSON value reader (no external library needed)
struct JsonReader {
    std::string src;
    size_t pos;

    JsonReader(std::string s) : src(std::move(s)), pos(0) {}

    void skip_ws() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\n' || src[pos] == '\r' || src[pos] == '\t'))
            ++pos;
    }

    char peek() {
        skip_ws();
        return pos < src.size() ? src[pos] : '\0';
    }

    char get() {
        skip_ws();
        if (pos >= src.size()) return '\0';
        return src[pos++];
    }

    std::string read_string() {
        if (get() != '"') throw std::runtime_error("Expected '\"'");
        std::string result;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') {
                ++pos;
                if (pos < src.size()) result += src[pos++];
            } else {
                result += src[pos++];
            }
        }
        if (pos < src.size()) ++pos; // skip closing "
        return result;
    }

    std::shared_ptr<Value> parse_value() {
        skip_ws();
        if (pos >= src.size()) throw std::runtime_error("Unexpected end of JSON");
        
        if (src[pos] == '{') return parse_object();
        if (src[pos] == '[') return parse_array();
        if (src[pos] == '"') {
            auto v = std::make_shared<Value>();
            v->type = read_string();
            return v;
        }
        if (src[pos] == 't' || src[pos] == 'f') {
            auto v = std::make_shared<Value>();
            size_t start = pos;
            while (pos < src.size() && src[pos] != ',' && src[pos] != '}' && src[pos] != ']' && src[pos] != ' ' && src[pos] != '\n')
                ++pos;
            std::string val = src.substr(start, pos - start);
            v->type = (val == "True" || val == "true") ? "True" : "False";
            return v;
        }
        if (src[pos] == 'n') {
            auto v = std::make_shared<Value>();
            v->type = "None";
            return v;
        }
        if (src[pos] == '-' || (src[pos] >= '0' && src[pos] <= '9')) {
            auto v = std::make_shared<Value>();
            size_t start = pos;
            while (pos < src.size() && src[pos] != ',' && src[pos] != '}' && src[pos] != ']' && src[pos] != ' ' && src[pos] != '\n')
                ++pos;
            std::string val = src.substr(start, pos - start);
            v->value = val;
            if (val.find('.') != std::string::npos) v->type = "float_literal";
            else v->type = "int_literal";
            return v;
        }
        throw std::runtime_error("Unexpected character: " + std::string(1, src[pos]));
    }

    std::shared_ptr<Value> parse_object() {
        get(); // '{'
        auto obj = std::make_shared<Value>();
        while (peek() != '}' && peek() != '\0') {
            std::string key = read_string();
            get(); // ':'
            auto val = parse_value();
            if (key == "type") obj->type = val->type;
            else if (key == "value") obj->value = val->value;
            else if (key == "children") {
                obj->children = val->children;
            }
            // Check for comma terminator
            char c = get();
            if (c == ',') {
                // continue
            } else if (c != '}') {
                throw std::runtime_error("Expected ',' or '}' in JSON object");
            }
        }
        if (peek() == '}') get(); // '}'
        return obj;
    }

    std::shared_ptr<Value> parse_array() {
        get(); // '['
        auto arr = std::make_shared<Value>();
        while (peek() != ']' && peek() != '\0') {
            bool is_value = peek() != '{' && peek() != '"';
            if (peek() == '"') {
                // It's a string value like ["def", "add"]
                std::string s = read_string();
                arr->children.push_back(std::make_shared<Value>(s));
            } else if (peek() == '{') {
                arr->children.push_back(parse_value());
            } else {
                size_t start = pos;
                while (pos < src.size() && src[pos] != ',' && src[pos] != ']' && src[pos] != ' ' && src[pos] != '\n')
                    ++pos;
                std::string val = src.substr(start, pos - start);
                if (!val.empty()) arr->children.push_back(std::make_shared<Value>(val));
                auto c = get();
                if (c == ',') continue;
            }
        }
        if (peek() == ']') get(); // ']'
        return arr;
    }
};

// Forward declarations needed before build_node can call them
ast::Module build_module(const std::shared_ptr<json_ast::Value>& node);
std::shared_ptr<ast::Expr> build_expr(const std::shared_ptr<json_ast::Value>& node);

// Build AST from JSON node
std::shared_ptr<ast::Node> build_ast_node(const std::shared_ptr<Value>& node) {
    if (!node) return nullptr;

    std::string type = node->type;

    // Literals
    if (type == "int_literal") {
        auto v = std::stoi(node->value);
        return std::make_shared<ast::IntLiteral>(static_cast<int>(v));
    }
    if (type == "float_literal") {
        auto v = std::stod(node->value);
        return std::make_shared<ast::FloatLiteral>(v);
    }
    if (type == "str_literal") {
        return std::make_shared<ast::StrLiteral>(node->value);
    }
    if (type == "True") return std::make_shared<ast::BoolLiteral>(true);
    if (type == "False") return std::make_shared<ast::BoolLiteral>(false);
    if (type == "None") return std::make_shared<ast::NoneLiteral>();
    if (type == "EllipsisLiteral") return std::make_shared<ast::EllipsisLiteral>();

    // Name
    if (type == "NAME") return std::make_shared<ast::Name>(node->value);

    // Binary operations
    if (type == "add" || type == "sub" || type == "mul" || type == "div" ||
        type == "floor_div" || type == "mod" || type == "pow" ||
        type == "lt" || type == "le" || type == "gt" || type == "ge" ||
        type == "eq" || type == "ne" || type == "and" || type == "or") {
        ast::BinOpExpr::Op op = ast::BinOpExpr::ADD;
        if (type == "sub") op = ast::BinOpExpr::SUB;
        else if (type == "mul") op = ast::BinOpExpr::MUL;
        else if (type == "div") op = ast::BinOpExpr::DIV;
        else if (type == "floor_div") op = ast::BinOpExpr::FLOOR_DIV;
        else if (type == "mod") op = ast::BinOpExpr::MOD;
        else if (type == "pow") op = ast::BinOpExpr::POW;
        else if (type == "lt") op = ast::BinOpExpr::LT;
        else if (type == "le") op = ast::BinOpExpr::LE;
        else if (type == "gt") op = ast::BinOpExpr::GT;
        else if (type == "ge") op = ast::BinOpExpr::GE;
        else if (type == "eq") op = ast::BinOpExpr::EQ;
        else if (type == "ne") op = ast::BinOpExpr::NE;
        else if (type == "and") op = ast::BinOpExpr::AND;
        else if (type == "or") op = ast::BinOpExpr::OR;

        std::shared_ptr<ast::Expr> left, right;
        if (node->children.size() >= 2) {
            left = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[0]));
            right = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[1]));
        }
        return std::make_shared<ast::BinOpExpr>(op, std::move(left), std::move(right));
    }

    // Unary operations
    if (type == "neg") {
        std::shared_ptr<ast::Expr> operand;
        if (!node->children.empty()) operand = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[0]));
        return std::make_shared<ast::UnaryOpExpr>(ast::UnaryOpExpr::NEG, std::move(operand));
    }
    if (type == "not") {
        std::shared_ptr<ast::Expr> operand;
        if (!node->children.empty()) operand = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[0]));
        return std::make_shared<ast::UnaryOpExpr>(ast::UnaryOpExpr::NOT, std::move(operand));
    }
    if (type == "u+plus") {
        std::shared_ptr<ast::Expr> operand;
        if (!node->children.empty()) operand = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[0]));
        return std::make_shared<ast::UnaryOpExpr>(ast::UnaryOpExpr::UPLUS, std::move(operand));
    }

    // Function definition
    if (type == "stmt_func") {
        std::string name = node->children.size() > 0 ? node->children[0]->value : "unknown";
        std::vector<ast::FunctionDef::Arg> args;
        std::vector<std::shared_ptr<ast::Stmt>> body;

        for (size_t i = 1; i < node->children.size(); ++i) {
            auto c = node->children[i];
            if (c->type == "arg") {
                args.push_back({c->value});
            } else if (c->type == "stmt_func_body") {
                for (auto& s : c->children) {
                    body.push_back(std::dynamic_pointer_cast<ast::Stmt>(build_ast_node(s)));
                }
            }
        }
        return std::make_shared<ast::FunctionDef>(name, args, body);
    }

    // Class definition
    if (type == "stmt_class") {
        std::string name = node->children.size() > 0 ? node->children[0]->value : "unknown";
        std::vector<std::string> bases;
        std::vector<std::shared_ptr<ast::Stmt>> body;

        for (size_t i = 1; i < node->children.size(); ++i) {
            auto c = node->children[i];
            if (c->type == "base") bases.push_back(c->value);
            else if (c->type == "stmt_class_body") {
                for (auto& s : c->children) {
                    body.push_back(std::dynamic_pointer_cast<ast::Stmt>(build_ast_node(s)));
                }
            }
        }
        return std::make_shared<ast::ClassDef>(name, bases, body);
    }

    // If statement
    if (type == "stmt_if") {
        std::shared_ptr<ast::Expr> test_expr;
        std::vector<std::shared_ptr<ast::Stmt>> body, orelse;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto c = node->children[i];
            if (c->type == "if_test") test_expr = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
            else if (c->type == "if_body") {
                for (auto& s : c->children) body.push_back(std::dynamic_pointer_cast<ast::Stmt>(build_ast_node(s)));
            }
            else if (c->type == "else_body") {
                for (auto& s : c->children) orelse.push_back(std::dynamic_pointer_cast<ast::Stmt>(build_ast_node(s)));
            }
        }
        return std::make_shared<ast::IfStmt>(test_expr, body, orelse);
    }

    // For statement
    if (type == "stmt_for") {
        std::string target = node->children.size() > 0 ? node->children[0]->value : "_";
        std::shared_ptr<ast::Expr> iter_expr;
        std::vector<std::shared_ptr<ast::Stmt>> body;
        for (size_t i = 1; i < node->children.size(); ++i) {
            auto c = node->children[i];
            if (c->type == "for_iter") iter_expr = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
            else if (c->type == "for_body") {
                for (auto& s : c->children) body.push_back(std::dynamic_pointer_cast<ast::Stmt>(build_ast_node(s)));
            }
        }
        return std::make_shared<ast::ForStmt>(target, iter_expr, body);
    }

    // While statement
    if (type == "stmt_while") {
        std::shared_ptr<ast::Expr> test;
        std::vector<std::shared_ptr<ast::Stmt>> body;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto c = node->children[i];
            if (c->type == "while_test") test = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
            else if (c->type == "while_body") {
                for (auto& s : c->children) body.push_back(std::dynamic_pointer_cast<ast::Stmt>(build_ast_node(s)));
            }
        }
        return std::make_shared<ast::WhileStmt>(test, body);
    }

    // Expression statement
    if (type == "stmt_expr") {
        return std::dynamic_pointer_cast<ast::Stmt>(build_ast_node(node->children[0]));
    }

    // Return statement
    if (type == "stmt_return") {
        std::shared_ptr<ast::Expr> val;
        if (!node->children.empty()) val = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[0]));
        return std::make_shared<ast::ReturnStmt>(val);
    }

    // Assignment statement
    if (type == "stmt_assign") {
        std::vector<std::string> targets;
        std::shared_ptr<ast::Expr> value;
        for (auto& c : node->children) {
            if (c->type == "assign_target") targets.push_back(c->value);
            else if (c->type == "assign_value") value = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
        }
        return std::make_shared<ast::AssignStmt>(targets, value);
    }

    // Augmented assignment
    if (type == "stmt_augassign") {
        ast::AugAssignStmt::Op op = ast::AugAssignStmt::ADD;
        std::string target;
        std::shared_ptr<ast::Expr> value;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto c = node->children[i];
            if (c->type == "aug_target") target = c->value;
            else if (c->type == "aug_op") {
                if (c->value == "+=") op = ast::AugAssignStmt::ADD;
                else if (c->value == "-=") op = ast::AugAssignStmt::SUB;
                else if (c->value == "*=") op = ast::AugAssignStmt::MUL;
                else if (c->value == "/=") op = ast::AugAssignStmt::DIV;
            }
            else if (c->type == "aug_value") value = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
        }
        return std::make_shared<ast::AugAssignStmt>(target, op, value);
    }

    // Pass/Break/Continue
    if (type == "pass") return std::make_shared<ast::PassStmt>();
    if (type == "break") return std::make_shared<ast::BreakStmt>();
    if (type == "continue") return std::make_shared<ast::ContinueStmt>();

    // Delete
    if (type == "stmt_delete") {
        std::vector<std::string> targets;
        for (auto& c : node->children) {
            if (c->type == "del_target") targets.push_back(c->value);
        }
        return std::make_shared<ast::DeleteStmt>(targets);
    }

    // Global
    if (type == "stmt_global") {
        std::vector<std::string> names;
        for (auto& c : node->children) names.push_back(c->value);
        return std::make_shared<ast::GlobalStmt>(names);
    }

    // Nonlocal
    if (type == "stmt_nonlocal") {
        std::vector<std::string> names;
        for (auto& c : node->children) names.push_back(c->value);
        return std::make_shared<ast::NonlocalStmt>(names);
    }

    // Assert
    if (type == "stmt_assert") {
        std::shared_ptr<ast::Expr> test, msg;
        for (auto& c : node->children) {
            auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
            if (!msg) test = e;
            else msg = e;
        }
        return std::make_shared<ast::AssertStmt>(test, msg);
    }

    // Raise
    if (type == "stmt_raise") {
        std::shared_ptr<ast::Expr> exc, cause;
        for (auto& c : node->children) {
            auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
            if (!cause) exc = e;
            else cause = e;
        }
        return std::make_shared<ast::RaiseStmt>(exc, cause);
    }

    // List comprehension and other expressions
    if (type == "list_expr") {
        std::vector<std::shared_ptr<ast::Expr>> elems;
        for (auto& c : node->children) {
            if (auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c))) elems.push_back(e);
        }
        return std::make_shared<ast::ListExpr>(elems);
    }

    // Call expression
    if (type == "call") {
        std::shared_ptr<ast::Expr> func;
        std::vector<std::shared_ptr<ast::Expr>> args;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[i]));
            if (i == 0) func = e;
            else args.push_back(e);
        }
        return std::make_shared<ast::CallExpr>(func, args);
    }

    // Attribute access
    if (type == "attr") {
        std::shared_ptr<ast::Expr> obj;
        std::string attr_name;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto c = node->children[i];
            auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
            if (!obj) obj = e;
            else attr_name = c->value;
        }
        return std::make_shared<ast::AttrExpr>(obj, attr_name);
    }

    // Subscript
    if (type == "subscript") {
        std::shared_ptr<ast::Expr> obj, slice;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[i]));
            if (!obj) obj = e;
            else slice = e;
        }
        return std::make_shared<ast::SubscriptExpr>(obj, slice);
    }

    // Lambda
    if (type == "lambda") {
        std::vector<ast::LambdaExpr::Arg> args;
        std::shared_ptr<ast::Expr> body;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto c = node->children[i];
            if (c->type == "arg") args.push_back({c->value});
            else body = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
        }
        return std::make_shared<ast::LambdaExpr>(args, body);
    }

    // Default: expression
    return build_expr(node);
}

std::shared_ptr<ast::Expr> build_expr(const std::shared_ptr<Value>& node) {
    if (!node) return nullptr;
    std::string type = node->type;

    if (type == "NAME") return std::make_shared<ast::Name>(node->value);
    if (type == "int_literal") {
        auto v = std::stoi(node->value);
        return std::make_shared<ast::IntLiteral>(static_cast<int>(v));
    }
    if (type == "float_literal") {
        return std::make_shared<ast::FloatLiteral>(std::stod(node->value));
    }
    if (type == "str_literal") {
        return std::make_shared<ast::StrLiteral>(node->value);
    }
    if (type == "True") return std::make_shared<ast::BoolLiteral>(true);
    if (type == "False") return std::make_shared<ast::BoolLiteral>(false);
    if (type == "None") return std::make_shared<ast::NoneLiteral>();
    if (type == "EllipsisLiteral") return std::make_shared<ast::EllipsisLiteral>();

    if (type == "add" || type == "sub" || type == "mul" || type == "div" ||
        type == "floor_div" || type == "mod" || type == "pow" ||
        type == "lt" || type == "le" || type == "gt" || type == "ge" ||
        type == "eq" || type == "ne" || type == "and" || type == "or") {
        ast::BinOpExpr::Op op = ast::BinOpExpr::ADD;
        if (type == "sub") op = ast::BinOpExpr::SUB;
        else if (type == "mul") op = ast::BinOpExpr::MUL;
        else if (type == "div") op = ast::BinOpExpr::DIV;
        else if (type == "floor_div") op = ast::BinOpExpr::FLOOR_DIV;
        else if (type == "mod") op = ast::BinOpExpr::MOD;
        else if (type == "pow") op = ast::BinOpExpr::POW;
        else if (type == "lt") op = ast::BinOpExpr::LT;
        else if (type == "le") op = ast::BinOpExpr::LE;
        else if (type == "gt") op = ast::BinOpExpr::GT;
        else if (type == "ge") op = ast::BinOpExpr::GE;
        else if (type == "eq") op = ast::BinOpExpr::EQ;
        else if (type == "ne") op = ast::BinOpExpr::NE;
        else if (type == "and") op = ast::BinOpExpr::AND;
        else if (type == "or") op = ast::BinOpExpr::OR;

        std::shared_ptr<ast::Expr> left, right;
        if (node->children.size() >= 2) {
            left = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[0]));
            right = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[1]));
        }
        return std::make_shared<ast::BinOpExpr>(op, std::move(left), std::move(right));
    }

    if (type == "neg") {
        std::shared_ptr<ast::Expr> operand;
        if (!node->children.empty()) operand = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[0]));
        return std::make_shared<ast::UnaryOpExpr>(ast::UnaryOpExpr::NEG, std::move(operand));
    }

    if (type == "list_expr") {
        std::vector<std::shared_ptr<ast::Expr>> elems;
        for (auto& c : node->children) {
            if (auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c))) elems.push_back(e);
        }
        return std::make_shared<ast::ListExpr>(elems);
    }

    if (type == "call") {
        std::shared_ptr<ast::Expr> func;
        std::vector<std::shared_ptr<ast::Expr>> args;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[i]));
            if (i == 0) func = e;
            else args.push_back(e);
        }
        return std::make_shared<ast::CallExpr>(func, args);
    }

    if (type == "attr") {
        std::shared_ptr<ast::Expr> obj;
        std::string attr_name;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto c = node->children[i];
            auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(c));
            if (!obj) obj = e;
            else attr_name = c->value;
        }
        return std::make_shared<ast::AttrExpr>(obj, attr_name);
    }

    if (type == "subscript") {
        std::shared_ptr<ast::Expr> obj, slice;
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto e = std::dynamic_pointer_cast<ast::Expr>(build_ast_node(node->children[i]));
            if (!obj) obj = e;
            else slice = e;
        }
        return std::make_shared<ast::SubscriptExpr>(obj, slice);
    }

    return nullptr;
}

ast::Module build_module(const std::shared_ptr<Value>& node) {
    std::vector<std::shared_ptr<ast::Stmt>> stmts;
    for (auto& child : node->children) {
        auto stmt = std::dynamic_pointer_cast<ast::Stmt>(build_ast_node(child));
        if (stmt) stmts.push_back(stmt);
    }
    auto mod = std::make_shared<ast::Module>(std::move(stmts));
    mod->classify_funcs_and_classes();
    return *mod;
}

} // anonymous namespace

// ===== Public API =====

std::shared_ptr<ast::Module> LarkParser::parse(const std::string& source) {
    // Read JSON from file (generated by lark_bridge.py)
    // For now, accept JSON directly as the source input
    try {
        JsonReader reader(source);
        auto root = reader.parse_object();
        auto mod = build_module(root);
        return std::make_shared<ast::Module>(std::vector<std::shared_ptr<ast::Stmt>>(
            mod.body().begin(), mod.body().end()));
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Parse error: ") + e.what());
    }
}

std::string LarkParser::get_error(const std::string& /*source*/, int line, int column) const {
    return "Parse error at line " + std::to_string(line) + ", column " + std::to_string(column);
}

} // namespace pyc::parser
