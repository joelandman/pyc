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

    ir::Value call_capi_imm(const char* symbol, std::vector<ir::Value> args,
                            std::int64_t imm, int imm_pos, const SourceLoc& loc,
                            bool* ok, std::vector<ir::Value> consume = {}) {
        return call_capi(symbol, std::move(args), loc, ok, std::move(consume),
                         imm, imm_pos);
    }

    bool terminated() const {
        const auto& is = cur()->blocks[blk_].instrs;
        return !is.empty() && ir::is_terminator(is.back().op);
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
    // `consume` names the temporaries this expression created and is done
    // with. Passing a value does NOT consume it -- a borrowed argument stays
    // live, which is why a list survives across every SetItem.
    //
    // `imm_pos` is where a non-object immediate sits in the C signature.
    // Without it, steal lookups use the wrong parameter index:
    // PyList_SetItem(list, index, item) steals param 2, but the IR carries
    // the index out-of-band, so item would be checked as param 1 and its
    // stolen reference wrongly released -- a double free.
    ir::Value call_capi(const char* symbol, std::vector<ir::Value> args,
                        const SourceLoc& loc, bool* ok,
                        std::vector<ir::Value> consume = {},
                        std::int64_t imm = 0, int imm_pos = -1) {
        const rt::CApiSymbol* sym = rt::lookup(symbol);
        if (!sym) {
            *ok = err(std::string("no C-API contract recorded for '") + symbol
                      + "'; lowering may not emit it (INTERFACES §4)", symbol, loc);
            return {};
        }
        if (!sym->emittable()) {
            *ok = err(std::string("C-API symbol '") + symbol + "' is not emittable: "
                      + (sym->banned ? std::string(sym->ban_reason)
                                     : std::string("its ownership contract is unrecorded")),
                      symbol, loc);
            return {};
        }
        // The table records the true C arity. Emitting the wrong number of
        // arguments produces an LLVM declaration that disagrees with the real
        // function, and the callee then reads garbage -- PyNumber_Power takes
        // (base, exp, modulus) and a two-argument call segfaults. Checking
        // here turns that entire class into a compile-time diagnostic.
        int passed = (int)args.size() + (imm_pos >= 0 ? 1 : 0);
        if (passed != sym->arity) {
            *ok = err(std::string("internal: '") + symbol + "' takes "
                      + std::to_string(sym->arity) + " argument(s), lowering "
                      "passed " + std::to_string(passed), symbol, loc);
            return {};
        }
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        ir::Instr in{ir::Op::CallCApi, args, out, sym->returns, symbol,
                     0, 0, loc, std::nullopt};
        in.imm = imm;
        in.has_imm = (imm_pos >= 0);
        if (sym->may_raise) in.on_error = make_landing_pad(loc);
        emit(std::move(in));

        // A stolen reference now belongs to the callee: drop it from the live
        // set WITHOUT a decref.
        for (std::size_t i = 0; i < args.size(); ++i) {
            int cpos = (imm_pos >= 0 && (int)i >= imm_pos) ? (int)i + 1 : (int)i;
            if (sym->steals_param(cpos)) forget(args[i]);
        }
        for (const ir::Value& c : consume) if (owns(c)) release(c, loc);
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
    // Blocks that handle an exception rather than propagating it. A landing
    // pad inside a try must reach the handler dispatch, not leave the
    // function -- that is the whole difference between `try` and no `try`.
    std::vector<std::uint32_t> try_stack_;

    std::uint32_t make_landing_pad(const SourceLoc& loc) {
        std::uint32_t here = blk_;
        std::uint32_t pad = new_block("unwind." + std::to_string(pad_n_++));
        set_block(pad);
        for (auto it = owned_.rbegin(); it != owned_.rend(); ++it)
            emit_decref(*it, loc);
        if (try_stack_.empty()) {
            // Nothing catches here: release everything the frame holds and
            // propagate on the C-API convention.
            for (auto it = frame_owned_.rbegin(); it != frame_owned_.rend(); ++it)
                emit_decref(*it, loc);
            emit(ir::Instr{ir::Op::ReturnErr, {}, std::nullopt,
                           Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
        } else {
            // frame_owned_ is NOT released: the handler runs inside the same
            // frame and those values are still live for it.
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", try_stack_.back(), 0, loc, std::nullopt});
        }
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
            [&](const ClassDef& n)         { ok = lower_classdef(n); },
            [&](const Return& n)           { ok = lower_return(n); },
            [&](const Delete& n)           { ok = lower_delete(n); },
            [&](const AugAssign& n)        { ok = unsupported("augmented assignment", n.loc); },
            [&](const AnnAssign& n)        { ok = unsupported("annotated assignment", n.loc); },
            [&](const For& n)              { ok = lower_for(n); },
            [&](const AsyncFor& n)         { ok = unsupported("async for", n.loc); },
            [&](const While& n)            { ok = lower_while(n); },
            [&](const If& n)               { ok = lower_if(n); },
            [&](const With& n)             { ok = unsupported("with", n.loc); },
            [&](const AsyncWith& n)        { ok = unsupported("async with", n.loc); },
            [&](const Match& n)            { ok = unsupported("match", n.loc); },
            [&](const Raise& n)            {
                if (!n.exc) { ok = unsupported("bare raise", n.loc); return; }
                if (n.cause) { ok = unsupported("raise ... from", n.loc); return; }
                ir::Value e = lower_expr(**n.exc, &ok);
                if (!ok) return;
                emit(ir::Instr{ir::Op::Raise, {e}, std::nullopt,
                               Ownership::NotAnObject, "", 0, 0, n.loc,
                               make_landing_pad(n.loc)});
            },
            [&](const Try& n)              { ok = lower_try(n); },
            [&](const TryStar& n)          { ok = unsupported("try/except*", n.loc); },
            [&](const Assert& n)           { ok = unsupported("assert", n.loc); },
            [&](const Import& n)           { ok = lower_import(n); },
            [&](const ImportFrom& n)       { ok = lower_import_from(n); },
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
        // frame_owned_ must be saved too. A method lowered inside a class body
        // would otherwise inherit the class's namespace dict, and its landing
        // pads would emit a decref for a value defined in a DIFFERENT
        // function -- which LLVM rejects as "does not dominate all uses".
        auto outer_frame = frame_owned_;
        auto outer_class_ns = class_ns_;

        mod_.functions.push_back(ir::Function{n.name, params, {}, {}, 1});
        fn_idx_ = mod_.functions.size() - 1;
        const std::size_t fn_index = fn_idx_;
        cur()->locals = function_locals(params, n.body);
        locals_.clear();
        for (std::uint32_t i = 0; i < cur()->locals.size(); ++i)
            locals_[cur()->locals[i]] = i;
        owned_.clear();
        loops_.clear();
        frame_owned_.clear();
        class_ns_.clear();          // a method body is not a class body
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
        frame_owned_ = outer_frame; class_ns_ = outer_class_ns;
        if (!ok) return false;

        // Bind the callable in the enclosing scope, by the same store path any
        // other assignment uses -- a def is not special.
        ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        // Reference the function by INDEX, not name. Two classes can each
        // define `who`, and looking it up by name both picked the wrong one
        // and emitted a duplicate LLVM symbol.
        ir::Instr mk{ir::Op::MakeFunction, {}, fv, Ownership::Owned,
                     n.name, 0, 0, n.loc, make_landing_pad(n.loc)};
        mk.imm = (std::int64_t)fn_index;
        mk.has_imm = true;
        emit(std::move(mk));
        mark_owned(fv);
        // Inside a class body, a plain callable is not enough: a
        // PyCFunction in a class dict does NOT bind self, because it is not a
        // descriptor. `C().m()` then fails with "missing required argument
        // 'self'". PyInstanceMethod wraps any callable and binds the instance
        // on attribute access, which is what a Python function does natively.
        if (!class_ns_.empty()) {
            bool okm = true;
            ir::Value m = call_capi("PyInstanceMethod_New", {fv}, n.loc, &okm, {fv});
            if (!okm) return false;
            mark_owned(m);
            store_name(n.name, m, n.loc);
            return true;
        }
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

    // One store path for every assignable form. A target that is not a bare
    // name does not bind a variable at all -- it mutates an object through the
    // protocol -- which is why scope.cpp does not treat it as a binding.
    bool store_target(const expr& target, const ir::Value& v, const SourceLoc& loc) {
        bool ok = true;
        std::visit(ov{
            [&](const Name& n) { store_name(n.id, v, loc); },
            [&](const Attribute& n) {
                ir::Value obj = lower_expr(*n.value, &ok);
                if (!ok) return;
                ir::Value name = const_str(n.attr, loc);
                call_capi("PyObject_SetAttr", {obj, name, v}, loc, &ok, {obj, name});
                if (ok && owns(v)) release(v, loc);
            },
            [&](const Subscript& n) {
                ir::Value obj = lower_expr(*n.value, &ok);
                if (!ok) return;
                ir::Value key = lower_expr(*n.slice, &ok);
                if (!ok) return;
                call_capi("PyObject_SetItem", {obj, key, v}, loc, &ok, {obj, key});
                if (ok && owns(v)) release(v, loc);
            },
            [&](const Tuple& n)   { ok = unsupported("tuple unpacking", n.loc); },
            [&](const List& n)    { ok = unsupported("list unpacking", n.loc); },
            [&](const Starred& n) { ok = unsupported("starred assignment", n.loc); },
            // Not assignable; CPython rejects these at compile time, and so
            // must we -- with a diagnostic, never silently.
            [&](const BinOp& n){ ok = bad_target("an expression", n.loc); },
            [&](const BoolOp& n){ ok = bad_target("an expression", n.loc); },
            [&](const NamedExpr& n){ ok = bad_target("a walrus expression", n.loc); },
            [&](const UnaryOp& n){ ok = bad_target("an expression", n.loc); },
            [&](const Lambda& n){ ok = bad_target("a lambda", n.loc); },
            [&](const IfExp& n){ ok = bad_target("a conditional expression", n.loc); },
            [&](const Dict& n){ ok = bad_target("a dict literal", n.loc); },
            [&](const Set& n){ ok = bad_target("a set literal", n.loc); },
            [&](const ListComp& n){ ok = bad_target("a comprehension", n.loc); },
            [&](const SetComp& n){ ok = bad_target("a comprehension", n.loc); },
            [&](const DictComp& n){ ok = bad_target("a comprehension", n.loc); },
            [&](const GeneratorExp& n){ ok = bad_target("a generator", n.loc); },
            [&](const Await& n){ ok = bad_target("an await", n.loc); },
            [&](const Yield& n){ ok = bad_target("a yield", n.loc); },
            [&](const YieldFrom& n){ ok = bad_target("a yield", n.loc); },
            [&](const Compare& n){ ok = bad_target("a comparison", n.loc); },
            [&](const Call& n){ ok = bad_target("a call", n.loc); },
            [&](const FormattedValue& n){ ok = bad_target("an f-string", n.loc); },
            [&](const JoinedStr& n){ ok = bad_target("an f-string", n.loc); },
            [&](const TemplateStr& n){ ok = bad_target("a t-string", n.loc); },
            [&](const Interpolation& n){ ok = bad_target("a t-string", n.loc); },
            [&](const Constant& n){ ok = bad_target("a literal", n.loc); },
            [&](const Slice& n){ ok = bad_target("a slice", n.loc); },
        }, target.v);
        return ok;
    }

    // Enumerated, not `auto`. CPython does reject `del 1` in its own
    // compiler, but "upstream checks it" is exactly the assumption that has
    // been wrong three times in this project already, and a generic arm would
    // silently accept any node kind added later.
    bool lower_delete(const Delete& n) {
        bool ok = true;
        for (const expr& t : n.targets) {
            std::visit(ov{
                [&](const Attribute& a2) {
                    ir::Value obj = lower_expr(*a2.value, &ok);
                    if (!ok) return;
                    ir::Value nm = const_str(a2.attr, n.loc);
                    call_capi("PyObject_DelAttr", {obj, nm}, n.loc, &ok, {obj, nm});
                },
                [&](const Subscript& s2) {
                    ir::Value obj = lower_expr(*s2.value, &ok);
                    if (!ok) return;
                    ir::Value key = lower_expr(*s2.slice, &ok);
                    if (!ok) return;
                    call_capi("PyObject_DelItem", {obj, key}, n.loc, &ok, {obj, key});
                },
                [&](const Name& n2)      { ok = unsupported("del of a name", n2.loc); },
                [&](const Tuple& n2)     { ok = unsupported("del of a tuple target", n2.loc); },
                [&](const List& n2)      { ok = unsupported("del of a list target", n2.loc); },
                [&](const Starred& n2)   { ok = bad_target("a starred target", n2.loc); },
                [&](const BinOp& x){ ok = bad_target("an expression", x.loc); },
                [&](const BoolOp& x){ ok = bad_target("an expression", x.loc); },
                [&](const NamedExpr& x){ ok = bad_target("a walrus expression", x.loc); },
                [&](const UnaryOp& x){ ok = bad_target("an expression", x.loc); },
                [&](const Lambda& x){ ok = bad_target("a lambda", x.loc); },
                [&](const IfExp& x){ ok = bad_target("a conditional expression", x.loc); },
                [&](const Dict& x){ ok = bad_target("a dict literal", x.loc); },
                [&](const Set& x){ ok = bad_target("a set literal", x.loc); },
                [&](const ListComp& x){ ok = bad_target("a comprehension", x.loc); },
                [&](const SetComp& x){ ok = bad_target("a comprehension", x.loc); },
                [&](const DictComp& x){ ok = bad_target("a comprehension", x.loc); },
                [&](const GeneratorExp& x){ ok = bad_target("a generator", x.loc); },
                [&](const Await& x){ ok = bad_target("an await", x.loc); },
                [&](const Yield& x){ ok = bad_target("a yield", x.loc); },
                [&](const YieldFrom& x){ ok = bad_target("a yield", x.loc); },
                [&](const Compare& x){ ok = bad_target("a comparison", x.loc); },
                [&](const Call& x){ ok = bad_target("a call", x.loc); },
                [&](const FormattedValue& x){ ok = bad_target("an f-string", x.loc); },
                [&](const JoinedStr& x){ ok = bad_target("an f-string", x.loc); },
                [&](const TemplateStr& x){ ok = bad_target("a t-string", x.loc); },
                [&](const Interpolation& x){ ok = bad_target("a t-string", x.loc); },
                [&](const Constant& x){ ok = bad_target("a literal", x.loc); },
                [&](const Slice& x){ ok = bad_target("a slice", x.loc); },
            }, t.v);
            if (!ok) break;
        }
        return ok;
    }

    bool bad_target(const char* what, const SourceLoc& loc) {
        return err(std::string("cannot assign to ") + what, "assignment-target", loc);
    }

    // One store path for every binding form: def, assignment, everything.
    // While lowering a class body, every binding goes into the namespace dict
    // rather than a local or global slot. That is what makes `def m(self)`
    // inside a class become a method instead of a module-level function.
    std::vector<ir::Value> class_ns_;

    void store_name(const std::string& name, const ir::Value& v, const SourceLoc& loc) {
        if (!class_ns_.empty()) {
            bool ok = true;
            ir::Value key = const_str(name, loc);
            call_capi("PyObject_SetItem", {class_ns_.back(), key, v}, loc, &ok, {key});
            if (owns(v)) release(v, loc);
            return;
        }
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

    // Values owned by an enclosing construct rather than by the current
    // statement -- a loop's iterator, for instance. Landing pads must release
    // these too, or every exception raised inside a loop body leaks the
    // iterator. The per-statement reset alone cannot see them.
    std::vector<ir::Value> frame_owned_;

    ir::Value emit_import(const std::string& name, bool top_level,
                          const SourceLoc& loc) {
        ir::Value m = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        ir::Instr in{ir::Op::ImportModule, {}, m, Ownership::Owned, name,
                     0, 0, loc, make_landing_pad(loc)};
        in.imm = top_level ? 1 : 0;
        in.has_imm = true;
        emit(std::move(in));
        mark_owned(m);
        return m;
    }

    bool lower_try(const Try& n) {
        // Measured on the corpus before choosing scope: 207 try/except, 8
        // involving finally, no bare `except:` and no except*. finally must
        // run on every exit path -- normal, exceptional, break, continue,
        // return -- which is a different problem, so it is refused rather
        // than approximated.
        if (!n.finalbody.empty()) return unsupported("try/finally", n.loc);
        if (n.handlers.empty()) return unsupported("try without except", n.loc);

        std::uint32_t dispatch = new_block("except.dispatch");
        std::uint32_t after    = new_block("try.after");

        try_stack_.push_back(dispatch);
        bool ok = true;
        for (const stmt& s2 : n.body) if (!lower_stmt(s2)) { ok = false; break; }
        try_stack_.pop_back();
        if (!ok) return false;

        // `else` runs only when the body completed without raising.
        for (const stmt& s2 : n.orelse) if (!lower_stmt(s2)) return false;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, n.loc, std::nullopt});

        set_block(dispatch);
        // Takes the exception and CLEARS the error indicator, so the handler
        // runs with no exception set -- as CPython does.
        ir::Value exc = call_capi("PyErr_GetRaisedException", {}, n.loc, &ok);
        if (!ok) return false;
        mark_owned(exc);
        frame_owned_.push_back(exc);

        for (const excepthandler& h : n.handlers) {
            const ExceptHandler& eh = std::get<ExceptHandler>(h.v);
            std::uint32_t body_b = new_block("except.body");
            std::uint32_t next_b = new_block("except.next");
            if (eh.type) {
                ir::Value ty = lower_expr(**eh.type, &ok);
                if (!ok) return false;
                ir::Value m = call_capi("PyErr_GivenExceptionMatches", {exc, ty},
                                        n.loc, &ok, {ty});
                if (!ok) return false;
                emit(ir::Instr{ir::Op::CondBr, {m}, std::nullopt,
                               Ownership::NotAnObject, "", body_b, next_b,
                               n.loc, std::nullopt});
            } else {
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", body_b, 0, n.loc, std::nullopt});
            }
            set_block(body_b);
            if (eh.name) store_name(*eh.name, exc, n.loc);   // INCREFs; exc stays ours
            for (const stmt& s2 : eh.body) if (!lower_stmt(s2)) return false;
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});
            set_block(next_b);
        }

        // No handler matched: put the exception back and propagate. The
        // reference is STOLEN by SetRaisedException, so it must not be
        // released afterwards -- §4's table makes that automatic.
        frame_owned_.pop_back();
        call_capi("PyErr_SetRaisedException", {exc}, n.loc, &ok);
        if (!ok) return false;
        forget(exc);
        std::uint32_t pad = make_landing_pad(n.loc);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", pad, 0, n.loc, std::nullopt});

        set_block(after);
        return true;
    }

    bool lower_classdef(const ClassDef& n) {
        if (!n.decorator_list.empty()) return unsupported("class decorators", n.loc);
        if (!n.keywords.empty()) return unsupported("metaclass= and class keywords", n.loc);

        bool ok = true;
        // Bases first: they are ordinary expressions evaluated in the
        // ENCLOSING scope, before the body runs.
        ir::Value bases = call_capi_imm("PyTuple_New", {},
                                        (std::int64_t)n.bases.size(), 0, n.loc, &ok);
        if (!ok) return false;
        mark_owned(bases);
        for (std::size_t i = 0; i < n.bases.size(); ++i) {
            ir::Value b = lower_expr(n.bases[i], &ok);
            if (!ok) return false;
            // PyTuple_SetItem steals, so `b` must not be released after.
            call_capi_imm("PyTuple_SetItem", {bases, b}, (std::int64_t)i, 1, n.loc, &ok);
            if (!ok) return false;
        }

        ir::Value ns = call_capi("PyDict_New", {}, n.loc, &ok);
        if (!ok) return false;
        mark_owned(ns);

        // The body is lowered INLINE in the enclosing function, with stores
        // redirected into ns. A class body is not a closure: it executes once,
        // immediately, which is why it needs no separate ir::Function.
        class_ns_.push_back(ns);
        frame_owned_.push_back(ns);
        frame_owned_.push_back(bases);
        auto saved_locals = locals_;
        locals_.clear();                  // names in a class body are not fast locals
        for (const stmt& s2 : n.body)
            if (!lower_stmt(s2)) { class_ns_.pop_back(); return false; }
        locals_ = saved_locals;
        class_ns_.pop_back();
        frame_owned_.pop_back();
        frame_owned_.pop_back();

        ir::Value cls = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::BuildClass, {bases, ns}, cls, Ownership::Owned,
                       n.name, 0, 0, n.loc, make_landing_pad(n.loc)});
        mark_owned(cls);
        if (owns(bases)) release(bases, n.loc);
        if (owns(ns)) release(ns, n.loc);
        store_name(n.name, cls, n.loc);
        return true;
    }

    bool lower_import(const Import& n) {
        for (const alias& a : n.names) {
            if (a.asname) {
                // `import a.b as c` binds the LEAF module.
                ir::Value m = emit_import(a.name, false, n.loc);
                store_name(*a.asname, m, n.loc);
            } else {
                // `import a.b` binds the TOP-LEVEL package `a`, not `a.b` --
                // getting this backwards silently binds the wrong object.
                ir::Value m = emit_import(a.name, true, n.loc);
                std::string bind = a.name.substr(0, a.name.find('.'));
                store_name(bind, m, n.loc);
            }
        }
        return true;
    }

    bool lower_import_from(const ImportFrom& n) {
        if (n.level && *n.level > 0)
            return unsupported("relative imports", n.loc);
        for (const alias& a : n.names)
            if (a.name == "*") return unsupported("wildcard imports", n.loc);
        std::string mod = n.module ? *n.module : std::string();
        if (mod.empty()) return unsupported("import from an unnamed module", n.loc);

        bool ok = true;
        ir::Value m = emit_import(mod, false, n.loc);
        for (const alias& a : n.names) {
            ir::Value key = const_str(a.name, n.loc);
            ir::Value v = call_capi("PyObject_GetAttr", {m, key}, n.loc, &ok, {key});
            if (!ok) return false;
            mark_owned(v);
            store_name(a.asname ? *a.asname : a.name, v, n.loc);
        }
        if (owns(m)) release(m, n.loc);
        return true;
    }

    bool lower_for(const For& n) {
        if (!n.orelse.empty()) return unsupported("for/else", n.loc);
        if (!std::holds_alternative<Name>(n.target->v))
            return unsupported("unpacking in a for target", n.loc);
        const Name& tgt = std::get<Name>(n.target->v);

        bool ok = true;
        ir::Value seq = lower_expr(*n.iter, &ok);
        if (!ok) return false;
        // The ITERATOR PROTOCOL, not a type test. This is the single place
        // that decides how `for` traverses, so a user class defining
        // __iter__ works exactly as a list does -- the divergence the old
        // tree had between comprehensions and sum() is not expressible here.
        ir::Value it = call_capi("PyObject_GetIter", {seq}, n.loc, &ok, {seq});
        if (!ok) return false;
        mark_owned(it);
        frame_owned_.push_back(it);

        std::uint32_t head = new_block("for.head");
        std::uint32_t body = new_block("for.body");
        std::uint32_t done = new_block("for.done");
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});
        set_block(head);
        ir::Value item = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::IterNext, {it}, item, Ownership::Owned, "",
                       body, done, n.loc, make_landing_pad(n.loc)});

        set_block(body);
        mark_owned(item);
        store_name(tgt.id, item, n.loc);          // store_name releases item
        loops_.push_back({head, done});
        for (const stmt& s2 : n.body) if (!lower_stmt(s2)) return false;
        loops_.pop_back();
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});

        set_block(done);
        frame_owned_.pop_back();
        if (owns(it)) release(it, n.loc);
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
        return store_target(a.targets[0], v, a.loc);
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
    // Ownership transferred elsewhere: leave the live set with no decref.
    void forget(const ir::Value& v) {
        for (std::size_t i = 0; i < owned_.size(); ++i)
            if (owned_[i].id == v.id) { owned_.erase(owned_.begin() + (long)i); break; }
    }
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
            [&](const BoolOp& n)        { out = lower_boolop(n, ok); },
            [&](const NamedExpr& n)     { *ok = unsupported("walrus", n.loc); },
            [&](const UnaryOp& n)       { out = lower_unaryop(n, ok); },
            [&](const Lambda& n)        { *ok = unsupported("lambda", n.loc); },
            [&](const IfExp& n)         { out = lower_ifexp(n, ok); },
            [&](const Dict& n)          { out = lower_dict(n, ok); },
            [&](const Set& n)           { out = lower_set(n, ok); },
            [&](const ListComp& n)      { *ok = unsupported("list comprehensions", n.loc); },
            [&](const SetComp& n)       { *ok = unsupported("set comprehensions", n.loc); },
            [&](const DictComp& n)      { *ok = unsupported("dict comprehensions", n.loc); },
            [&](const GeneratorExp& n)  { *ok = unsupported("generator expressions", n.loc); },
            [&](const Await& n)         { *ok = unsupported("await", n.loc); },
            [&](const Yield& n)         { *ok = unsupported("yield", n.loc); },
            [&](const YieldFrom& n)     { *ok = unsupported("yield from", n.loc); },
            [&](const Compare& n)       { out = lower_compare(n, ok); },
            [&](const FormattedValue& n){ *ok = unsupported("f-string interpolation", n.loc); },
            [&](const JoinedStr& n)     { *ok = unsupported("f-strings", n.loc); },
            [&](const TemplateStr& n)   { *ok = unsupported("t-strings", n.loc); },
            [&](const Interpolation& n) { *ok = unsupported("t-string interpolation", n.loc); },
            [&](const Attribute& n)     { out = lower_attribute(n, ok); },
            [&](const Subscript& n)     { out = lower_subscript(n, ok); },
            [&](const Starred& n)       { *ok = unsupported("star-unpacking", n.loc); },
            [&](const List& n)          { out = lower_sequence(n.elts, "PyList", n.loc, ok); },
            [&](const Tuple& n)         { out = lower_sequence(n.elts, "PyTuple", n.loc, ok); },
            [&](const Slice& n)         { out = lower_slice(n, ok); },
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
        if (!c.keywords.empty()) return lower_call_kw(c, fn, args, ok);
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        std::vector<ir::Value> saved = args;
        emit(ir::Instr{ir::Op::CallObject, std::move(args), out,
                       Ownership::Owned, "", 0, 0, c.loc, make_landing_pad(c.loc)});
        // A Python-level call borrows its callable and arguments.
        for (const ir::Value& a : saved) if (owns(a)) release(a, c.loc);
        mark_owned(out);
        return out;
    }

    // Attribute and subscript go through the object PROTOCOL -- GetAttr and
    // GetItem -- never a type test. That is what makes them work identically
    // on a dict, a list, a numpy array and a user class with __getitem__ (I3).
    ir::Value lower_attribute(const Attribute& n, bool* ok) {
        ir::Value obj = lower_expr(*n.value, ok);
        if (!*ok) return {};
        ir::Value name = const_str(n.attr, n.loc);
        ir::Value out = call_capi("PyObject_GetAttr", {obj, name}, n.loc, ok, {obj, name});
        if (*ok) mark_owned(out);
        return out;
    }

    ir::Value lower_subscript(const Subscript& n, bool* ok) {
        ir::Value obj = lower_expr(*n.value, ok);
        if (!*ok) return {};
        ir::Value key = lower_expr(*n.slice, ok);
        if (!*ok) return {};
        ir::Value out = call_capi("PyObject_GetItem", {obj, key}, n.loc, ok, {obj, key});
        if (*ok) mark_owned(out);
        return out;
    }

    // A slice is an ordinary object built by PySlice_New; omitted bounds are
    // None, exactly as CPython represents them.
    ir::Value lower_slice(const Slice& n, bool* ok) {
        auto part = [&](const std::optional<Box<expr>>& e) -> ir::Value {
            if (e && *e) return lower_expr(**e, ok);
            ir::Value v = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, v, Ownership::Owned, "",
                           0, 0, n.loc, std::nullopt});
            mark_owned(v);
            return v;
        };
        ir::Value lo = part(n.lower);   if (!*ok) return {};
        ir::Value hi = part(n.upper);   if (!*ok) return {};
        ir::Value st = part(n.step);    if (!*ok) return {};
        ir::Value out = call_capi("PySlice_New", {lo, hi, st}, n.loc, ok, {lo, hi, st});
        if (*ok) mark_owned(out);
        return out;
    }

    // List and tuple share a shape: allocate, then SetItem each slot.
    //
    // PyList_SetItem and PyTuple_SetItem both STEAL the item reference, which
    // §4's table records -- so call_capi does NOT emit a decref for the
    // element afterwards. Emitting one would be a double free, and it is the
    // exact entry my curated steal list got wrong the first time.
    ir::Value lower_sequence(const std::vector<expr>& elts, const char* prefix,
                             const SourceLoc& loc, bool* ok) {
        for (const expr& e : elts)
            if (std::holds_alternative<Starred>(e.v)) {
                *ok = unsupported("star-unpacking in a literal", loc);
                return {};
            }
        std::string mk = std::string(prefix) + "_New";
        std::string set = std::string(prefix) + "_SetItem";
        ir::Value seq = call_capi_imm(mk.c_str(), {}, (std::int64_t)elts.size(), 0, loc, ok);
        if (!*ok) return {};
        mark_owned(seq);
        for (std::size_t i = 0; i < elts.size(); ++i) {
            ir::Value v = lower_expr(elts[i], ok);
            if (!*ok) return {};
            // index is C parameter 1; item is parameter 2 and IS stolen.
            call_capi_imm(set.c_str(), {seq, v}, (std::int64_t)i, 1, loc, ok);
            if (!*ok) return {};
        }
        return seq;
    }

    ir::Value lower_set(const Set& n, bool* ok) {
        for (const expr& e : n.elts)
            if (std::holds_alternative<Starred>(e.v)) {
                *ok = unsupported("star-unpacking in a set literal", n.loc);
                return {};
            }
        // PySet_New takes a nullable iterable; NULL means empty. The arity
        // check rejects the zero-argument form, which is how this was found.
        ir::Value s2 = call_capi("PySet_New", {ir::Value{}}, n.loc, ok);
        if (!*ok) return {};
        mark_owned(s2);
        for (const expr& e : n.elts) {
            ir::Value v = lower_expr(e, ok);
            if (!*ok) return {};
            // PySet_Add does NOT steal, so the element reference is ours to
            // release -- which call_capi does, from the same table.
            call_capi("PySet_Add", {s2, v}, n.loc, ok, {v});
            if (!*ok) return {};
        }
        return s2;
    }

    ir::Value lower_dict(const Dict& n, bool* ok) {
        // A null key marks `**mapping` unpacking (INTERFACES §2.3). It needs
        // PyDict_Update, not SetItem, so it is refused rather than mis-lowered.
        for (const auto& k : n.keys)
            if (!k || !*k) { *ok = unsupported("** unpacking in a dict literal", n.loc); return {}; }
        if (n.keys.size() != n.values.size()) {
            *ok = err("dict literal has mismatched keys and values", "Dict", n.loc);
            return {};
        }
        ir::Value d = call_capi("PyDict_New", {}, n.loc, ok);
        if (!*ok) return {};
        mark_owned(d);
        for (std::size_t i = 0; i < n.keys.size(); ++i) {
            ir::Value k = lower_expr(**n.keys[i], ok);
            if (!*ok) return {};
            ir::Value v = lower_expr(n.values[i], ok);
            if (!*ok) return {};
            call_capi("PyDict_SetItem", {d, k, v}, n.loc, ok, {k, v});
            if (!*ok) return {};
        }
        return d;
    }

    // Box a machine int (0/1) as a Python bool.
    ir::Value box_bool(const ir::Value& i, const SourceLoc& loc, bool* ok) {
        ir::Value out = call_capi("PyBool_FromLong", {i}, loc, ok);
        if (*ok) mark_owned(out);
        return out;
    }

    ir::Value int_not(const ir::Value& i, const SourceLoc& loc) {
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
        emit(ir::Instr{ir::Op::IntNot, {i}, out, Ownership::NotAnObject, "",
                       0, 0, loc, std::nullopt});
        return out;
    }

    ir::Value emit_phi(const std::vector<ir::Value>& vals,
                       const std::vector<std::uint32_t>& blocks,
                       const SourceLoc& loc) {
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        ir::Instr in{ir::Op::Phi, vals, out, Ownership::Owned, "",
                     0, 0, loc, std::nullopt};
        in.phi_blocks = blocks;
        // A phi must lead its block, so it is inserted rather than appended.
        cur()->blocks[blk_].instrs.insert(cur()->blocks[blk_].instrs.begin(),
                                          std::move(in));
        mark_owned(out);
        return out;
    }

    // `a and b` yields a if a is falsy, else b -- the VALUE, not a bool, and
    // b is not evaluated when a decides the answer. Both are observable, so
    // neither can be approximated with PyObject_IsTrue on the result.
    ir::Value lower_boolop(const BoolOp& n, bool* ok) {
        if (n.values.size() < 2) { *ok = err("degenerate boolean operator", "BoolOp", n.loc); return {}; }
        bool is_and = std::holds_alternative<And>(n.op.v);

        ir::Value acc = lower_expr(n.values[0], ok);
        if (!*ok) return {};
        for (std::size_t i = 1; i < n.values.size(); ++i) {
            ir::Value t = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
            emit(ir::Instr{ir::Op::IsTrue, {acc}, t, Ownership::NotAnObject,
                           "PyObject_IsTrue", 0, 0, n.loc, make_landing_pad(n.loc)});
            std::uint32_t rhs_b = new_block(is_and ? "and.rhs" : "or.rhs");
            std::uint32_t join  = new_block(is_and ? "and.join" : "or.join");
            std::uint32_t pred  = (std::uint32_t)blk_;
            // and: evaluate rhs when truthy. or: evaluate rhs when falsy.
            emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt, Ownership::NotAnObject,
                           "", is_and ? rhs_b : join, is_and ? join : rhs_b,
                           n.loc, std::nullopt});
            set_block(rhs_b);
            // The short-circuit value is dead on this path; the rhs value
            // becomes the result. Release exactly one of them per path.
            if (owns(acc)) emit_decref(acc, n.loc);
            ir::Value rhs = lower_expr(n.values[i], ok);
            if (!*ok) return {};
            std::uint32_t rhs_end = (std::uint32_t)blk_;
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", join, 0, n.loc, std::nullopt});
            set_block(join);
            forget(acc); forget(rhs);
            acc = emit_phi({acc, rhs}, {pred, rhs_end}, n.loc);
        }
        return acc;
    }

    ir::Value lower_ifexp(const IfExp& n, bool* ok) {
        ir::Value t = lower_predicate(*n.test, ok);
        if (!*ok) return {};
        std::uint32_t tb = new_block("ifexp.then");
        std::uint32_t fb = new_block("ifexp.else");
        std::uint32_t jb = new_block("ifexp.join");
        emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt, Ownership::NotAnObject,
                       "", tb, fb, n.loc, std::nullopt});
        set_block(tb);
        ir::Value a = lower_expr(*n.body, ok);   if (!*ok) return {};
        std::uint32_t ae = (std::uint32_t)blk_;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", jb, 0, n.loc, std::nullopt});
        set_block(fb);
        ir::Value b = lower_expr(*n.orelse, ok); if (!*ok) return {};
        std::uint32_t be = (std::uint32_t)blk_;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", jb, 0, n.loc, std::nullopt});
        set_block(jb);
        forget(a); forget(b);
        return emit_phi({a, b}, {ae, be}, n.loc);
    }

    ir::Value lower_compare(const Compare& n, bool* ok) {
        if (n.ops.size() != n.comparators.size() || n.ops.empty()) {
            *ok = err("malformed comparison", "Compare", n.loc);
            return {};
        }
        // `a < b < c` is `a < b and b < c` with b evaluated ONCE and c not
        // evaluated at all when the first test fails. Both are observable, so
        // it gets the same short-circuit shape as `and` -- never
        // `(a < b) < c`, which is a different program.
        if (n.ops.size() > 1) return lower_chained_compare(n, ok);
        ir::Value l = lower_expr(*n.left, ok);        if (!*ok) return {};
        ir::Value r = lower_expr(n.comparators[0], ok); if (!*ok) return {};

        ir::Value out;
        std::visit(ov{
            // Py_LT=0 Py_LE=1 Py_EQ=2 Py_NE=3 Py_GT=4 Py_GE=5
            [&](const Lt&)    { out = rich(l, r, 0, n.loc, ok); },
            [&](const LtE&)   { out = rich(l, r, 1, n.loc, ok); },
            [&](const Eq&)    { out = rich(l, r, 2, n.loc, ok); },
            [&](const NotEq&) { out = rich(l, r, 3, n.loc, ok); },
            [&](const Gt&)    { out = rich(l, r, 4, n.loc, ok); },
            [&](const GtE&)   { out = rich(l, r, 5, n.loc, ok); },
            [&](const Is&)    { out = identity(l, r, false, n.loc, ok); },
            [&](const IsNot&) { out = identity(l, r, true,  n.loc, ok); },
            // `x in y` is PySequence_Contains(y, x): the container first.
            [&](const In&)    { out = contains(r, l, false, n.loc, ok); },
            [&](const NotIn&) { out = contains(r, l, true,  n.loc, ok); },
        }, n.ops[0].v);
        return out;
    }

    ir::Value compare_one(const ir::Value& l, const ir::Value& r,
                          const cmpop& op, const SourceLoc& loc, bool* ok) {
        ir::Value out;
        std::visit(ov{
            [&](const Lt&)    { out = rich(l, r, 0, loc, ok); },
            [&](const LtE&)   { out = rich(l, r, 1, loc, ok); },
            [&](const Eq&)    { out = rich(l, r, 2, loc, ok); },
            [&](const NotEq&) { out = rich(l, r, 3, loc, ok); },
            [&](const Gt&)    { out = rich(l, r, 4, loc, ok); },
            [&](const GtE&)   { out = rich(l, r, 5, loc, ok); },
            [&](const Is&)    { out = identity(l, r, false, loc, ok); },
            [&](const IsNot&) { out = identity(l, r, true,  loc, ok); },
            [&](const In&)    { out = contains(r, l, false, loc, ok); },
            [&](const NotIn&) { out = contains(r, l, true,  loc, ok); },
        }, op.v);
        return out;
    }

    // Chained comparisons, built NESTED rather than as a loop.
    //
    // `a < b < c` is `a < b and b < c` with b evaluated once and c not
    // evaluated when the first test fails. The obvious loop shape is wrong:
    // the comparand is defined inside the branch that evaluates it, so
    // releasing it after the join reaches a path where it was never defined.
    // LLVM's verifier says so as "Instruction does not dominate all uses".
    //
    // Nesting fixes it structurally -- every release sits in a block dominated
    // by the definition, and the short-circuit path gets its own block so it
    // can release the comparand it is abandoning.
    ir::Value lower_chained_compare(const Compare& n, bool* ok) {
        ir::Value left = lower_expr(*n.left, ok);
        if (!*ok) return {};
        ir::Value r = chain_step(n, 0, left, ok);
        if (*ok && owns(left)) release(left, n.loc);
        return r;
    }

    ir::Value chain_step(const Compare& n, std::size_t i, const ir::Value& left,
                         bool* ok) {
        ir::Value rhs = lower_expr(n.comparators[i], ok);
        if (!*ok) return {};
        ir::Value r = compare_one_keep(left, rhs, n.ops[i], n.loc, ok);
        if (!*ok) return {};
        if (i + 1 == n.ops.size()) {
            if (owns(rhs)) release(rhs, n.loc);
            return r;
        }
        ir::Value t = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
        emit(ir::Instr{ir::Op::IsTrue, {r}, t, Ownership::NotAnObject,
                       "PyObject_IsTrue", 0, 0, n.loc, make_landing_pad(n.loc)});
        std::uint32_t next_b  = new_block("cmp.next");
        std::uint32_t short_b = new_block("cmp.short");
        std::uint32_t join    = new_block("cmp.join");
        emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt, Ownership::NotAnObject,
                       "", next_b, short_b, n.loc, std::nullopt});

        // Short-circuit: the result is r, and the comparand is abandoned here.
        set_block(short_b);
        if (owns(rhs)) emit_decref(rhs, n.loc);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join, 0, n.loc, std::nullopt});

        set_block(next_b);
        if (owns(r)) emit_decref(r, n.loc);
        ir::Value inner = chain_step(n, i + 1, rhs, ok);
        if (!*ok) return {};
        if (owns(rhs)) release(rhs, n.loc);
        std::uint32_t inner_end = (std::uint32_t)blk_;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join, 0, n.loc, std::nullopt});

        set_block(join);
        forget(r); forget(inner);
        return emit_phi({r, inner}, {short_b, inner_end}, n.loc);
    }

    // Like compare_one but does not consume its operands.
    ir::Value compare_one_keep(const ir::Value& l, const ir::Value& r,
                               const cmpop& op, const SourceLoc& loc, bool* ok) {
        bool lo = owns(l), ro = owns(r);
        if (lo) forget(l);
        if (ro) forget(r);
        ir::Value out = compare_one(l, r, op, loc, ok);
        if (lo) mark_owned(l);
        if (ro) mark_owned(r);
        return out;
    }

    ir::Value rich(const ir::Value& l, const ir::Value& r, std::int64_t opid,
                   const SourceLoc& loc, bool* ok) {
        ir::Value out = call_capi("PyObject_RichCompare", {l, r}, loc, ok,
                                  {l, r}, opid, 2);
        if (*ok) mark_owned(out);
        return out;
    }

    ir::Value identity(const ir::Value& l, const ir::Value& r, bool negate,
                       const SourceLoc& loc, bool* ok) {
        ir::Value b = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
        emit(ir::Instr{ir::Op::Is, {l, r}, b, Ownership::NotAnObject, "",
                       0, 0, loc, std::nullopt});
        if (owns(l)) release(l, loc);
        if (owns(r)) release(r, loc);
        if (negate) b = int_not(b, loc);
        return box_bool(b, loc, ok);
    }

    ir::Value contains(const ir::Value& seq, const ir::Value& item, bool negate,
                       const SourceLoc& loc, bool* ok) {
        ir::Value i = call_capi("PySequence_Contains", {seq, item}, loc, ok,
                                {seq, item});
        if (!*ok) return {};
        if (negate) i = int_not(i, loc);
        return box_bool(i, loc, ok);
    }

    ir::Value lower_unaryop(const UnaryOp& n, bool* ok) {
        ir::Value v = lower_expr(*n.operand, ok);
        if (!*ok) return {};
        ir::Value out;
        std::visit(ov{
            [&](const USub&)   { out = call_capi("PyNumber_Negative", {v}, n.loc, ok, {v});
                                 if (*ok) mark_owned(out); },
            [&](const UAdd&)   { out = call_capi("PyNumber_Positive", {v}, n.loc, ok, {v});
                                 if (*ok) mark_owned(out); },
            [&](const Invert&) { out = call_capi("PyNumber_Invert", {v}, n.loc, ok, {v});
                                 if (*ok) mark_owned(out); },
            // `not x` is truthiness-based, so it goes through the protocol and
            // yields a real bool -- never a bitwise trick on the operand.
            [&](const Not&)    { ir::Value i = call_capi("PyObject_Not", {v}, n.loc, ok, {v});
                                 if (*ok) out = box_bool(i, n.loc, ok); },
        }, n.op.v);
        return out;
    }

    ir::Value const_str(const std::string& text, const SourceLoc& loc) {
        ir::Value v = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::ConstStr, {}, v, Ownership::Owned, text,
                       0, 0, loc, std::nullopt});
        mark_owned(v);
        return v;
    }

    // A call with keywords goes through PyObject_Call(callable, args, kwargs)
    // rather than the vectorcall fast path: building the tuple and dict is the
    // straightforward form, and correctness comes first (CHARTER §1).
    ir::Value lower_call_kw(const Call& c, const ir::Value& fn,
                            const std::vector<ir::Value>& all, bool* ok) {
        for (const keyword& k : c.keywords)
            if (!k.arg) { *ok = unsupported("** unpacking in a call", c.loc); return {}; }

        std::size_t npos = all.size() - 1;              // all[0] is the callable
        ir::Value tup = call_capi_imm("PyTuple_New", {}, (std::int64_t)npos, 0, c.loc, ok);
        if (!*ok) return {};
        mark_owned(tup);
        for (std::size_t i = 0; i < npos; ++i) {
            // PyTuple_SetItem steals, so the positional value must NOT be
            // released afterwards -- §4's table is what makes that automatic.
            call_capi_imm("PyTuple_SetItem", {tup, all[i + 1]},
                          (std::int64_t)i, 1, c.loc, ok);
            if (!*ok) return {};
        }
        ir::Value kw = call_capi("PyDict_New", {}, c.loc, ok);
        if (!*ok) return {};
        mark_owned(kw);
        for (const keyword& k : c.keywords) {
            ir::Value key = const_str(*k.arg, c.loc);
            ir::Value val = lower_expr(*k.value, ok);
            if (!*ok) return {};
            call_capi("PyDict_SetItem", {kw, key, val}, c.loc, ok, {key, val});
            if (!*ok) return {};
        }
        ir::Value out = call_capi("PyObject_Call", {fn, tup, kw}, c.loc, ok,
                                  {fn, tup, kw});
        if (*ok) mark_owned(out);
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
        // PyNumber_Power is ternary: `a ** b` is pow(a, b, None).
        if (std::string(sym) == "PyNumber_Power") {
            ir::Value none = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, none, Ownership::Owned, "",
                           0, 0, b.loc, std::nullopt});
            mark_owned(none);
            ir::Value out2 = call_capi(sym, {l, r, none}, b.loc, ok, {l, r, none});
            if (*ok) mark_owned(out2);
            return out2;
        }
        ir::Value out = call_capi(sym, {l, r}, b.loc, ok, {l, r});
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
