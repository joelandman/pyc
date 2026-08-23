// Typed AST -> typed SSA IR (INTERFACES.md §2 -> §3).
//
// Three things this must never become, per A3's directive:
//
//   * a dispatch chain on node-kind or method-name STRINGS. Dispatch is
//     std::visit over the generated variants, so a node kind CPython adds
//     stops the build (I4) instead of falling through an if-else chain.
//   * a special case per builtin. print/len/sum are not known here at all;
//     they are ordinary global loads followed by a call, which is why they
//     cannot diverge from each other the way the old tree's did.
//   * unboxed without proof. Nothing here unboxes: every value is a boxed
//     PyObject* obtained from a §4 symbol. I2 requires the boxed path to stay
//     correct beneath any future unboxing, so it is built first.
#include "pyc/ast/generated.hpp"
#include "pyc/ir/ir.hpp"
#include "pyc/rt/capi.hpp"

#include <string>
#include <map>

namespace pyc {
std::vector<std::string> function_locals(const std::vector<std::string>&,
                                        const std::vector<pyc::ast::stmt>&);
}

namespace pyc {
namespace {

using namespace pyc::ast;
using pyc::rt::Ownership;

class Lowerer {
public:
    Lowerer(ir::Module& m, DiagnosticSink& d) : mod_(m), diags_(d) {}

    bool lower_module(const ast::mod& node) {
        const Module* mm = std::get_if<Module>(&node.v);
        if (!mm) return err("only a module can be compiled", "mod", {});
        mod_.functions.push_back(ir::Function{"__main__", {}, {}, {}, 1});
        fn_idx_ = mod_.functions.size() - 1;
        cur()->blocks.push_back(ir::Block{"entry", {}});
        blk_ = 0;
        for (const stmt& s : mm->body)
            if (!lower_stmt(s)) return false;
        emit(ir::Instr{ir::Op::Return, {}, std::nullopt,
                       Ownership::NotAnObject, "", 0, 0, {}, std::nullopt});
        return true;
    }

private:
    ir::Module& mod_;
    DiagnosticSink& diags_;
    // An INDEX, not a pointer: mod_.functions grows while a nested def is
    // lowered, and a raw pointer into a vector that reallocates is a
    // use-after-free. ASAN caught exactly that here.
    std::size_t fn_idx_ = 0;
    // Named cur() rather than fn(): lower_call has a local `fn` holding the
    // callable, and a shadowed accessor there compiles into a call on a Value.
    ir::Function*       cur()       { return &mod_.functions[fn_idx_]; }
    const ir::Function* cur() const { return &mod_.functions[fn_idx_]; }
    std::size_t blk_ = 0;
    // name -> slot, for the function currently being lowered. Empty at module
    // level, where every name is a global.
    std::map<std::string, std::uint32_t> locals_;

    bool err(std::string msg, std::string construct, SourceLoc loc) {
        diags_.report(Diagnostic{Diagnostic::Severity::Error, std::move(msg),
                                 std::move(loc), std::move(construct), {}});
        return false;
    }
    // I1: a construct we cannot lower is a hard error naming it and its line,
    // never a silent skip. This is what makes a coverage gap show up as a P2
    // COMPILE_ERROR in the harness rather than a P0 wrong answer.
    bool unsupported(const char* what, const SourceLoc& loc) {
        return err(std::string("cannot compile ") + what + " yet", what, loc);
    }

    static bool is_terminator(ir::Op op) {
        return op == ir::Op::Br || op == ir::Op::CondBr
            || op == ir::Op::Return || op == ir::Op::ReturnErr;
    }
    bool terminated() const {
        const auto& is = cur()->blocks[blk_].instrs;
        return !is.empty() && is_terminator(is.back().op);
    }
    // Anything after a terminator is unreachable, and a block with two
    // terminators is malformed IR that LLVM's verifier rejects outright --
    // the same "Module verification failed" class the old tree hit. Dropping
    // it here is not an optimisation; it is what keeps the IR well-formed
    // when `break` or `continue` ends a block that the enclosing construct
    // then tries to branch out of.
    void emit(ir::Instr i) {
        if (terminated()) return;
        cur()->blocks[blk_].instrs.push_back(std::move(i));
    }

    // Every call goes through §4's table, so ownership is never guessed.
    ir::Value call_capi(const char* symbol, std::vector<ir::Value> args,
                        const SourceLoc& loc, bool* ok) {
        const rt::CApiSymbol* sym = rt::lookup(symbol);
        if (!sym) {
            *ok = err(std::string("no C-API contract recorded for '") + symbol
                      + "'; lowering may not emit it (INTERFACES §4)",
                      symbol, loc);
            return {};
        }
        if (!sym->emittable()) {
            *ok = err(std::string("C-API symbol '") + symbol + "' is not "
                      "emittable: " + (sym->banned
                          ? std::string(sym->ban_reason)
                          : std::string("its ownership contract is unrecorded")),
                      symbol, loc);
            return {};
        }
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        std::vector<ir::Value> saved = args;
        ir::Instr in{ir::Op::CallCApi, std::move(args), out,
                     sym->returns, symbol, 0, 0, loc, std::nullopt};
        if (sym->may_raise) in.on_error = make_landing_pad(loc);
        emit(std::move(in));
        // Release owned temporaries the call did not take. §4 records which
        // parameters STEAL, and decref'ing a stolen reference is a double
        // free -- which is why that table is consulted rather than assumed.
        for (std::size_t i = 0; i < saved.size(); ++i)
            if (owns(saved[i]) && !sym->steals_param(static_cast<int>(i)))
                release(saved[i], loc);
        *ok = true;
        return out;
    }

    // --- blocks ------------------------------------------------------------

    std::uint32_t new_block(std::string label) {
        cur()->blocks.push_back(ir::Block{std::move(label), {}});
        return static_cast<std::uint32_t>(cur()->blocks.size() - 1);
    }
    void set_block(std::uint32_t b) { blk_ = b; }

    // A landing pad releases exactly the temporaries this statement has taken
    // ownership of so far, then returns failure. Building it per call site
    // keeps the released set exact; identical pads are a later merge pass.
    //
    // This is the whole reason error edges exist: without it every raised
    // exception leaks whatever the statement was holding, which is the old
    // tree's frame-leak class reintroduced.
    std::uint32_t make_landing_pad(const SourceLoc& loc) {
        std::uint32_t here = blk_;
        std::uint32_t pad = new_block("unwind." + std::to_string(pad_n_++));
        set_block(pad);
        for (auto it = owned_.rbegin(); it != owned_.rend(); ++it)
            emit_decref(*it, loc);
        emit(ir::Instr{ir::Op::ReturnErr, {}, std::nullopt,
                       Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
        set_block(here);
        return pad;
    }

    // --- statements --------------------------------------------------------

    bool lower_stmt(const stmt& s) {
        // Temporaries do not outlive their statement, so the set an error
        // path must release is exactly what this statement has taken.
        auto saved = owned_;
        owned_.clear();
        bool ok = lower_stmt_inner(s);
        owned_ = saved;
        return ok;
    }

    bool lower_stmt_inner(const stmt& s) {
        bool ok = true;
        std::visit(ov{
            [&](const Expr& e)   { ok = lower_expr_stmt(e); },
            [&](const Assign& a) { ok = lower_assign(a); },
            [&](const Pass&)     { /* nothing to emit */ },
            // Everything else is a diagnostic, one arm each. No generic arm:
            // a node kind added upstream must break this build (I4).
            [&](const FunctionDef& n)      { ok = lower_functiondef(n); },
            [&](const AsyncFunctionDef& n) { ok = unsupported("async function definitions", n.loc); },
            [&](const ClassDef& n)         { ok = unsupported("class definitions", n.loc); },
            [&](const Return& n)           { ok = lower_return(n); },
            [&](const Delete& n)           { ok = unsupported("del", n.loc); },
            [&](const AugAssign& n)        { ok = unsupported("augmented assignment", n.loc); },
            [&](const AnnAssign& n)        { ok = unsupported("annotated assignment", n.loc); },
            [&](const For& n)              { ok = unsupported("for loops", n.loc); },
            [&](const AsyncFor& n)         { ok = unsupported("async for", n.loc); },
            [&](const While& n)            { ok = lower_while(n); },
            [&](const If& n)               { ok = lower_if(n); },
            [&](const With& n)             { ok = unsupported("with", n.loc); },
            [&](const AsyncWith& n)        { ok = unsupported("async with", n.loc); },
            [&](const Match& n)            { ok = unsupported("match", n.loc); },
            [&](const Raise& n)            { ok = unsupported("raise", n.loc); },
            [&](const Try& n)              { ok = unsupported("try", n.loc); },
            [&](const TryStar& n)          { ok = unsupported("try/except*", n.loc); },
            [&](const Assert& n)           { ok = unsupported("assert", n.loc); },
            [&](const Import& n)           { ok = unsupported("import", n.loc); },
            [&](const ImportFrom& n)       { ok = unsupported("from-import", n.loc); },
            [&](const Global&)             {
                // No code to emit: function_locals() has already excluded
                // these names, so every reference resolves to the global
                // path by construction. `nonlocal` is different -- it needs
                // closure cells -- and stays unsupported.
            },
            [&](const Nonlocal& n)         { ok = unsupported("nonlocal", n.loc); },
            [&](const Break& n)            {
                if (loops_.empty()) ok = err("break outside a loop", "break", n.loc);
                else emit(ir::Instr{ir::Op::Br, {}, std::nullopt,
                                    Ownership::NotAnObject, "",
                                    loops_.back().done, 0, n.loc, std::nullopt});
            },
            [&](const Continue& n)         {
                if (loops_.empty()) ok = err("continue outside a loop", "continue", n.loc);
                else emit(ir::Instr{ir::Op::Br, {}, std::nullopt,
                                    Ownership::NotAnObject, "",
                                    loops_.back().head, 0, n.loc, std::nullopt});
            },
            [&](const TypeAlias& n)        { ok = unsupported("type aliases", n.loc); },
        }, s.v);
        return ok;
    }

    // Truthiness goes through PyObject_IsTrue like everything else -- there is
    // no fast path for "obviously a bool", because that would be a proof we
    // have not made (I2).
    ir::Value lower_predicate(const expr& e, bool* ok) {
        ir::Value v = lower_expr(e, ok);
        if (!*ok) return {};
        ir::Value t = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
        SourceLoc loc{};
        emit(ir::Instr{ir::Op::IsTrue, {v}, t, Ownership::NotAnObject,
                       "PyObject_IsTrue", 0, 0, loc, make_landing_pad(loc)});
        if (owns(v)) release(v, loc);
        return t;
    }

    bool lower_functiondef(const FunctionDef& n) {
        if (!n.decorator_list.empty()) return unsupported("decorators", n.loc);
        const arguments& a = *n.args;
        if (!a.posonlyargs.empty()) return unsupported("positional-only parameters", n.loc);
        if (!a.kwonlyargs.empty())  return unsupported("keyword-only parameters", n.loc);
        if (!a.defaults.empty())    return unsupported("default arguments", n.loc);
        if (a.vararg)               return unsupported("*args", n.loc);
        if (a.kwarg)                return unsupported("**kwargs", n.loc);

        std::vector<std::string> params;
        for (const arg& p : a.args) params.push_back(p.arg);

        // Save the enclosing function's state: a nested def is lowered into a
        // separate ir::Function, and must not inherit the outer local map.
        std::size_t outer_fn = fn_idx_;
        std::size_t outer_blk = blk_;
        auto outer_locals = locals_;
        auto outer_owned = owned_;
        auto outer_loops = loops_;

        mod_.functions.push_back(ir::Function{n.name, params, {}, {}, 1});
        fn_idx_ = mod_.functions.size() - 1;
        cur()->locals = function_locals(params, n.body);
        locals_.clear();
        for (std::uint32_t i = 0; i < cur()->locals.size(); ++i)
            locals_[cur()->locals[i]] = i;
        owned_.clear();
        loops_.clear();
        cur()->blocks.push_back(ir::Block{"entry", {}});
        blk_ = 0;

        bool ok = true;
        for (const stmt& s2 : n.body) if (!lower_stmt(s2)) { ok = false; break; }
        if (ok) {
            // Falling off the end returns None, always.
            ir::Value none = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, none, Ownership::Owned, "",
                           0, 0, n.loc, std::nullopt});
            emit(ir::Instr{ir::Op::Return, {none}, std::nullopt,
                           Ownership::NotAnObject, "", 0, 0, n.loc, std::nullopt});
        }

        fn_idx_ = outer_fn; blk_ = outer_blk; locals_ = outer_locals;
        owned_ = outer_owned; loops_ = outer_loops;
        if (!ok) return false;

        // Bind the callable in the enclosing scope, by the same store path any
        // other assignment uses -- a def is not special.
        ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::MakeFunction, {}, fv, Ownership::Owned,
                       n.name, 0, 0, n.loc, make_landing_pad(n.loc)});
        mark_owned(fv);
        store_name(n.name, fv, n.loc);
        return true;
    }

    bool lower_return(const Return& n) {
        bool ok = true;
        ir::Value v;
        if (n.value) { v = lower_expr(**n.value, &ok); if (!ok) return false; }
        else {
            v = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, v, Ownership::Owned, "",
                           0, 0, n.loc, std::nullopt});
            mark_owned(v);
        }
        // The returned reference is handed to the caller, so it must NOT be
        // released here -- but every OTHER live temporary must be, or an early
        // return leaks exactly what a landing pad would have freed.
        for (auto it = owned_.rbegin(); it != owned_.rend(); ++it)
            if (it->id != v.id) emit_decref(*it, n.loc);
        emit(ir::Instr{ir::Op::Return, {v}, std::nullopt, Ownership::NotAnObject,
                       "", 0, 0, n.loc, std::nullopt});
        return true;
    }

    // One store path for every binding form: def, assignment, everything.
    void store_name(const std::string& name, const ir::Value& v, const SourceLoc& loc) {
        auto it = locals_.find(name);
        if (it != locals_.end())
            emit(ir::Instr{ir::Op::StoreLocal, {v}, std::nullopt,
                           Ownership::NotAnObject, name, it->second, 0, loc, std::nullopt});
        else
            emit(ir::Instr{ir::Op::StoreGlobal, {v}, std::nullopt,
                           Ownership::NotAnObject, name, 0, 0, loc, std::nullopt});
        if (owns(v)) release(v, loc);
    }

    bool lower_if(const If& n) {
        bool ok = true;
        ir::Value t = lower_predicate(*n.test, &ok);
        if (!ok) return false;
        std::uint32_t then_b = new_block("then");
        std::uint32_t else_b = new_block("else");
        std::uint32_t join_b = new_block("endif");
        emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt, Ownership::NotAnObject,
                       "", then_b, else_b, n.loc, std::nullopt});
        set_block(then_b);
        for (const stmt& s2 : n.body) if (!lower_stmt(s2)) return false;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join_b, 0, n.loc, std::nullopt});
        set_block(else_b);
        for (const stmt& s2 : n.orelse) if (!lower_stmt(s2)) return false;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join_b, 0, n.loc, std::nullopt});
        set_block(join_b);
        return true;
    }

    bool lower_while(const While& n) {
        if (!n.orelse.empty()) return unsupported("while/else", n.loc);
        std::uint32_t head = new_block("while.head");
        std::uint32_t body = new_block("while.body");
        std::uint32_t done = new_block("while.done");
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});
        set_block(head);
        bool ok = true;
        ir::Value t = lower_predicate(*n.test, &ok);
        if (!ok) return false;
        emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt, Ownership::NotAnObject,
                       "", body, done, n.loc, std::nullopt});
        set_block(body);
        loops_.push_back({head, done});
        for (const stmt& s2 : n.body) if (!lower_stmt(s2)) return false;
        loops_.pop_back();
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});
        set_block(done);
        return true;
    }

    std::uint32_t pad_n_ = 0;
    struct Loop { std::uint32_t head, done; };
    std::vector<Loop> loops_;

    bool lower_expr_stmt(const Expr& e) {
        bool ok = true;
        ir::Value v = lower_expr(*e.value, &ok);
        if (!ok) return false;
        // An expression statement's value is discarded. If we own it, we must
        // release it -- this is where a leak would otherwise begin.
        if (v.valid() && owns(v)) emit_decref(v, e.loc);
        return true;
    }

    bool lower_assign(const Assign& a) {
        bool ok = true;
        ir::Value v = lower_expr(*a.value, &ok);
        if (!ok) return false;
        if (a.targets.size() != 1)
            return unsupported("multiple assignment targets", a.loc);
        const Name* n = std::get_if<Name>(&a.targets[0].v);
        if (!n) return unsupported("assignment to anything but a bare name", a.loc);
        store_name(n->id, v, a.loc);
        return true;
    }

    // --- expressions -------------------------------------------------------

    std::vector<ir::Value> owned_;
    bool owns(const ir::Value& v) const {
        for (const ir::Value& o : owned_) if (o.id == v.id) return true;
        return false;
    }
    void mark_owned(const ir::Value& v) { owned_.push_back(v); }
    void emit_decref(const ir::Value& v, const SourceLoc& loc) {
        emit(ir::Instr{ir::Op::DecRef, {v}, std::nullopt,
                       Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
    }
    // Release and forget: a value released twice is a double free, so
    // ownership is dropped from the live set at the same moment.
    void release(const ir::Value& v, const SourceLoc& loc) {
        emit_decref(v, loc);
        for (std::size_t i = 0; i < owned_.size(); ++i)
            if (owned_[i].id == v.id) { owned_.erase(owned_.begin() + (long)i); break; }
    }

    ir::Value lower_expr(const expr& e, bool* ok) {
        ir::Value out;
        std::visit(ov{
            [&](const Constant& c) { out = lower_const(c, ok); },
            [&](const Name& n)     { out = lower_name(n, ok); },
            [&](const Call& c)     { out = lower_call(c, ok); },
            [&](const BinOp& b)    { out = lower_binop(b, ok); },
            [&](const BoolOp& n)        { *ok = unsupported("boolean operators", n.loc); },
            [&](const NamedExpr& n)     { *ok = unsupported("walrus", n.loc); },
            [&](const UnaryOp& n)       { *ok = unsupported("unary operators", n.loc); },
            [&](const Lambda& n)        { *ok = unsupported("lambda", n.loc); },
            [&](const IfExp& n)         { *ok = unsupported("conditional expressions", n.loc); },
            [&](const Dict& n)          { *ok = unsupported("dict literals", n.loc); },
            [&](const Set& n)           { *ok = unsupported("set literals", n.loc); },
            [&](const ListComp& n)      { *ok = unsupported("list comprehensions", n.loc); },
            [&](const SetComp& n)       { *ok = unsupported("set comprehensions", n.loc); },
            [&](const DictComp& n)      { *ok = unsupported("dict comprehensions", n.loc); },
            [&](const GeneratorExp& n)  { *ok = unsupported("generator expressions", n.loc); },
            [&](const Await& n)         { *ok = unsupported("await", n.loc); },
            [&](const Yield& n)         { *ok = unsupported("yield", n.loc); },
            [&](const YieldFrom& n)     { *ok = unsupported("yield from", n.loc); },
            [&](const Compare& n)       { *ok = unsupported("comparisons", n.loc); },
            [&](const FormattedValue& n){ *ok = unsupported("f-string interpolation", n.loc); },
            [&](const JoinedStr& n)     { *ok = unsupported("f-strings", n.loc); },
            [&](const TemplateStr& n)   { *ok = unsupported("t-strings", n.loc); },
            [&](const Interpolation& n) { *ok = unsupported("t-string interpolation", n.loc); },
            [&](const Attribute& n)     { *ok = unsupported("attribute access", n.loc); },
            [&](const Subscript& n)     { *ok = unsupported("subscripting", n.loc); },
            [&](const Starred& n)       { *ok = unsupported("star-unpacking", n.loc); },
            [&](const List& n)          { *ok = unsupported("list literals", n.loc); },
            [&](const Tuple& n)         { *ok = unsupported("tuple literals", n.loc); },
            [&](const Slice& n)         { *ok = unsupported("slices", n.loc); },
        }, e.v);
        return out;
    }

    ir::Value lower_const(const Constant& c, bool* ok) {
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        ir::Instr in{ir::Op::ConstNone, {}, out, Ownership::Owned, "", 0, 0,
                     c.loc, std::nullopt};
        std::visit(ov{
            // The literal stays DECIMAL TEXT all the way to codegen, which
            // hands it to PyLong_FromString. Nothing here can wrap.
            [&](const ConstBigInt& v) { in.op = ir::Op::ConstInt;   in.text = v.digits; },
            [&](const ConstFloat& v)  { in.op = ir::Op::ConstFloat; in.text = std::to_string(v.value); },
            [&](const ConstStr& v)    { in.op = ir::Op::ConstStr;   in.text = v.value; },
            [&](const ConstBytes& v)  { in.op = ir::Op::ConstBytes; in.text = v.value; },
            [&](const ConstBool& v)   { in.op = ir::Op::ConstBool;  in.text = v.value ? "True" : "False"; },
            [&](const ConstNone&)     { in.op = ir::Op::ConstNone; },
            [&](const ConstEllipsis&) { *ok = unsupported("Ellipsis literals", c.loc); },
            [&](const ConstComplex&)  { *ok = unsupported("complex literals", c.loc); },
            [&](const ConstTuple&)    { *ok = unsupported("tuple constants", c.loc); },
            [&](const ConstFrozenSet&){ *ok = unsupported("frozenset constants", c.loc); },
        }, c.value.v);
        if (!*ok) return {};
        emit(std::move(in));
        mark_owned(out);
        return out;
    }

    ir::Value lower_name(const Name& n, bool* ok) {
        // No builtin is special here. `print` is a global load like any other,
        // which is precisely why print/len/sum cannot diverge (I3).
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        auto it = locals_.find(n.id);
        if (it != locals_.end()) {
            // May raise UnboundLocalError: a local read before assignment is
            // an error, not a fallback to the global of the same name.
            emit(ir::Instr{ir::Op::LoadLocal, {}, out, Ownership::Owned,
                           n.id, it->second, 0, n.loc, make_landing_pad(n.loc)});
        } else {
            emit(ir::Instr{ir::Op::LoadGlobal, {}, out, Ownership::Owned,
                           n.id, 0, 0, n.loc, make_landing_pad(n.loc)});
        }
        mark_owned(out);
        *ok = true;
        return out;
    }

    ir::Value lower_call(const Call& c, bool* ok) {
        ir::Value fn = lower_expr(*c.func, ok);
        if (!*ok) return {};
        std::vector<ir::Value> args{fn};
        for (const expr& a : c.args) {
            ir::Value v = lower_expr(a, ok);
            if (!*ok) return {};
            args.push_back(v);
        }
        if (!c.keywords.empty()) { *ok = unsupported("keyword arguments", c.loc); return {}; }
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        std::vector<ir::Value> saved = args;
        emit(ir::Instr{ir::Op::CallObject, std::move(args), out,
                       Ownership::Owned, "", 0, 0, c.loc, make_landing_pad(c.loc)});
        // A Python-level call borrows its callable and arguments.
        for (const ir::Value& a : saved) if (owns(a)) release(a, c.loc);
        mark_owned(out);
        return out;
    }

    ir::Value lower_binop(const BinOp& b, bool* ok) {
        ir::Value l = lower_expr(*b.left, ok);   if (!*ok) return {};
        ir::Value r = lower_expr(*b.right, ok);  if (!*ok) return {};
        const char* sym = nullptr;
        std::visit(ov{
            [&](const Add&)      { sym = "PyNumber_Add"; },
            [&](const Sub&)      { sym = "PyNumber_Subtract"; },
            [&](const Mult&)     { sym = "PyNumber_Multiply"; },
            [&](const Div&)      { sym = "PyNumber_TrueDivide"; },
            [&](const FloorDiv&) { sym = "PyNumber_FloorDivide"; },
            [&](const Mod&)      { sym = "PyNumber_Remainder"; },
            [&](const Pow&)      { sym = "PyNumber_Power"; },
            [&](const LShift&)   { sym = "PyNumber_Lshift"; },
            [&](const RShift&)   { sym = "PyNumber_Rshift"; },
            [&](const BitOr&)    { sym = "PyNumber_Or"; },
            [&](const BitXor&)   { sym = "PyNumber_Xor"; },
            [&](const BitAnd&)   { sym = "PyNumber_And"; },
            [&](const MatMult&)  { sym = "PyNumber_MatrixMultiply"; },
        }, b.op.v);
        if (!sym) { *ok = unsupported("this binary operator", b.loc); return {}; }
        ir::Value out = call_capi(sym, {l, r}, b.loc, ok);
        if (*ok) mark_owned(out);
        return out;
    }
};

}  // namespace

bool lower_to_ir(const ast::mod& tree, const std::string& file,
                 ir::Module& out, DiagnosticSink& diags) {
    out.source_file = file;
    Lowerer l(out, diags);
    return l.lower_module(tree);
}

}  // namespace pyc
