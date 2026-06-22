// ast.h - Abstract Syntax Tree nodes for PyC Python compiler
// All member variables use trailing underscores to avoid name conflicts with getters
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <variant>

namespace pyc::ast {

class Node { public: virtual ~Node() = default; };
class Expr : public Node {};
class Stmt : public Node {};

// ===== Literals =====

class IntLiteral final : public Expr {
    int val_;
public:
    explicit IntLiteral(int v) : val_(v) {}
    int value() const { return val_; }
};
class FloatLiteral final : public Expr {
    double val_;
public:
    explicit FloatLiteral(double v) : val_(v) {}
    double value() const { return val_; }
};
class StrLiteral final : public Expr {
    std::string val_;
public:
    explicit StrLiteral(std::string v) : val_(std::move(v)) {}
    const std::string& value() const { return val_; }
};
class BoolLiteral final : public Expr {
    bool val_;
public:
    explicit BoolLiteral(bool v) : val_(v) {}
    bool value() const { return val_; }
};
class NoneLiteral final : public Expr {};
class EllipsisLiteral final : public Expr {};

// ===== Identifiers =====

class Name final : public Expr {
    std::string id_;
public:
    explicit Name(std::string id) : id_(std::move(id)) {}
    const std::string& id() const { return id_; }
};

// ===== Expressions =====

class BinOpExpr final : public Expr {
public:
    enum Op { ADD, SUB, MUL, DIV, FLOOR_DIV, MOD, POW, LT, LE, GT, GE, EQ, NE, AND, OR, IN, IS };
    Op op_;
    std::shared_ptr<Expr> left_, right_;
    BinOpExpr(Op o, std::shared_ptr<Expr> l, std::shared_ptr<Expr> r)
        : op_(o), left_(std::move(l)), right_(std::move(r)) {}
    Op op() const { return op_; }
    Expr* left() const { return left_.get(); }
    Expr* right() const { return right_.get(); }
};

class UnaryOpExpr final : public Expr {
public:
    enum Op { NEG, NOT, UPLUS };
    Op op_;
    std::shared_ptr<Expr> operand_;
    UnaryOpExpr(Op o, std::shared_ptr<Expr> ope)
        : op_(o), operand_(std::move(ope)) {}
    Op op() const { return op_; }
    Expr* operand() const { return operand_.get(); }
};

class CallExpr final : public Expr {
    std::shared_ptr<Expr> func_;
    std::vector<std::shared_ptr<Expr>> args_;
public:
    CallExpr(std::shared_ptr<Expr> f, std::vector<std::shared_ptr<Expr>> a)
        : func_(std::move(f)), args_(std::move(a)) {}
    Expr* func() const { return func_.get(); }
    const auto& args() const { return args_; }
};

class AttrExpr final : public Expr {
    std::shared_ptr<Expr> obj_;
    std::string attr_;
public:
    AttrExpr(std::shared_ptr<Expr> o, std::string a)
        : obj_(std::move(o)), attr_(std::move(a)) {}
    Expr* obj() const { return obj_.get(); }
    const std::string& attr() const { return attr_; }
};

class SubscriptExpr final : public Expr {
    std::shared_ptr<Expr> obj_, slice_;
public:
    SubscriptExpr(std::shared_ptr<Expr> o, std::shared_ptr<Expr> s)
        : obj_(std::move(o)), slice_(std::move(s)) {}
    Expr* obj() const { return obj_.get(); }
    Expr* slice() const { return slice_.get(); }
};

class ListExpr final : public Expr {
    std::vector<std::shared_ptr<Expr>> elems_;
public:
    explicit ListExpr(std::vector<std::shared_ptr<Expr>> e) : elems_(std::move(e)) {}
    const auto& elems() const { return elems_; }
};

class DictLiteral final : public Expr {
    std::vector<std::pair<std::shared_ptr<Expr>, std::shared_ptr<Expr>>> pairs_;
public:
    explicit DictLiteral(std::vector<std::pair<std::shared_ptr<Expr>, std::shared_ptr<Expr>>> p)
        : pairs_(std::move(p)) {}
    const auto& pairs() const { return pairs_; }
};

class TupleExpr final : public Expr {
    std::vector<std::shared_ptr<Expr>> elems_;
public:
    explicit TupleExpr(std::vector<std::shared_ptr<Expr>> e) : elems_(std::move(e)) {}
    const auto& elems() const { return elems_; }
};

// ===== Statements =====

class AssignStmt final : public Stmt {
    std::vector<std::string> targets_;
    std::shared_ptr<Expr> value_;
public:
    AssignStmt(std::vector<std::string> t, std::shared_ptr<Expr> v)
        : targets_(std::move(t)), value_(std::move(v)) {}
    const auto& targets() const { return targets_; }
    Expr* value() const { return value_.get(); }
};

class AugAssignStmt final : public Stmt {
    std::string target_;
public:
    enum Op { ADD, SUB, MUL, DIV };
    Op op_;
    std::shared_ptr<Expr> value_;
    AugAssignStmt(std::string t, Op o, std::shared_ptr<Expr> v)
        : target_(std::move(t)), op_(o), value_(std::move(v)) {}
    const std::string& target() const { return target_; }
    Expr* value() const { return value_.get(); }
};

class ReturnStmt final : public Stmt {
    std::shared_ptr<Expr> ret_val_;
public:
    explicit ReturnStmt(std::shared_ptr<Expr> v) : ret_val_(std::move(v)) {}
    bool has_value() const { return ret_val_ != nullptr; }
    Expr* value() const { return ret_val_.get(); }
};

class IfStmt final : public Stmt {
    std::shared_ptr<Expr> test_;
    std::vector<std::shared_ptr<Stmt>> body_, orelse_;
public:
    IfStmt(std::shared_ptr<Expr> t, std::vector<std::shared_ptr<Stmt>> b,
           std::vector<std::shared_ptr<Stmt>> o)
        : test_(std::move(t)), body_(std::move(b)), orelse_(std::move(o)) {}
    Expr* test() const { return test_.get(); }
    const auto& body() const { return body_; }
    const auto& orelse() const { return orelse_; }
};

class ForStmt final : public Stmt {
    std::string target_;
    std::shared_ptr<Expr> iter_;
    std::vector<std::shared_ptr<Stmt>> body_;
public:
    ForStmt(std::string t, std::shared_ptr<Expr> i, std::vector<std::shared_ptr<Stmt>> b)
        : target_(std::move(t)), iter_(std::move(i)), body_(std::move(b)) {}
    const std::string& target() const { return target_; }
    Expr* iter() const { return iter_.get(); }
    const auto& body() const { return body_; }
};

class WhileStmt final : public Stmt {
    std::shared_ptr<Expr> test_;
    std::vector<std::shared_ptr<Stmt>> body_;
public:
    WhileStmt(std::shared_ptr<Expr> t, std::vector<std::shared_ptr<Stmt>> b)
        : test_(std::move(t)), body_(std::move(b)) {}
    Expr* test() const { return test_.get(); }
    const auto& body() const { return body_; }
};

class FunctionDef final : public Stmt {
    std::string name_;
public:
    struct Arg { std::string name; };
    std::vector<Arg> args_;
    std::vector<std::shared_ptr<Stmt>> body_;
    FunctionDef(std::string n, std::vector<Arg> a, std::vector<std::shared_ptr<Stmt>> b)
        : name_(std::move(n)), args_(std::move(a)), body_(std::move(b)) {}
    const std::string& name() const { return name_; }
    const auto& args() const { return args_; }
    const auto& body() const { return body_; }
};

class ClassDef final : public Stmt {
    std::string name_;
    std::vector<std::string> bases_;
    std::vector<std::shared_ptr<Stmt>> body_;
public:
    ClassDef(std::string n, std::vector<std::string> b, std::vector<std::shared_ptr<Stmt>> bd)
        : name_(std::move(n)), bases_(std::move(b)), body_(std::move(bd)) {}
    const std::string& name() const { return name_; }
    const auto& bases() const { return bases_; }
    const auto& body() const { return body_; }
};

class PassStmt final : public Stmt {};
class BreakStmt final : public Stmt {};
class ContinueStmt final : public Stmt {};

class DeleteStmt final : public Stmt {
    std::vector<std::string> targets_;
public:
    explicit DeleteStmt(std::vector<std::string> t) : targets_(std::move(t)) {}
    const auto& targets() const { return targets_; }
};

class GlobalStmt final : public Stmt {
    std::vector<std::string> names_;
public:
    explicit GlobalStmt(std::vector<std::string> n) : names_(std::move(n)) {}
    const auto& names() const { return names_; }
};

class NonlocalStmt final : public Stmt {
    std::vector<std::string> names_;
public:
    explicit NonlocalStmt(std::vector<std::string> n) : names_(std::move(n)) {}
    const auto& names() const { return names_; }
};

class AssertStmt final : public Stmt {
    std::shared_ptr<Expr> test_, msg_;
public:
    AssertStmt(std::shared_ptr<Expr> t, std::shared_ptr<Expr> m)
        : test_(std::move(t)), msg_(std::move(m)) {}
    Expr* test() const { return test_.get(); }
    Expr* msg() const { return msg_.get(); }
};

class RaiseStmt final : public Stmt {
    std::shared_ptr<Expr> exc_, cause_;
public:
    RaiseStmt(std::shared_ptr<Expr> e, std::shared_ptr<Expr> c)
        : exc_(std::move(e)), cause_(std::move(c)) {}
    Expr* exc() const { return exc_.get(); }
    Expr* cause() const { return cause_.get(); }
};

class WithItem {
public:
    std::shared_ptr<Expr> context_;
    std::string optional_vars_;
    WithItem(std::shared_ptr<Expr> ctx, std::string vars)
        : context_(std::move(ctx)), optional_vars_(std::move(vars)) {}
};

class WithStmt final : public Stmt {
    std::vector<WithItem> items_;
    std::vector<std::shared_ptr<Stmt>> body_;
public:
    WithStmt(std::vector<WithItem> it, std::vector<std::shared_ptr<Stmt>> b)
        : items_(std::move(it)), body_(std::move(b)) {}
    const auto& items() const { return items_; }
    const auto& body() const { return body_; }
};

class ExceptClause {
public:
    std::shared_ptr<Expr> type_;
    std::vector<std::shared_ptr<Stmt>> body_;
    bool is_star = false;
    std::string var_name;
    ExceptClause(std::shared_ptr<Expr> t,
                 std::vector<std::shared_ptr<Stmt>> b, bool star = false)
        : type_(std::move(t)), body_(std::move(b)), is_star(star) {}
};

class TryStmt final : public Stmt {
    std::vector<std::shared_ptr<Stmt>> body_, orelse_, finalbody_;
    std::vector<ExceptClause> handlers_;
public:
    TryStmt(std::vector<std::shared_ptr<Stmt>> b, std::vector<ExceptClause> h,
            std::vector<std::shared_ptr<Stmt>> o, std::vector<std::shared_ptr<Stmt>> f)
        : body_(std::move(b)), orelse_(std::move(o)), finalbody_(std::move(f)), handlers_(std::move(h)) {}
    const auto& body() const { return body_; }
    const auto& handlers() const { return handlers_; }
    const auto& orelse() const { return orelse_; }
    const auto& finalbody() const { return finalbody_; }
};

class MatchStmt final : public Stmt {
    std::shared_ptr<Expr> subject_;
public:
    struct Case {
        std::vector<std::shared_ptr<Expr>> patterns;
        std::shared_ptr<Expr> guard;
        std::vector<std::shared_ptr<Stmt>> body;
    };
    std::vector<Case> cases_;
    MatchStmt(std::shared_ptr<Expr> s, std::vector<Case> c)
        : subject_(std::move(s)), cases_(std::move(c)) {}
    Expr* subject() const { return subject_.get(); }
    const auto& cases() const { return cases_; }
};

class ImportFrom {
public:
    std::string module_name_;
    int level = 0;
    std::vector<std::string> names_;  // For "from X import a, b, c"
    ImportFrom(std::string m, int l) : module_name_(std::move(m)), level(l) {}
};

class ImportStmt final : public Stmt {
    std::shared_ptr<ImportFrom> from_import_;
public:
    ImportStmt(std::shared_ptr<ImportFrom> f) : from_import_(std::move(f)) {}
    const ImportFrom* from_import_ptr() const { return from_import_.get(); }
};

class YieldExpr final : public Expr {
    std::shared_ptr<Expr> val_;
    bool is_yield_from_ = false;
public:
    YieldExpr(std::shared_ptr<Expr> v, bool from = false)
        : val_(std::move(v)), is_yield_from_(from) {}
    Expr* value() const { return val_.get(); }
    bool is_from() const { return is_yield_from_; }
};

class LambdaExpr final : public Expr {
public:
    struct Arg {
        std::string name;
    };
    std::vector<Arg> args_;
    std::shared_ptr<Expr> body_;
public:
    LambdaExpr(std::vector<Arg> a, std::shared_ptr<Expr> b)
        : args_(std::move(a)), body_(std::move(b)) {}
    const auto& args() const { return args_; }
    Expr* body() const { return body_.get(); }
};

class ListComp final : public Expr {
    std::shared_ptr<Expr> elt_;
public:
    struct Comprehension {
        std::string target;
        std::shared_ptr<Expr> iterable;
        std::vector<std::shared_ptr<Expr>> ifs;
    };
    std::vector<Comprehension> comprehensions_;
    ListComp(std::shared_ptr<Expr> e, std::vector<Comprehension> c)
        : elt_(std::move(e)), comprehensions_(std::move(c)) {}
    Expr* elt() const { return elt_.get(); }
    const auto& comprehensions() const { return comprehensions_; }
};

class SetComp final : public Expr {
    std::shared_ptr<Expr> elt_;
public:
    struct Comprehension {
        std::string target;
        std::shared_ptr<Expr> iterable;
        std::vector<std::shared_ptr<Expr>> ifs;
    };
    std::vector<Comprehension> comprehensions_;
    SetComp(std::shared_ptr<Expr> e, std::vector<Comprehension> c)
        : elt_(std::move(e)), comprehensions_(std::move(c)) {}
    Expr* elt() const { return elt_.get(); }
    const auto& comprehensions() const { return comprehensions_; }
};

class GenExpr final : public Expr {
    std::shared_ptr<Expr> elt_;
public:
    struct Comprehension {
        std::string target;
        std::shared_ptr<Expr> iterable;
        std::vector<std::shared_ptr<Expr>> ifs;
    };
    std::vector<Comprehension> comprehensions_;
    GenExpr(std::shared_ptr<Expr> e, std::vector<Comprehension> c)
        : elt_(std::move(e)), comprehensions_(std::move(c)) {}
    Expr* elt() const { return elt_.get(); }
    const auto& comprehensions() const { return comprehensions_; }
};

class DictComp final : public Expr {
    std::shared_ptr<Expr> key_, val_;
public:
    struct Comprehension {
        std::string target;
        std::shared_ptr<Expr> iterable;
        std::vector<std::shared_ptr<Expr>> ifs;
    };
    std::vector<Comprehension> comprehensions_;
    DictComp(std::shared_ptr<Expr> k, std::shared_ptr<Expr> v, std::vector<Comprehension> c)
        : key_(std::move(k)), val_(std::move(v)), comprehensions_(std::move(c)) {}
    Expr* key() const { return key_.get(); }
    Expr* value() const { return val_.get(); }
    const auto& comprehensions() const { return comprehensions_; }
};

class StarredExpr final : public Expr {
    std::shared_ptr<Expr> val_;
public:
    explicit StarredExpr(std::shared_ptr<Expr> v) : val_(std::move(v)) {}
    Expr* value() const { return val_.get(); }
};

class AwaitExpr final : public Expr {
    std::shared_ptr<Expr> val_;
public:
    explicit AwaitExpr(std::shared_ptr<Expr> v) : val_(std::move(v)) {}
    Expr* value() const { return val_.get(); }
};

// ===== Module =====

class Module final : public Node {
    std::vector<std::shared_ptr<Stmt>> body_;
    std::vector<std::shared_ptr<FunctionDef>> functions_;
    std::vector<std::shared_ptr<ClassDef>> classes_;
public:
    Module() = default;
    Module(std::vector<std::shared_ptr<Stmt>> b)
        : body_(std::move(b)), functions_(), classes_() {}
    const auto& body() const { return body_; }
    const auto& functions() const { return functions_; }
    const auto& classes() const { return classes_; }
    void classify_funcs_and_classes();
};

} // namespace pyc::ast
