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

}  // namespace pyc
