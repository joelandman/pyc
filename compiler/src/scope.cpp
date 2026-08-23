// Scope analysis: which names are LOCAL to a function.
//
// Python's rule is not "assigned before use" but "assigned anywhere in the
// body makes it local throughout", which is why
//
//     x = 1
//     def f():
//         print(x)     # UnboundLocalError, not 1
//         x = 2
//
// raises rather than printing. Getting this wrong does not crash -- it reads
// the global and returns a plausible wrong answer, which is the P0 class.
//
// Exhaustive over stmt by construction: a binding form CPython adds must break
// this build rather than silently not binding (I4).
#include "pyc/ast/generated.hpp"

#include <set>
#include <string>
#include <vector>

namespace pyc {

using namespace pyc::ast;

namespace {

struct Collector {
    std::set<std::string> bound;      // assigned somewhere in this scope
    std::set<std::string> declared_global;
    std::set<std::string> declared_nonlocal;

    // Only a bare Name binds. `a.b = 1` and `a[0] = 1` mutate an object; they
    // do not create a local.
    void target(const expr& e) {
        std::visit(ov{
            [&](const Name& n)    { bound.insert(n.id); },
            [&](const Tuple& t)   { for (const expr& x : t.elts) target(x); },
            [&](const List& l)    { for (const expr& x : l.elts) target(x); },
            [&](const Starred& s) { if (s.value) target(*s.value); },
            [&](const Attribute&) {}, [&](const Subscript&) {},
            // Not binding forms; listed so the visitor stays exhaustive.
            [&](const BinOp&){}, [&](const BoolOp&){}, [&](const NamedExpr& n){
                if (n.target) target(*n.target); },
            [&](const UnaryOp&){}, [&](const Lambda&){}, [&](const IfExp&){},
            [&](const Dict&){}, [&](const Set&){}, [&](const ListComp&){},
            [&](const SetComp&){}, [&](const DictComp&){}, [&](const GeneratorExp&){},
            [&](const Await&){}, [&](const Yield&){}, [&](const YieldFrom&){},
            [&](const Compare&){}, [&](const Call&){}, [&](const FormattedValue&){},
            [&](const JoinedStr&){}, [&](const TemplateStr&){}, [&](const Interpolation&){},
            [&](const Constant&){}, [&](const Slice&){},
        }, e.v);
    }

    void block(const std::vector<stmt>& body) { for (const stmt& s : body) one(s); }

    void one(const stmt& s) {
        std::visit(ov{
            [&](const Assign& n)    { for (const expr& t : n.targets) target(t); },
            [&](const AugAssign& n) { if (n.target) target(*n.target); },
            [&](const AnnAssign& n) { if (n.target && n.value) target(*n.target); },
            [&](const For& n)       { if (n.target) target(*n.target);
                                      block(n.body); block(n.orelse); },
            [&](const AsyncFor& n)  { if (n.target) target(*n.target);
                                      block(n.body); block(n.orelse); },
            [&](const While& n)     { block(n.body); block(n.orelse); },
            [&](const If& n)        { block(n.body); block(n.orelse); },
            [&](const With& n)      { for (const withitem& w : n.items)
                                          if (w.optional_vars) target(**w.optional_vars);
                                      block(n.body); },
            [&](const AsyncWith& n) { for (const withitem& w : n.items)
                                          if (w.optional_vars) target(**w.optional_vars);
                                      block(n.body); },
            [&](const Try& n)       { block(n.body); block(n.orelse); block(n.finalbody);
                                      for (const excepthandler& h : n.handlers) handler(h); },
            [&](const TryStar& n)   { block(n.body); block(n.orelse); block(n.finalbody);
                                      for (const excepthandler& h : n.handlers) handler(h); },
            [&](const Match& n)     { for (const match_case& c : n.cases) block(c.body); },
            // A def or class binds ITS OWN NAME here, but its body is a
            // separate scope and must not be descended into.
            [&](const FunctionDef& n)      { bound.insert(n.name); },
            [&](const AsyncFunctionDef& n) { bound.insert(n.name); },
            [&](const ClassDef& n)         { bound.insert(n.name); },
            [&](const Import& n)      { for (const alias& a : n.names)
                                            bound.insert(a.asname ? *a.asname
                                                                  : a.name.substr(0, a.name.find('.'))); },
            [&](const ImportFrom& n)  { for (const alias& a : n.names)
                                            bound.insert(a.asname ? *a.asname : a.name); },
            [&](const Global& n)      { for (const std::string& x : n.names) declared_global.insert(x); },
            [&](const Nonlocal& n)    { for (const std::string& x : n.names) declared_nonlocal.insert(x); },
            [&](const TypeAlias& n)   { if (n.name) target(*n.name); },
            [&](const Delete& n)      { for (const expr& t : n.targets) target(t); },
            [&](const Return&){}, [&](const Expr&){}, [&](const Pass&){},
            [&](const Break&){}, [&](const Continue&){}, [&](const Raise&){},
            [&](const Assert&){},
        }, s.v);
    }

    void handler(const excepthandler& h) {
        std::visit(ov{
            [&](const ExceptHandler& eh) {
                if (eh.name) bound.insert(*eh.name);
                block(eh.body);
            },
        }, h.v);
    }
};

}  // namespace

// Locals = everything bound in the body, plus the parameters, minus anything
// explicitly declared global or nonlocal.
std::vector<std::string> function_locals(const std::vector<std::string>& params,
                                         const std::vector<stmt>& body) {
    Collector c;
    c.block(body);
    std::vector<std::string> out = params;
    for (const std::string& n : c.bound) {
        if (c.declared_global.count(n) || c.declared_nonlocal.count(n)) continue;
        bool dup = false;
        for (const std::string& p : out) if (p == n) { dup = true; break; }
        if (!dup) out.push_back(n);
    }
    return out;
}

// Names read anywhere inside NESTED functions/lambdas/comprehensions of this
// body. Intersected with the enclosing function's locals, this is exactly the
// set that must live in cells: a variable is only a cell variable because
// something inner reads it.
//
// Deliberately over-approximate -- it collects every Name, ignoring whether an
// inner scope rebinds it. An extra cell costs an indirection; a missing one
// silently reads the wrong variable.
namespace {
struct NestedReads {
    std::set<std::string>& out;

    void expr_(const expr& e, bool inside) {
        std::visit(ov{
            [&](const Name& n){ if (inside) out.insert(n.id); },
            [&](const Lambda& n){ expr_(*n.body, true); },
            [&](const ListComp& n){ comp(n.generators, &*n.elt, nullptr); },
            [&](const SetComp& n){ comp(n.generators, &*n.elt, nullptr); },
            [&](const DictComp& n){ comp(n.generators, &*n.value, &*n.key); },
            [&](const GeneratorExp& n){ comp(n.generators, &*n.elt, nullptr); },
            [&](const BinOp& n){ expr_(*n.left, inside); expr_(*n.right, inside); },
            [&](const BoolOp& n){ for (const expr& v : n.values) expr_(v, inside); },
            [&](const UnaryOp& n){ expr_(*n.operand, inside); },
            [&](const Compare& n){ expr_(*n.left, inside);
                                   for (const expr& v : n.comparators) expr_(v, inside); },
            [&](const Call& n){ expr_(*n.func, inside);
                                for (const expr& v : n.args) expr_(v, inside);
                                for (const keyword& k : n.keywords) expr_(*k.value, inside); },
            [&](const Attribute& n){ expr_(*n.value, inside); },
            [&](const Subscript& n){ expr_(*n.value, inside); expr_(*n.slice, inside); },
            [&](const IfExp& n){ expr_(*n.test, inside); expr_(*n.body, inside);
                                 expr_(*n.orelse, inside); },
            [&](const Tuple& n){ for (const expr& v : n.elts) expr_(v, inside); },
            [&](const List& n){ for (const expr& v : n.elts) expr_(v, inside); },
            [&](const Set& n){ for (const expr& v : n.elts) expr_(v, inside); },
            [&](const Dict& n){ for (const auto& k : n.keys) if (k && *k) expr_(**k, inside);
                                for (const expr& v : n.values) expr_(v, inside); },
            [&](const Starred& n){ expr_(*n.value, inside); },
            [&](const Slice& n){ if (n.lower && *n.lower) expr_(**n.lower, inside);
                                 if (n.upper && *n.upper) expr_(**n.upper, inside);
                                 if (n.step && *n.step) expr_(**n.step, inside); },
            [&](const NamedExpr& n){ expr_(*n.value, inside); },
            [&](const Await& n){ expr_(*n.value, inside); },
            [&](const Yield& n){ if (n.value) expr_(**n.value, inside); },
            [&](const YieldFrom& n){ expr_(*n.value, inside); },
            [&](const FormattedValue& n){ expr_(*n.value, inside); },
            [&](const JoinedStr& n){ for (const expr& v : n.values) expr_(v, inside); },
            [&](const TemplateStr& n){ for (const expr& v : n.values) expr_(v, inside); },
            [&](const Interpolation& n){ expr_(*n.value, inside); },
            [&](const Constant&){},
        }, e.v);
    }

    void comp(const std::vector<comprehension>& gens, const expr* elt, const expr* key) {
        if (elt) expr_(*elt, true);
        if (key) expr_(*key, true);
        for (const comprehension& g : gens) {
            expr_(*g.iter, true);
            for (const expr& c : g.ifs) expr_(c, true);
        }
    }

    void stmt_(const stmt& s, bool inside) {
        std::visit(ov{
            // Entering a nested function: everything below is "inside".
            [&](const FunctionDef& n){ for (const stmt& y : n.body) stmt_(y, true);
                                       for (const expr& d : n.decorator_list) expr_(d, inside); },
            [&](const AsyncFunctionDef& n){ for (const stmt& y : n.body) stmt_(y, true); },
            [&](const ClassDef& n){ for (const stmt& y : n.body) stmt_(y, true); },
            [&](const Expr& n){ expr_(*n.value, inside); },
            [&](const Return& n){ if (n.value) expr_(**n.value, inside); },
            [&](const Assign& n){ expr_(*n.value, inside);
                                  for (const expr& t : n.targets) expr_(t, inside); },
            [&](const AugAssign& n){ expr_(*n.value, inside); expr_(*n.target, inside); },
            [&](const AnnAssign& n){ if (n.value) expr_(**n.value, inside); },
            [&](const If& n){ expr_(*n.test, inside);
                              for (const stmt& y : n.body) stmt_(y, inside);
                              for (const stmt& y : n.orelse) stmt_(y, inside); },
            [&](const While& n){ expr_(*n.test, inside);
                                 for (const stmt& y : n.body) stmt_(y, inside);
                                 for (const stmt& y : n.orelse) stmt_(y, inside); },
            [&](const For& n){ expr_(*n.iter, inside); expr_(*n.target, inside);
                               for (const stmt& y : n.body) stmt_(y, inside);
                               for (const stmt& y : n.orelse) stmt_(y, inside); },
            [&](const AsyncFor& n){ for (const stmt& y : n.body) stmt_(y, inside); },
            [&](const With& n){ for (const withitem& w : n.items) expr_(*w.context_expr, inside);
                                for (const stmt& y : n.body) stmt_(y, inside); },
            [&](const AsyncWith& n){ for (const stmt& y : n.body) stmt_(y, inside); },
            [&](const Try& n){ for (const stmt& y : n.body) stmt_(y, inside);
                               for (const stmt& y : n.orelse) stmt_(y, inside);
                               for (const stmt& y : n.finalbody) stmt_(y, inside);
                               for (const excepthandler& h : n.handlers)
                                   for (const stmt& y : std::get<ExceptHandler>(h.v).body)
                                       stmt_(y, inside); },
            [&](const TryStar& n){ for (const stmt& y : n.body) stmt_(y, inside); },
            [&](const Match& n){ expr_(*n.subject, inside);
                                 for (const match_case& c : n.cases)
                                     for (const stmt& y : c.body) stmt_(y, inside); },
            [&](const Raise& n){ if (n.exc) expr_(**n.exc, inside); },
            [&](const Assert& n){ expr_(*n.test, inside); },
            [&](const Delete& n){ for (const expr& t : n.targets) expr_(t, inside); },
            [&](const Import&){}, [&](const ImportFrom&){}, [&](const Global&){},
            [&](const Nonlocal&){}, [&](const Pass&){}, [&](const Break&){},
            [&](const Continue&){}, [&](const TypeAlias&){},
        }, s.v);
    }
};
}  // namespace

std::set<std::string> nested_reads(const std::vector<stmt>& body) {
    std::set<std::string> out;
    NestedReads nr{out};
    for (const stmt& s : body) nr.stmt_(s, false);
    return out;
}

// Names a lambda/comprehension body reads, for the same purpose.
std::set<std::string> nested_reads_expr(const expr& e) {
    std::set<std::string> out;
    NestedReads nr{out};
    nr.expr_(e, true);
    return out;
}

}  // namespace pyc
