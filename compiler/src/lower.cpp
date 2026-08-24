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
#include "pyc/genexp.hpp"
#include "pyc/ir/ir.hpp"
#include "pyc/rt/capi.hpp"

#include <string>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace pyc {
std::vector<std::string> function_locals(const std::vector<std::string>&,
                                        const std::vector<pyc::ast::stmt>&);
std::set<std::string> nested_reads(const std::vector<pyc::ast::stmt>&);
std::set<std::string> declared_nonlocals(const std::vector<pyc::ast::stmt>&);
std::set<std::string> all_reads(const std::vector<pyc::ast::stmt>&);
std::set<std::string> nested_reads_expr(const pyc::ast::expr&);
}

namespace pyc {
namespace {

using namespace pyc::ast;
using pyc::rt::Ownership;

class Lowerer {
public:
    Lowerer(ir::Module& m, DiagnosticSink& d,
            const std::vector<GenexpEntry>& gx)
        : mod_(m), diags_(d), genexps_(gx) {}

    // Generator expressions carry a code object CPython compiled at build
    // time, keyed by source position (rebuild/GENERATORS.md).
    const std::vector<GenexpEntry>& genexps_;
    const GenexpEntry* find_genexp(const SourceLoc& loc) const {
        for (const GenexpEntry& g : genexps_)
            if (g.line == loc.line && g.col == loc.col) return &g;
        return nullptr;
    }

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
    // Names in THIS function whose slot holds a cell: either a local something
    // nested reads (a cellvar) or a name inherited from an enclosing function
    // (a freevar). Both are read with cell.get rather than load.local.
    std::map<std::string, std::uint32_t> cells_;
    // Enclosing functions' cell maps, outermost first. A free name resolves by
    // searching outward, which is what makes capture work at any depth.
    std::vector<std::map<std::string, std::uint32_t>> enclosing_cells_;

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
        // Record the steals on the instruction itself, from the same table
        // that drives the forget() below, so the IR dump says so.
        for (std::size_t i = 0; i < args.size(); ++i) {
            int cp = (imm_pos >= 0 && (int)i >= imm_pos) ? (int)i + 1 : (int)i;
            if (sym->steals_param(cp)) in.stolen.push_back(args[i].id);
        }
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
            // Nothing catches here, so the exception LEAVES this function:
            // record where. Without this a compiled binary printed only the
            // exception type and message, with no file, line or function at
            // all -- the thing you most need from a crash in a deployed
            // program. Only on paths that actually propagate, and the line is
            // this pad's own, so it names the failing operation.
            emit(ir::Instr{ir::Op::AddTraceback, {}, std::nullopt,
                           Ownership::NotAnObject,
                           fn_idx_ == 0 ? "<module>" : cur()->name,
                           0, 0, loc, std::nullopt});
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
            [&](const AsyncFunctionDef& n) { ok = lower_async_functiondef(n); },
            [&](const ClassDef& n)         { ok = lower_classdef(n); },
            [&](const Return& n)           { ok = lower_return(n); },
            [&](const Delete& n)           { ok = lower_delete(n); },
            [&](const AugAssign& n)        { ok = lower_augassign(n); },
            [&](const AnnAssign& n)        { ok = unsupported("annotated assignment", n.loc); },
            [&](const For& n)              { ok = lower_for(n); },
            [&](const AsyncFor& n)         { ok = unsupported("async for", n.loc); },
            [&](const While& n)            { ok = lower_while(n); },
            [&](const If& n)               { ok = lower_if(n); },
            [&](const With& n)             { ok = lower_with(n); },
            [&](const AsyncWith& n)        { ok = unsupported("async with", n.loc); },
            [&](const Match& n)            { ok = unsupported("match", n.loc); },
            [&](const Raise& n)            {
                if (!n.exc) {
                    // Re-raise whatever is currently being handled.
                    call_capi("pyc_rt_reraise", {}, n.loc, &ok);
                    if (!ok) return;
                    std::uint32_t pad = make_landing_pad(n.loc);
                    emit(ir::Instr{ir::Op::Br, {}, std::nullopt,
                                   Ownership::NotAnObject, "", pad, 0, n.loc,
                                   std::nullopt});
                    return;
                }
                if (n.cause) { ok = unsupported("raise ... from", n.loc); return; }
                ir::Value e = lower_expr(**n.exc, &ok);
                if (!ok) return;
                emit(ir::Instr{ir::Op::Raise, {e}, std::nullopt,
                               Ownership::NotAnObject, "", 0, 0, n.loc,
                               make_landing_pad(n.loc)});
            },
            [&](const Try& n)              { ok = lower_try(n); },
            [&](const TryStar& n)          { ok = unsupported("try/except*", n.loc); },
            [&](const Assert& n)           {
                // `assert c, m` is `if not c: raise AssertionError(m)`. The
                // message expression is evaluated ONLY on failure.
                ir::Value t = lower_predicate(*n.test, &ok);
                if (!ok) return;
                std::uint32_t fail_b = new_block("assert.fail");
                std::uint32_t okb    = new_block("assert.ok");
                emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt,
                               Ownership::NotAnObject, "", okb, fail_b, n.loc,
                               std::nullopt});
                set_block(fail_b);
                ir::Value m = ir::Value{};
                if (n.msg) { m = lower_expr(**n.msg, &ok); if (!ok) return; }
                call_capi("pyc_rt_assert_fail", {m}, n.loc, &ok,
                          m.valid() ? std::vector<ir::Value>{m} : std::vector<ir::Value>{});
                if (!ok) return;
                std::uint32_t pad = make_landing_pad(n.loc);
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", pad, 0, n.loc, std::nullopt});
                set_block(okb);
            },
            [&](const Import& n)           { ok = lower_import(n); },
            [&](const ImportFrom& n)       { ok = lower_import_from(n); },
            [&](const Global&)             {
                // No code to emit: function_locals() has already excluded
                // these names, so every reference resolves to the global
                // path by construction. `nonlocal` is different -- it needs
                // closure cells -- and stays unsupported.
            },
            [&](const Nonlocal&)           {
                // No code: function_locals() already excludes these names, and
                // the closure analysis gives them a cell slot, so reads and
                // WRITES both go through the cell -- which is the whole point
                // of nonlocal as opposed to a plain free variable.
            },
            [&](const Break& n)            { ok = finish_jump(true, n.loc); },
            [&](const Continue& n)         { ok = finish_jump(false, n.loc); },
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

    struct Loop { std::uint32_t head, done; };

    // Saved lowering state while a nested function is built.
    struct FnScope {
        std::size_t fn, blk;
        std::map<std::string, std::uint32_t> locals;
        std::vector<ir::Value> owned, frame, class_ns;
        std::vector<Loop> loops;
        std::vector<std::uint32_t> tries;
        std::map<std::string, std::uint32_t> cells;
        std::vector<std::map<std::string, std::uint32_t>> enclosing;
    };

    FnScope begin_function(const std::string& name,
                           const std::vector<std::string>& params,
                           const std::vector<std::string>& locals) {
        FnScope sc{fn_idx_, blk_, locals_, owned_, frame_owned_, class_ns_,
                   loops_, try_stack_, cells_, enclosing_cells_};
        mod_.functions.push_back(ir::Function{name, params, {}, {}, 1});
        fn_idx_ = mod_.functions.size() - 1;
        cur()->locals = locals;
        locals_.clear();
        for (std::uint32_t i = 0; i < locals.size(); ++i) locals_[locals[i]] = i;
        owned_.clear(); frame_owned_.clear(); class_ns_.clear();
        loops_.clear(); try_stack_.clear();
        enclosing_cells_.push_back(sc.cells);
        cells_.clear();
        cur()->blocks.push_back(ir::Block{"entry", {}});
        blk_ = 0;
        return sc;
    }

    std::size_t end_function(const FnScope& sc) {
        std::size_t made = fn_idx_;
        fn_idx_ = sc.fn; blk_ = sc.blk; locals_ = sc.locals;
        owned_ = sc.owned; frame_owned_ = sc.frame; class_ns_ = sc.class_ns;
        loops_ = sc.loops; try_stack_ = sc.tries;
        cells_ = sc.cells; enclosing_cells_ = sc.enclosing;
        return made;
    }

    ir::Value make_function_value(std::size_t idx, const std::string& name,
                                  const SourceLoc& loc,
                                  const std::vector<ir::Value>& closure = {},
                                  ir::Value defaults = {},
                                  int vararg_slot = -1, int kwarg_slot = -1) {
        ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        std::vector<ir::Value> mkargs{defaults};
        for (const ir::Value& c : closure) mkargs.push_back(c);
        // Slots are biased by one so 0 can mean "absent", matching the def
        // path -- MakeFunction reads target/target_else that way.
        ir::Instr mk{ir::Op::MakeFunction, mkargs, fv, Ownership::Owned, name,
                     (std::uint32_t)(vararg_slot + 1),
                     (std::uint32_t)(kwarg_slot + 1), loc, make_landing_pad(loc)};
        mk.imm = (std::int64_t)idx;
        mk.has_imm = true;
        emit(std::move(mk));
        mark_owned(fv);
        return fv;
    }

    // Names a nested construct reads that are LOCALS of the enclosing
    // function. Without closures these cannot be reached from the synthetic
    // function, so they are either passed as hidden arguments (comprehensions,
    // where we control the call) or refused (lambda, where the user calls it).
    void free_locals(const expr& e, std::set<std::string>& bound,
                     std::set<std::string>& out) {
        struct Scan {
            const std::map<std::string, std::uint32_t>& locals;
            std::set<std::string>& bound;
            std::set<std::string>& out;
            void name(const std::string& id) {
                if (!bound.count(id) && locals.count(id)) out.insert(id);
            }
        } sc{locals_, bound, out};
        walk_names(e, sc.bound, [&](const std::string& id){ sc.name(id); });
    }

    template <class F>
    void walk_names(const expr& e, std::set<std::string>& bound, F&& f) {
        // Deliberately conservative and simple: collect every Name read. Over-
        // reporting costs a redundant hidden argument; under-reporting would
        // silently read the wrong variable.
        std::function<void(const expr&)> go = [&](const expr& x) {
            std::visit(ov{
                [&](const Name& n){ f(n.id); },
                [&](const BinOp& n){ go(*n.left); go(*n.right); },
                [&](const BoolOp& n){ for (const expr& v : n.values) go(v); },
                [&](const UnaryOp& n){ go(*n.operand); },
                [&](const Compare& n){ go(*n.left); for (const expr& v : n.comparators) go(v); },
                [&](const Call& n){ go(*n.func); for (const expr& v : n.args) go(v);
                                    for (const keyword& k : n.keywords) go(*k.value); },
                [&](const Attribute& n){ go(*n.value); },
                [&](const Subscript& n){ go(*n.value); go(*n.slice); },
                [&](const IfExp& n){ go(*n.test); go(*n.body); go(*n.orelse); },
                [&](const Tuple& n){ for (const expr& v : n.elts) go(v); },
                [&](const List& n){ for (const expr& v : n.elts) go(v); },
                [&](const Set& n){ for (const expr& v : n.elts) go(v); },
                [&](const Dict& n){ for (const auto& k : n.keys) if (k && *k) go(**k);
                                    for (const expr& v : n.values) go(v); },
                [&](const Starred& n){ go(*n.value); },
                [&](const Slice& n){ if (n.lower && *n.lower) go(**n.lower);
                                     if (n.upper && *n.upper) go(**n.upper);
                                     if (n.step && *n.step) go(**n.step); },
                [&](const NamedExpr& n){ go(*n.value); },
                [&](const Lambda& n){ go(*n.body); },
                [&](const ListComp& n){ go(*n.elt); for (const comprehension& c : n.generators) go(*c.iter); },
                [&](const SetComp& n){ go(*n.elt); for (const comprehension& c : n.generators) go(*c.iter); },
                [&](const DictComp& n){ go(*n.key); go(*n.value);
                                        for (const comprehension& c : n.generators) go(*c.iter); },
                [&](const GeneratorExp& n){ go(*n.elt); for (const comprehension& c : n.generators) go(*c.iter); },
                [&](const Await& n){ go(*n.value); },
                [&](const Yield& n){ if (n.value) go(**n.value); },
                [&](const YieldFrom& n){ go(*n.value); },
                [&](const FormattedValue& n){ go(*n.value); },
                [&](const JoinedStr& n){ for (const expr& v : n.values) go(v); },
                [&](const TemplateStr& n){ for (const expr& v : n.values) go(v); },
                [&](const Interpolation& n){ go(*n.value); },
                [&](const Constant&){},
            }, x.v);
        };
        go(e);
    }

    ir::Value lower_lambda(const Lambda& n, bool* ok) {
        const arguments& a = *n.args;
        // *args and **kwargs work exactly as they do for a def -- same
        // trampoline, same slots. Positional-only and keyword-only markers
        // stay refused: the trampoline binds keywords by NAME, so treating
        // `lambda a, /: ...` as an ordinary parameter would accept f(a=1),
        // which CPython rejects. Accepting what CPython rejects is the same
        // defect as computing the wrong value.
        if (!a.posonlyargs.empty() || !a.kwonlyargs.empty()) {
            *ok = unsupported("positional-only or keyword-only lambda parameters",
                              n.loc);
            return {};
        }
        // Lambda defaults are ordinary defaults: evaluated once, here, in the
        // enclosing scope. `lambda x, k=k: ...` inside a loop is the standard
        // way to capture the current value rather than the cell.
        ir::Value lam_defaults;
        if (!a.defaults.empty()) {
            lam_defaults = call_capi_imm("PyTuple_New", {},
                                         (std::int64_t)a.defaults.size(), 0, n.loc, ok);
            if (!*ok) return {};
            mark_owned(lam_defaults);
            for (std::size_t i = 0; i < a.defaults.size(); ++i) {
                ir::Value d = lower_expr(a.defaults[i], ok);
                if (!*ok) return {};
                call_capi_imm("PyTuple_SetItem", {lam_defaults, d},
                              (std::int64_t)i, 1, n.loc, ok);   // steals d
                if (!*ok) return {};
            }
        }
        std::vector<std::string> params;
        for (const arg& p : a.args) params.push_back(p.arg);
        // *args and **kwargs get their own slots, after the named parameters.
        std::vector<std::string> slotnames = params;
        int vararg_slot = -1, kwarg_slot = -1;
        if (a.vararg) { vararg_slot = (int)slotnames.size(); slotnames.push_back((*a.vararg)->arg); }
        if (a.kwarg)  { kwarg_slot  = (int)slotnames.size(); slotnames.push_back((*a.kwarg)->arg); }

        // A lambda captures through the same cells a nested def does. It used
        // to be refused here because a lambda is called by the USER, so a
        // captured local cannot be passed as a hidden argument the way a
        // comprehension's can -- cells remove that asymmetry.
        std::set<std::string> reads = nested_reads_expr(*n.body);
        std::set<std::string> own(slotnames.begin(), slotnames.end());
        std::vector<std::string> freevars;
        std::vector<ir::Value> closure_cells;
        for (const std::string& r : reads) {
            if (own.count(r)) continue;
            auto cit = cells_.find(r);
            if (cit == cells_.end()) continue;
            freevars.push_back(r);
            ir::Value c = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::LoadLocal, {}, c, Ownership::Owned, r,
                           cit->second, 0, n.loc, make_landing_pad(n.loc)});
            mark_owned(c);
            closure_cells.push_back(c);
        }
        std::vector<std::string> lam_locals = slotnames;
        for (const std::string& f2 : freevars) lam_locals.push_back(f2);

        FnScope sc = begin_function("<lambda>", params, lam_locals);
        cur()->freevars = freevars;
        for (const std::string& f2 : freevars) cells_[f2] = locals_[f2];
        bool bok = true;
        ir::Value r = lower_expr(*n.body, &bok);
        if (bok)
            emit(ir::Instr{ir::Op::Return, {r}, std::nullopt, Ownership::NotAnObject,
                           "", 0, 0, n.loc, std::nullopt});
        std::size_t idx = end_function(sc);
        if (!bok) { *ok = false; return {}; }
        *ok = true;
        ir::Value out = make_function_value(idx, qualname("<lambda>"), n.loc, closure_cells,
                                            lam_defaults, vararg_slot, kwarg_slot);
        // Guarded, NOT unconditional: the lambda path restores the owned set
        // differently from the def path, and re-marking here double-freed the
        // captured cell -- `return lambda x: x + n` crashed with no output.
        for (const ir::Value& c : closure_cells) if (owns(c)) release(c, n.loc);
        if (lam_defaults.valid() && owns(lam_defaults)) release(lam_defaults, n.loc);
        return out;
    }

    // A comprehension is a separate SCOPE in Python 3: its loop variable does
    // not leak. Inlining it would leak, which is an observable difference, so
    // it becomes a synthetic function exactly as CPython compiles it. The
    // iterator is passed as `.0`; captured enclosing locals follow as extra
    // hidden parameters, which works here because we emit the call ourselves.
    ir::Value lower_comp(const std::vector<comprehension>& gens,
                         const expr* elt, const expr* key,
                         const char* kind, const SourceLoc& loc, bool* ok) {
        const comprehension& g = gens[0];
        if (g.is_async) { *ok = unsupported("async comprehensions", loc); return {}; }
        // `for k, v in pairs` binds several names; collect them all so the
        // synthetic function declares a slot for each.
        std::vector<std::string> vars;
        for (const comprehension& c : gens) {
            if (c.is_async) { *ok = unsupported("async comprehensions", loc); return {}; }
            collect_target_names(*c.target, vars);
        }
        if (vars.empty()) {
            *ok = unsupported("this comprehension target", loc);
            return {};
        }
        const std::string var = vars[0];

        std::set<std::string> bound(vars.begin(), vars.end());
        std::set<std::string> captured;
        free_locals(*elt, bound, captured);
        if (key) free_locals(*key, bound, captured);
        for (const comprehension& c : gens) {
            for (const expr& cond : c.ifs) free_locals(cond, bound, captured);
            // Only the FIRST iterable is evaluated outside; later ones are
            // expressions inside the comprehension and may capture too.
            if (&c != &gens[0]) free_locals(*c.iter, bound, captured);
        }
        std::vector<std::string> caps(captured.begin(), captured.end());

        // Evaluate the ITERABLE in the enclosing scope, as Python does.
        ir::Value seq = lower_expr(*g.iter, ok);
        if (!*ok) return {};
        ir::Value iter = call_capi("PyObject_GetIter", {seq}, loc, ok, {seq});
        if (!*ok) return {};
        mark_owned(iter);

        std::vector<ir::Value> capvals;
        for (const std::string& c : caps) {
            ir::Value v = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            auto it = locals_.find(c);
            emit(ir::Instr{ir::Op::LoadLocal, {}, v, Ownership::Owned, c,
                           it->second, 0, loc, make_landing_pad(loc)});
            mark_owned(v);
            capvals.push_back(v);
        }

        std::vector<std::string> params{".0"};
        for (const std::string& c : caps) params.push_back(c);
        std::vector<std::string> locals = params;
        for (const std::string& x : vars) locals.push_back(x);
        locals.push_back(".acc");

        std::string fname = std::string("<") + kind + "comp>";
        FnScope sc = begin_function(fname, params, locals);
        bool bok = true;
        ir::Value acc;
        if (std::string(kind) == "list")
            acc = call_capi_imm("PyList_New", {}, 0, 0, loc, &bok);
        else if (std::string(kind) == "set")
            acc = call_capi("PySet_New", {ir::Value{}}, loc, &bok);
        else
            acc = call_capi("PyDict_New", {}, loc, &bok);
        if (bok) {
            mark_owned(acc);
            store_name(".acc", acc, loc);
            ir::Value it0 = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::LoadLocal, {}, it0, Ownership::Owned, ".0",
                           0, 0, loc, make_landing_pad(loc)});
            mark_owned(it0);
            frame_owned_.push_back(it0);

            // Nest the generators: each one is a loop whose body is the
            // next, and the innermost body accumulates. Written recursively
            // because the shape IS recursive -- flattening it would need an
            // explicit stack of loop headers for no benefit.
            std::function<bool(std::size_t, const ir::Value&)> nest =
                [&](std::size_t gi, const ir::Value& iter_v) -> bool {
                    const comprehension& gg = gens[gi];
                    std::uint32_t head = new_block("comp.head");
                    std::uint32_t body = new_block("comp.body");
                    std::uint32_t done = new_block("comp.done");
                    emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                                   "", head, 0, loc, std::nullopt});
                    set_block(head);
                    ir::Value item = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                    emit(ir::Instr{ir::Op::IterNext, {iter_v}, item, Ownership::Owned,
                                   "", body, done, loc, make_landing_pad(loc)});
                    set_block(body);
                    mark_owned(item);
                    if (!store_target(*gg.target, item, loc)) return false;

                    bool bk = true;
                    for (const expr& cond : gg.ifs) {
                        ir::Value t = lower_predicate(cond, &bk);
                        if (!bk) return false;
                        std::uint32_t keep = new_block("comp.keep");
                        emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt,
                                       Ownership::NotAnObject, "", keep, head, loc,
                                       std::nullopt});
                        set_block(keep);
                    }

                    if (gi + 1 < gens.size()) {
                        ir::Value sub = lower_expr(*gens[gi + 1].iter, &bk);
                        if (!bk) return false;
                        ir::Value subit = call_capi("PyObject_GetIter", {sub}, loc,
                                                    &bk, {sub});
                        if (!bk) return false;
                        mark_owned(subit);
                        frame_owned_.push_back(subit);
                        if (!nest(gi + 1, subit)) return false;
                        frame_owned_.pop_back();
                        if (owns(subit)) release(subit, loc);
                    } else {
                        ir::Value accv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                        emit(ir::Instr{ir::Op::LoadLocal, {}, accv, Ownership::Owned,
                                       ".acc", (std::uint32_t)(locals.size() - 1), 0,
                                       loc, make_landing_pad(loc)});
                        mark_owned(accv);
                        std::string kk(kind);
                        if (kk == "list") {
                            ir::Value v = lower_expr(*elt, &bk);
                            if (bk) call_capi("PyList_Append", {accv, v}, loc, &bk, {v, accv});
                        } else if (kk == "set") {
                            ir::Value v = lower_expr(*elt, &bk);
                            if (bk) call_capi("PySet_Add", {accv, v}, loc, &bk, {v, accv});
                        } else {
                            ir::Value k2 = lower_expr(*key, &bk);
                            ir::Value v = bk ? lower_expr(*elt, &bk) : ir::Value{};
                            if (bk) call_capi("PyDict_SetItem", {accv, k2, v}, loc, &bk,
                                              {k2, v, accv});
                        }
                        if (!bk) return false;
                    }
                    emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                                   "", head, 0, loc, std::nullopt});
                    set_block(done);
                    return true;
                };

            bok = nest(0, it0);
            if (bok) {
                frame_owned_.pop_back();
                if (owns(it0)) release(it0, loc);
                ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::LoadLocal, {}, out, Ownership::Owned,
                               ".acc", (std::uint32_t)(locals.size() - 1), 0, loc,
                               make_landing_pad(loc)});
                emit(ir::Instr{ir::Op::Return, {out}, std::nullopt,
                               Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
            }
        }
        std::size_t idx = end_function(sc);
        if (!bok) { *ok = false; return {}; }

        ir::Value fnv = make_function_value(idx, fname, loc);
        std::vector<ir::Value> args{fnv, iter};
        for (const ir::Value& c : capvals) args.push_back(c);
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        std::vector<ir::Value> saved = args;
        emit(ir::Instr{ir::Op::CallObject, args, out, Ownership::Owned, "",
                       0, 0, loc, make_landing_pad(loc)});
        for (const ir::Value& a : saved) if (owns(a)) release(a, loc);
        mark_owned(out);
        *ok = true;
        return out;
    }

    // Every Name read anywhere in a statement list. Used to detect closure
    // capture before lowering, so it becomes a diagnostic rather than a
    // NameError at run time (I1).
    void stmt_names(const std::vector<stmt>& body, std::set<std::string>& bound,
                    std::set<std::string>& out) {
        auto note = [&](const expr& e) { free_locals(e, bound, out); };
        std::function<void(const stmt&)> go = [&](const stmt& s2) {
            std::visit(ov{
                [&](const Expr& x){ note(*x.value); },
                [&](const Return& x){ if (x.value) note(**x.value); },
                [&](const Assign& x){ note(*x.value); },
                [&](const AugAssign& x){ note(*x.value); note(*x.target); },
                [&](const AnnAssign& x){ if (x.value) note(**x.value); },
                [&](const If& x){ note(*x.test); for (const stmt& y : x.body) go(y);
                                  for (const stmt& y : x.orelse) go(y); },
                [&](const While& x){ note(*x.test); for (const stmt& y : x.body) go(y);
                                     for (const stmt& y : x.orelse) go(y); },
                [&](const For& x){ note(*x.iter); for (const stmt& y : x.body) go(y);
                                   for (const stmt& y : x.orelse) go(y); },
                [&](const AsyncFor& x){ note(*x.iter); for (const stmt& y : x.body) go(y); },
                [&](const Try& x){ for (const stmt& y : x.body) go(y);
                                   for (const stmt& y : x.orelse) go(y);
                                   for (const stmt& y : x.finalbody) go(y);
                                   for (const excepthandler& h : x.handlers)
                                       for (const stmt& y : std::get<ExceptHandler>(h.v).body) go(y); },
                [&](const TryStar& x){ for (const stmt& y : x.body) go(y); },
                [&](const With& x){ for (const withitem& w : x.items) note(*w.context_expr);
                                    for (const stmt& y : x.body) go(y); },
                [&](const AsyncWith& x){ for (const stmt& y : x.body) go(y); },
                [&](const Raise& x){ if (x.exc) note(**x.exc); },
                [&](const Assert& x){ note(*x.test); },
                [&](const Delete& x){ for (const expr& t : x.targets) note(t); },
                [&](const Match& x){ note(*x.subject);
                                     for (const match_case& c : x.cases)
                                         for (const stmt& y : c.body) go(y); },
                // A nested def's own body is a further scope; its captures are
                // reported when IT is lowered.
                [&](const FunctionDef&){}, [&](const AsyncFunctionDef&){},
                [&](const ClassDef&){}, [&](const Import&){}, [&](const ImportFrom&){},
                [&](const Global&){}, [&](const Nonlocal&){}, [&](const Pass&){},
                [&](const Break&){}, [&](const Continue&){}, [&](const TypeAlias&){},
            }, s2.v);
        };
        for (const stmt& s2 : body) go(s2);
    }

    // Bind a function whose BODY was compiled by CPython -- a generator
    // function, a coroutine, or an async generator. pyc still owns everything
    // the enclosing scope is responsible for: the defaults evaluated at def
    // time, the closure cells, the decorators, and the binding.
    //
    // Shared by def and async def rather than written twice. Two lowerings of
    // one construct is the drift that produced a leak in the def path while
    // the lambda path was correct, and an unsound second copy of try/finally.
    // async def. await, async for and async with are all SyntaxErrors outside
    // an async function, so compiling the body covers every one of them --
    // there is nothing left for pyc to lower inside it.
    bool lower_async_functiondef(const AsyncFunctionDef& n) {
        const arguments& a = *n.args;
        if (!a.posonlyargs.empty()) return unsupported("positional-only parameters", n.loc);
        if (!a.kwonlyargs.empty())  return unsupported("keyword-only parameters", n.loc);
        // Defaults are evaluated ONCE, here, in the enclosing scope.
        bool dok = true;
        ir::Value defaults;
        if (!a.defaults.empty()) {
            defaults = call_capi_imm("PyTuple_New", {},
                                     (std::int64_t)a.defaults.size(), 0, n.loc, &dok);
            if (!dok) return false;
            mark_owned(defaults);
            for (std::size_t i = 0; i < a.defaults.size(); ++i) {
                ir::Value d = lower_expr(a.defaults[i], &dok);
                if (!dok) return false;
                call_capi_imm("PyTuple_SetItem", {defaults, d},
                              (std::int64_t)i, 1, n.loc, &dok);    // steals d
                if (!dok) return false;
            }
        }
        return lower_cpython_function(n.name, n.decorator_list, defaults, n.loc);
    }

    bool lower_cpython_function(const std::string& name,
                                const std::vector<expr>& decorators,
                                ir::Value defaults, const SourceLoc& loc) {
        const GenexpEntry* gf = find_genexp(loc);
        if (!gf) return err("no compiled code object for this function",
                            "async function definitions", loc);
        ir::Value closure;
        if (!gf->freevars.empty()) {
            bool cok = true;
            closure = call_capi_imm("PyTuple_New", {},
                                    (std::int64_t)gf->freevars.size(), 0, loc, &cok);
            if (!cok) return false;
            mark_owned(closure);
            for (std::size_t i = 0; i < gf->freevars.size(); ++i) {
                const std::string& fv2 = gf->freevars[i];
                auto cit = cells_.find(fv2);
                if (cit == cells_.end())
                    return err("function captures '" + fv2 +
                               "', which has no closure cell here", "yield", loc);
                ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::LoadLocal, {}, cell, Ownership::Owned,
                               fv2, cit->second, 0, loc, make_landing_pad(loc)});
                mark_owned(cell);
                call_capi_imm("PyTuple_SetItem", {closure, cell},
                              (std::int64_t)i, 1, loc, &cok);      // steals
                if (!cok) return false;
            }
        }
        ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::MakeGenFunc,
                       {closure.valid() ? closure : ir::Value{},
                        defaults.valid() ? defaults : ir::Value{}},
                       fv, Ownership::Owned, gf->code, 0, 0, loc,
                       make_landing_pad(loc)});
        mark_owned(fv);
        if (closure.valid() && owns(closure)) release(closure, loc);
        if (defaults.valid() && owns(defaults)) release(defaults, loc);
        bool okd = true;
        fv = apply_decorators(decorators, fv, loc, &okd);
        if (!okd) return false;
        if (!class_ns_.empty()) {
            ir::Value m = call_capi("pyc_rt_bind_method", {fv}, loc, &okd, {fv});
            if (!okd) return false;
            mark_owned(m);
            store_name(name, m, loc);
            return true;
        }
        store_name(name, fv, loc);
        return true;
    }

    bool lower_functiondef(const FunctionDef& n) {
        const arguments& a = *n.args;
        if (!a.posonlyargs.empty()) return unsupported("positional-only parameters", n.loc);
        if (!a.kwonlyargs.empty())  return unsupported("keyword-only parameters", n.loc);

        std::vector<std::string> params;
        for (const arg& p : a.args) params.push_back(p.arg);

        // Defaults are evaluated ONCE, here, in the enclosing scope -- not per
        // call. That is the behaviour behind the mutable-default surprise, and
        // evaluating them at call time would be a different language.
        bool dok = true;
        ir::Value defaults;
        if (!a.defaults.empty()) {
            defaults = call_capi_imm("PyTuple_New", {},
                                     (std::int64_t)a.defaults.size(), 0, n.loc, &dok);
            if (!dok) return false;
            mark_owned(defaults);
            for (std::size_t i = 0; i < a.defaults.size(); ++i) {
                ir::Value d = lower_expr(a.defaults[i], &dok);
                if (!dok) return false;
                call_capi_imm("PyTuple_SetItem", {defaults, d},
                              (std::int64_t)i, 1, n.loc, &dok);   // steals d
                if (!dok) return false;
            }
        }

        // The qualname is fixed BEFORE the body is lowered, because lowering
        // the body pushes this function's own scope onto qual_.
        const std::string fn_qualname = qualname(n.name);

        // A def containing yield, and every async def, has its body compiled
        // by CPython and run by the interpreter (rebuild/GENERATORS.md).
        if (find_genexp(n.loc))
            return lower_cpython_function(n.name, n.decorator_list, defaults, n.loc);

        // Save the enclosing function's state: a nested def is lowered into a
        // separate ir::Function, and must not inherit the outer local map.
        std::size_t outer_fn = fn_idx_;
        std::size_t outer_blk = blk_;
        auto outer_locals = locals_;
        auto outer_owned = owned_;
        auto outer_loops = loops_;
        // try_stack_ too: a def inside a try otherwise emitted landing pads
        // branching to the ENCLOSING function's handler block, which is not a
        // label in this function at all.
        auto outer_tries = try_stack_;
        // frame_owned_ must be saved too. A method lowered inside a class body
        // would otherwise inherit the class's namespace dict, and its landing
        // pads would emit a decref for a value defined in a DIFFERENT
        // function -- which LLVM rejects as "does not dominate all uses".
        auto outer_frame = frame_owned_;
        auto outer_class_ns = class_ns_;
        auto outer_cells = cells_;
        auto outer_enclosing = enclosing_cells_;

        // *args and **kwargs get their own local slots, after the named
        // parameters and before everything else the body binds.
        std::vector<std::string> slotnames = params;
        int vararg_slot = -1, kwarg_slot = -1;
        if (a.vararg) { vararg_slot = (int)slotnames.size(); slotnames.push_back((*a.vararg)->arg); }
        if (a.kwarg)  { kwarg_slot  = (int)slotnames.size(); slotnames.push_back((*a.kwarg)->arg); }

        // --- closure analysis -------------------------------------------
        std::vector<std::string> own_locals = function_locals(slotnames, n.body);
        // A local is a CELL variable exactly when something nested reads it.
        std::set<std::string> inner = nested_reads(n.body);
        std::vector<std::string> cellvars;
        for (const std::string& l : own_locals)
            if (inner.count(l)) cellvars.push_back(l);
        // A name is FREE when this function (or something nested in it) reads
        // it, it is not local here, and an enclosing function holds it in a
        // cell. Searching outward is what makes depth-3 nesting work.
        std::vector<std::string> freevars;
        {
            std::set<std::string> reads = inner;
            std::set<std::string> none;
            stmt_names(n.body, none, reads);        // names read directly too
            // A `nonlocal` name is free even if it is only ever written.
            std::set<std::string> nl = declared_nonlocals(n.body);
            reads.insert(nl.begin(), nl.end());
            // Mentioning super or __class__ inside a class body captures the
            // implicit __class__ cell, exactly as CPython's symtable does.
            if (!class_cells_.empty()) {
                std::set<std::string> all = all_reads(n.body);
                if (all.count("super") || all.count("__class__"))
                    reads.insert("__class__");
            }
            std::set<std::string> own(own_locals.begin(), own_locals.end());
            for (const std::string& r : reads) {
                if (own.count(r)) continue;
                if (r == "__class__" && !class_cells_.empty()) {
                    freevars.push_back(r);          // sourced from class_cells_
                    continue;
                }
                bool in_enclosing = cells_.count(r) > 0;
                for (auto it = enclosing_cells_.rbegin();
                     !in_enclosing && it != enclosing_cells_.rend(); ++it)
                    in_enclosing = it->count(r) > 0;
                if (in_enclosing) freevars.push_back(r);
            }
            // CPython rejects `nonlocal x` with no binding in any enclosing
            // function scope at compile time. Falling through to the global
            // would run, and quietly write the wrong variable.
            for (const std::string& x : nl) {
                bool bound_outward = false;
                for (const std::string& f2 : freevars) if (f2 == x) { bound_outward = true; break; }
                if (!bound_outward) {
                    return err("no binding for nonlocal '" + x + "' found",
                               "nonlocal", n.loc);
                }
            }
        }
        // Closure cells for the nested function come from THIS function's
        // slots, so they must be read before the scope switches.
        std::vector<ir::Value> closure_cells;
        for (const std::string& fv2 : freevars) {
            if (fv2 == "__class__" && !class_cells_.empty()) {
                // Already a cell value in hand; it has no enclosing slot.
                ir::Value c = class_cells_.back();
                emit(ir::Instr{ir::Op::IncRef, {c}, std::nullopt,
                               Ownership::NotAnObject, "", 0, 0, n.loc, std::nullopt});
                closure_cells.push_back(c);
                continue;
            }
            auto cit = cells_.find(fv2);
            if (cit == cells_.end()) continue;
            ir::Value c = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::LoadLocal, {}, c, Ownership::Owned, fv2,
                           cit->second, 0, n.loc, make_landing_pad(n.loc)});
            mark_owned(c);
            closure_cells.push_back(c);
        }

        std::vector<std::string> all_locals = own_locals;
        for (const std::string& fv2 : freevars) all_locals.push_back(fv2);

        mod_.functions.push_back(ir::Function{n.name, params, {}, {}, 1});
        fn_idx_ = mod_.functions.size() - 1;
        const std::size_t fn_index = fn_idx_;
        cur()->locals = all_locals;
        cur()->cellvars = cellvars;
        cur()->freevars = freevars;
        locals_.clear();
        for (std::uint32_t i = 0; i < all_locals.size(); ++i)
            locals_[all_locals[i]] = i;
        enclosing_cells_.push_back(cells_);
        cells_.clear();
        for (const std::string& c : cellvars) cells_[c] = locals_[c];
        for (const std::string& fv2 : freevars) cells_[fv2] = locals_[fv2];
        owned_.clear();
        loops_.clear();
        try_stack_.clear();
        frame_owned_.clear();
        class_ns_.clear();          // a method body is not a class body
        auto outer_qual = qual_;
        // A name bound inside a function body is qualified through <locals>.
        // A method's qualname is "C.foo", so the class prefix stays, but the
        // enclosing FUNCTION contributes "name.<locals>".
        qual_.push_back(n.name + ".<locals>");
        cur()->blocks.push_back(ir::Block{"entry", {}});
        blk_ = 0;

        // Prologue: give each cell variable a cell. A parameter arrives as a
        // plain value, so its cell must be seeded with it and the slot
        // overwritten -- otherwise the nested function sees an empty cell.
        for (const std::string& c : cellvars) {
            std::uint32_t slot = locals_[c];
            bool is_param = slot < params.size()
                         || (vararg_slot >= 0 && (int)slot == vararg_slot)
                         || (kwarg_slot >= 0 && (int)slot == kwarg_slot);
            ir::Value seed;
            if (is_param) {
                seed = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::LoadLocal, {}, seed, Ownership::Owned, c,
                               slot, 0, n.loc, std::nullopt});
                mark_owned(seed);
            }
            ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::CellNew,
                           seed.valid() ? std::vector<ir::Value>{seed}
                                        : std::vector<ir::Value>{},
                           cell, Ownership::Owned, c, 0, 0, n.loc,
                           make_landing_pad(n.loc)});
            mark_owned(cell);
            if (seed.valid()) release(seed, n.loc);
            emit(ir::Instr{ir::Op::StoreLocal, {cell}, std::nullopt,
                           Ownership::NotAnObject, c, slot, 0, n.loc, std::nullopt});
            release(cell, n.loc);
        }

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
        owned_ = outer_owned; loops_ = outer_loops; try_stack_ = outer_tries;
        frame_owned_ = outer_frame; class_ns_ = outer_class_ns;
        cells_ = outer_cells; enclosing_cells_ = outer_enclosing;
        qual_ = outer_qual;
        if (!ok) return false;

        // Bind the callable in the enclosing scope, by the same store path any
        // other assignment uses -- a def is not special.
        ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        // Reference the function by INDEX, not name. Two classes can each
        // define `who`, and looking it up by name both picked the wrong one
        // and emitted a duplicate LLVM symbol.
        // target/target_else carry the *args and **kwargs slot indices,
        // biased by one so 0 can mean "absent".
        // args[0] is defaults (or the null value), args[1..] the closure cells.
        std::vector<ir::Value> mkargs{defaults.valid() ? defaults : ir::Value{}};
        for (const ir::Value& c : closure_cells) mkargs.push_back(c);
        ir::Instr mk{ir::Op::MakeFunction, mkargs,
                     fv, Ownership::Owned, fn_qualname,
                     (std::uint32_t)(vararg_slot + 1),
                     (std::uint32_t)(kwarg_slot + 1),
                     n.loc, make_landing_pad(n.loc)};
        mk.imm = (std::int64_t)fn_index;
        mk.has_imm = true;
        emit(std::move(mk));
        mark_owned(fv);
        if (defaults.valid() && owns(defaults)) release(defaults, n.loc);
        // MakeFunction does not consume the cells -- it INCREFs them into the
        // closure tuple -- so the references loaded above are still ours. The
        // lambda path already released them; this one leaked one cell per
        // free variable, on every nested def that captures anything.
        // They were marked owned AFTER outer_owned was saved, so the restore
        // above dropped them from the owned set: re-mark before releasing.
        for (const ir::Value& c : closure_cells) { mark_owned(c); release(c, n.loc); }
        if (!set_docstring(fv, n.body, n.loc)) return false;
        // A pyc function is a descriptor now, so a method in a class dict
        // binds self by itself; pyc_rt_bind_method is a pass-through kept so
        // the class path has one place to change if that stops being true.
        if (!class_ns_.empty()) {
            bool okm = true;
            fv = apply_decorators(n.decorator_list, fv, n.loc, &okm);
            if (!okm) return false;
            ir::Value m = call_capi("pyc_rt_bind_method", {fv}, n.loc, &okm, {fv});
            if (!okm) return false;
            mark_owned(m);
            store_name(n.name, m, n.loc);
            return true;
        }
        fv = apply_decorators(n.decorator_list, fv, n.loc, &ok);
        if (!ok) return false;
        store_name(n.name, fv, n.loc);
        return true;
    }

    // @a @b def f  ->  f = a(b(f)). Applied BOTTOM-UP, i.e. nearest the def
    // first, which is the order Python specifies and the opposite of how the
    // list reads.
    ir::Value apply_decorators(const std::vector<expr>& decos,
                               ir::Value fv, const SourceLoc& loc, bool* ok) {
        for (auto it = decos.rbegin(); it != decos.rend(); ++it) {
            ir::Value d = lower_expr(*it, ok);
            if (!*ok) return fv;
            ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            std::vector<ir::Value> args{d, fv};
            emit(ir::Instr{ir::Op::CallObject, args, out, Ownership::Owned, "",
                           0, 0, loc, make_landing_pad(loc)});
            if (owns(d)) release(d, loc);
            if (owns(fv)) release(fv, loc);
            mark_owned(out);
            fv = out;
        }
        return fv;
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
        return finish_return(v, n.loc);
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
            [&](const Tuple& n)   { ok = unpack_into(n.elts, v, loc); },
            [&](const List& n)    { ok = unpack_into(n.elts, v, loc); },
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
                [&](const Name& n2)      {
                    if (locals_.count(n2.id)) { ok = unsupported("del of a local", n2.loc); return; }
                    ir::Value r = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
                    emit(ir::Instr{ir::Op::DelGlobal, {}, r, Ownership::NotAnObject,
                                   n2.id, 0, 0, n.loc, make_landing_pad(n.loc)});
                },
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

    // `a, b = value` and its nested forms. The arity check and CPython's
    // exact wording live in the runtime helper; here we only distribute.
    // A machine integer constant, for C-API parameters typed as integers.
    void emit_int_const(const ir::Value& dst, std::int64_t n, const SourceLoc& loc) {
        ir::Instr in{ir::Op::IntConst, {}, dst, Ownership::NotAnObject,
                     std::to_string(n), 0, 0, loc, std::nullopt};
        emit(std::move(in));
    }

    bool unpack_into(const std::vector<expr>& targets, const ir::Value& v,
                     const SourceLoc& loc) {
        bool ok = true;
        int star = -1;
        for (std::size_t i = 0; i < targets.size(); ++i)
            if (std::holds_alternative<Starred>(targets[i].v)) {
                if (star >= 0) return err("two starred targets in one assignment",
                                          "assignment-target", loc);
                star = (int)i;
            }
        ir::Value tup;
        if (star >= 0) {
            // `a, *rest, b = v`: the star absorbs whatever the fixed positions
            // leave, so the split is a run-time decision.
            std::int64_t before = star;
            std::int64_t after = (std::int64_t)targets.size() - star - 1;
            ir::Value nb = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
            ir::Value na = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
            emit_int_const(nb, before, loc);
            emit_int_const(na, after, loc);
            tup = call_capi("pyc_rt_unpack_ex", {v, nb, na}, loc, &ok);
            if (!ok) return false;
            mark_owned(tup);
        } else {
            tup = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            ir::Instr in{ir::Op::Unpack, {v}, tup, Ownership::Owned, "",
                         0, 0, loc, make_landing_pad(loc)};
            in.imm = (std::int64_t)targets.size();
            in.has_imm = true;
            emit(std::move(in));
        }
        mark_owned(tup);
        if (owns(v)) release(v, loc);
        for (std::size_t i = 0; i < targets.size(); ++i) {
            ir::Value idx = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            ir::Instr gi{ir::Op::ConstInt, {}, idx, Ownership::Owned,
                         std::to_string(i), 0, 0, loc, std::nullopt};
            emit(std::move(gi));
            mark_owned(idx);
            ir::Value item = call_capi("PyObject_GetItem", {tup, idx}, loc, &ok, {idx});
            if (!ok) return false;
            mark_owned(item);
            // Recurses, so `(a, (b, c)) = ...` works by construction.
            const expr* tgt = &targets[i];
            if (const Starred* st = std::get_if<Starred>(&tgt->v)) tgt = &*st->value;
            if (!store_target(*tgt, item, loc)) return false;
        }
        if (owns(tup)) release(tup, loc);
        return true;
    }

    void store_target_keep(const expr& t, const ir::Value& v,
                           const SourceLoc& loc, bool* ok) {
        bool was = owns(v);
        if (was) forget(v);
        *ok = store_target(t, v, loc);
        if (was) mark_owned(v);
    }

    bool bad_target(const char* what, const SourceLoc& loc) {
        return err(std::string("cannot assign to ") + what, "assignment-target", loc);
    }

    // One store path for every binding form: def, assignment, everything.
    // While lowering a class body, every binding goes into the namespace dict
    // rather than a local or global slot. That is what makes `def m(self)`
    // inside a class become a method instead of a module-level function.
    std::vector<ir::Value> class_ns_;
    // __qualname__ prefix components. CPython puts the qualname, not the bare
    // name, in argument-binding TypeErrors: "C.foo()", "outer.<locals>.inner()".
    std::vector<std::string> qual_;
    // Zero-argument super() is compiler magic in CPython: the class object is
    // handed to the method through an implicit __class__ closure cell, which
    // is filled only AFTER the class exists. Methods are built before that, so
    // the cell -- not the class -- is what they capture.
    std::vector<ir::Value> class_cells_;
    std::string qualname(const std::string& name) const {
        std::string q;
        for (const std::string& c : qual_) { q += c; q += "."; }
        return q + name;
    }

    // Bind without consuming the caller's reference. The namespace INCREFs,
    // so the value remains ours to use afterwards -- which is what makes
    // `(x := f())` evaluate to the same object it binds.
    void store_name_keep(const std::string& name, const ir::Value& v,
                         const SourceLoc& loc) {
        bool was = owns(v);
        if (was) forget(v);
        store_name(name, v, loc);
        if (was) mark_owned(v);
    }

    void store_name(const std::string& name, const ir::Value& v, const SourceLoc& loc) {
        auto cit = cells_.find(name);
        if (cit != cells_.end() && class_ns_.empty()) {
            ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            bool ok = true;
            emit(ir::Instr{ir::Op::LoadLocal, {}, cell, Ownership::Owned, name,
                           cit->second, 0, loc, make_landing_pad(loc)});
            mark_owned(cell);
            emit(ir::Instr{ir::Op::CellSet, {cell, v}, std::nullopt,
                           Ownership::NotAnObject, name, 0, 0, loc, std::nullopt});
            release(cell, loc);
            if (owns(v)) release(v, loc);
            (void)ok;
            return;
        }
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

    // `x += y` is NOT `x = x + y`: it calls the in-place slot, so a list
    // extends in place and a tuple does not. Using the binary operator would
    // change observable behaviour for every mutable type.
    bool lower_augassign(const AugAssign& n) {
        static const struct { const char* sym; } kUnused{nullptr}; (void)kUnused;
        const char* sym = nullptr;
        std::visit(ov{
            [&](const Add&)      { sym = "PyNumber_InPlaceAdd"; },
            [&](const Sub&)      { sym = "PyNumber_InPlaceSubtract"; },
            [&](const Mult&)     { sym = "PyNumber_InPlaceMultiply"; },
            [&](const Div&)      { sym = "PyNumber_InPlaceTrueDivide"; },
            [&](const FloorDiv&) { sym = "PyNumber_InPlaceFloorDivide"; },
            [&](const Mod&)      { sym = "PyNumber_InPlaceRemainder"; },
            [&](const Pow&)      { sym = "PyNumber_InPlacePower"; },
            [&](const LShift&)   { sym = "PyNumber_InPlaceLshift"; },
            [&](const RShift&)   { sym = "PyNumber_InPlaceRshift"; },
            [&](const BitOr&)    { sym = "PyNumber_InPlaceOr"; },
            [&](const BitXor&)   { sym = "PyNumber_InPlaceXor"; },
            [&](const BitAnd&)   { sym = "PyNumber_InPlaceAnd"; },
            [&](const MatMult&)  { sym = "PyNumber_InPlaceMatrixMultiply"; },
        }, n.op.v);
        if (!sym) return unsupported("this augmented operator", n.loc);

        bool ok = true;
        // The target must be evaluated EXACTLY ONCE. Reading it with
        // lower_expr and then writing it with store_target evaluates the
        // object expression twice, so `get_box().n += 1` called get_box()
        // twice -- visible whenever that expression has a side effect.
        // Evaluate the base (and subscript key) once here and reuse them.
        ir::Value base, key;
        bool has_base = false, has_key = false;
        std::visit(ov{
            [&](const Attribute& a) {
                base = lower_expr(*a.value, &ok); has_base = ok;
            },
            [&](const Subscript& a) {
                base = lower_expr(*a.value, &ok); if (!ok) return;
                key = lower_expr(*a.slice, &ok);  if (!ok) return;
                has_base = has_key = true;
            },
            [&](const auto&) {},
        }, n.target->v);
        if (!ok) return false;

        ir::Value cur_v;
        if (has_key) {
            cur_v = call_capi("PyObject_GetItem", {base, key}, n.loc, &ok);
        } else if (has_base) {
            ir::Value nm = const_str(std::get<Attribute>(n.target->v).attr, n.loc);
            cur_v = call_capi("PyObject_GetAttr", {base, nm}, n.loc, &ok, {nm});
        } else {
            cur_v = lower_expr(*n.target, &ok);            // plain name
        }
        if (!ok) return false;
        mark_owned(cur_v);
        ir::Value rhs = lower_expr(*n.value, &ok);
        if (!ok) return false;
        ir::Value out;
        if (std::string(sym) == "PyNumber_InPlacePower") {
            ir::Value none = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, none, Ownership::Owned, "",
                           0, 0, n.loc, std::nullopt});
            mark_owned(none);
            out = call_capi(sym, {cur_v, rhs, none}, n.loc, &ok, {cur_v, rhs, none});
        } else {
            out = call_capi(sym, {cur_v, rhs}, n.loc, &ok, {cur_v, rhs});
        }
        if (!ok) return false;
        mark_owned(out);
        // Write back through the SAME base/key, not a re-evaluation.
        if (has_key) {
            call_capi("PyObject_SetItem", {base, key, out}, n.loc, &ok, {base, key});
            if (ok && owns(out)) release(out, n.loc);
            return ok;
        }
        if (has_base) {
            ir::Value nm = const_str(std::get<Attribute>(n.target->v).attr, n.loc);
            call_capi("PyObject_SetAttr", {base, nm, out}, n.loc, &ok, {base, nm});
            if (ok && owns(out)) release(out, n.loc);
            return ok;
        }
        return store_target(*n.target, out, n.loc);
    }

    bool lower_with(const With& n) {
        if (n.items.size() != 1) return unsupported("multiple with items", n.loc);
        // __exit__ must run on EVERY exit path. break/continue/return would
        // leave the block without it, and silently skipping cleanup is worse
        // than refusing -- a file handle stays open, a lock stays held.
        for (const stmt& s2 : n.body) {
            bool bad = false;
            std::visit(ov{
                [&](const Return&){ bad = true; }, [&](const Break&){ bad = true; },
                [&](const Continue&){ bad = true; },
                [&](const Expr&){}, [&](const Assign&){}, [&](const AugAssign&){},
                [&](const AnnAssign&){}, [&](const If&){}, [&](const While&){},
                [&](const For&){}, [&](const AsyncFor&){}, [&](const With&){},
                [&](const AsyncWith&){}, [&](const Try&){}, [&](const TryStar&){},
                [&](const Match&){}, [&](const FunctionDef&){},
                [&](const AsyncFunctionDef&){}, [&](const ClassDef&){},
                [&](const Import&){}, [&](const ImportFrom&){}, [&](const Global&){},
                [&](const Nonlocal&){}, [&](const Pass&){}, [&](const Raise&){},
                [&](const Assert&){}, [&](const Delete&){}, [&](const TypeAlias&){},
            }, s2.v);
            if (bad) return unsupported("return/break/continue inside with", n.loc);
        }

        bool ok = true;
        const withitem& w = n.items[0];
        ir::Value mgr = lower_expr(*w.context_expr, &ok);
        if (!ok) return false;
        // __exit__ is looked up BEFORE __enter__ runs, as CPython does: a
        // manager missing __exit__ must fail before any setup happens.
        ir::Value exitf = call_capi("pyc_rt_cm_exit", {mgr}, n.loc, &ok);
        if (!ok) return false;
        mark_owned(exitf);
        frame_owned_.push_back(exitf);
        ir::Value entered = call_capi("pyc_rt_cm_enter", {mgr}, n.loc, &ok, {mgr});
        if (!ok) return false;
        mark_owned(entered);
        if (w.optional_vars) { if (!store_target(**w.optional_vars, entered, n.loc)) return false; }
        else if (owns(entered)) release(entered, n.loc);

        std::uint32_t dispatch = new_block("with.unwind");
        std::uint32_t after    = new_block("with.after");
        try_stack_.push_back(dispatch);
        for (const stmt& s2 : n.body) if (!lower_stmt(s2)) { ok = false; break; }
        try_stack_.pop_back();
        if (!ok) return false;

        call_capi("pyc_rt_exit_normal", {exitf}, n.loc, &ok);
        if (!ok) return false;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, n.loc, std::nullopt});

        set_block(dispatch);
        ir::Value sup = call_capi("pyc_rt_exit_exc", {exitf}, n.loc, &ok);
        if (!ok) return false;
        std::uint32_t reraise = new_block("with.reraise");
        emit(ir::Instr{ir::Op::CondBr, {sup}, std::nullopt, Ownership::NotAnObject,
                       "", after, reraise, n.loc, std::nullopt});
        set_block(reraise);
        frame_owned_.pop_back();
        std::uint32_t pad = make_landing_pad(n.loc);
        frame_owned_.push_back(exitf);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", pad, 0, n.loc, std::nullopt});

        set_block(after);
        frame_owned_.pop_back();
        if (owns(exitf)) release(exitf, n.loc);
        return true;
    }

    // One pending finally region. `preds` accumulates every edge into the
    // finally body together with what it is carrying: a pending exception, a
    // pending return value, or neither for normal completion.
    struct FinallyCtx {
        std::uint32_t entry;
        std::vector<std::uint32_t> pred_blocks;
        std::vector<ir::Value> pred_exc;   // pending exception, else null
        std::vector<ir::Value> pred_ret;   // pending return value, else null
        std::vector<ir::Value> pred_brk;   // non-null: a break is pending
        std::vector<ir::Value> pred_cont;  // non-null: a continue is pending
        bool any_jump = false;             // did any break/continue arrive here?
        // Four parallel edges rather than one integer discriminant: a pointer
        // phi already exists and needs no new IR, and "is this null" is the
        // same test the other two paths use.
        void edge(std::uint32_t b, ir::Value e, ir::Value r,
                  ir::Value k, ir::Value c) {
            pred_blocks.push_back(b); pred_exc.push_back(e);
            pred_ret.push_back(r); pred_brk.push_back(k); pred_cont.push_back(c);
        }
    };
    std::vector<FinallyCtx*> fin_stack_;
    // Except blocks currently open, holding the previous handled exception.
    // CPython pops its exc_info stack as a frame unwinds; pyc must do the same
    // on EVERY way out of a handler. Leaving one on the stack made the handled
    // exception the __context__ of the next unrelated one: `return` from an
    // except block, then a later raise, printed "During handling of the above
    // exception" for an exception that had been fully handled.
    std::vector<ir::Value> handled_stack_;

    // Pop every open handler, innermost first. Emitted before any exit that
    // leaves the handler without falling off its end.
    void pop_open_handlers(const SourceLoc& loc) {
        for (auto it = handled_stack_.rbegin(); it != handled_stack_.rend(); ++it) {
            bool ok = true;
            call_capi("pyc_rt_pop_handled", {*it}, loc, &ok);
            (void)ok;
        }
    }
    // Loop depth when the innermost finally region was entered. A break inside
    // a try/finally must run the cleanup on its way out, which the dispatch
    // does not yet carry; a break in a loop STARTED inside the try is a plain
    // branch and stays supported.
    std::size_t fin_loop_depth_ = 0;

    ir::Value const_null(const SourceLoc& loc) {
        ir::Value v = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::ConstNull, {}, v, Ownership::AlwaysNull, "",
                       0, 0, loc, std::nullopt});
        return v;
    }

    bool lower_try(const Try& n) {
        if (n.finalbody.empty()) return lower_try_except(n);
        return lower_try_finally(n);
    }

    // try/finally, with ONE copy of the cleanup.
    //
    // The earlier attempt duplicated the finally body at each exit. That is
    // unsound: the landing-pad model snapshots the live set when a pad is
    // created, so under nesting the two copies' pads become cross-reachable
    // and a pad decrefs a value defined only in the other copy.
    //
    // Instead every exit converges on one block, carrying WHY it left as a
    // pair of values -- pending exception, pending return -- merged by phis.
    // The cleanup runs once, then dispatches on those. Both predecessors
    // arrive with their statement temporaries already released (landing pads
    // decref owned_ before branching, and the normal path has none live), so
    // the live set at the join is the same on every edge, which is exactly
    // what the duplicated form could not guarantee.
    bool lower_try_finally(const Try& n) {
        FinallyCtx fin;
        fin.entry = new_block("finally.body");
        std::uint32_t catch_b = new_block("finally.catch");
        std::uint32_t after   = new_block("try.after");

        // Uncaught exceptions from the guarded region land here, not in an
        // enclosing handler: the cleanup has to run first.
        try_stack_.push_back(catch_b);
        fin_stack_.push_back(&fin);
        std::size_t saved_fin_depth = fin_loop_depth_;
        fin_loop_depth_ = loops_.size();
        bool ok = true;
        if (n.handlers.empty() && n.orelse.empty()) {
            for (const stmt& s2 : n.body) if (!lower_stmt(s2)) { ok = false; break; }
        } else {
            // try/except/finally is try/except wrapped in a finally region.
            Try inner{n.body, n.handlers, n.orelse, {}, n.loc};
            ok = lower_try_except(inner);
        }
        fin_stack_.pop_back();
        fin_loop_depth_ = saved_fin_depth;
        try_stack_.pop_back();
        if (!ok) return false;

        // Normal completion: nothing pending. Only if control can actually
        // fall out of the guarded region -- `try: return x finally: ...` ends
        // it with a terminator, and recording an edge from an already
        // terminated block produces a duplicate phi predecessor carrying a
        // different value, which LLVM rejects.
        if (!terminated()) {
            fin.edge((std::uint32_t)blk_, const_null(n.loc), const_null(n.loc),
                     const_null(n.loc), const_null(n.loc));
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", fin.entry, 0, n.loc, std::nullopt});
        }

        // The unwind edge: take the exception so the cleanup runs with no
        // error set, exactly as CPython does, and carry it to the dispatch.
        set_block(catch_b);
        ir::Value exc = call_capi("PyErr_GetRaisedException", {}, n.loc, &ok);
        if (!ok) return false;
        mark_owned(exc);
        fin.edge((std::uint32_t)blk_, exc, const_null(n.loc),
                 const_null(n.loc), const_null(n.loc));
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", fin.entry, 0, n.loc, std::nullopt});
        forget(exc);      // it lives on through the phi, not this name

        set_block(fin.entry);
        ir::Value p_exc  = emit_phi(fin.pred_exc,  fin.pred_blocks, n.loc);
        ir::Value p_ret  = emit_phi(fin.pred_ret,  fin.pred_blocks, n.loc);
        // Only when a break or continue actually reached this cleanup: the
        // body has been lowered by now, so every edge is known, and emitting
        // unused marker phis would create references nothing releases.
        ir::Value p_brk, p_cont;
        if (fin.any_jump) {
            p_brk  = emit_phi(fin.pred_brk,  fin.pred_blocks, n.loc);
            p_cont = emit_phi(fin.pred_cont, fin.pred_blocks, n.loc);
            forget(p_brk); forget(p_cont);
        }
        // The pending values must survive the cleanup, which may run arbitrary
        // code, so they are frame-owned across it rather than statement temps.
        forget(p_exc); forget(p_ret);
        frame_owned_.push_back(p_exc);
        frame_owned_.push_back(p_ret);
        for (const stmt& s2 : n.finalbody) if (!lower_stmt(s2)) return false;
        frame_owned_.pop_back();
        frame_owned_.pop_back();

        // Dispatch. Exception first: CPython re-raises after the cleanup
        // unless the cleanup itself left by another route.
        std::uint32_t exc_b  = new_block("finally.reraise");
        std::uint32_t noexc_b = new_block("finally.noexc");
        std::uint32_t ret_b  = new_block("finally.return");
        {
            ir::Value nn = const_null(n.loc);
            ir::Value isnull = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            emit(ir::Instr{ir::Op::Is, {p_exc, nn}, isnull, Ownership::NotAnObject,
                           "", 0, 0, n.loc, std::nullopt});
            emit(ir::Instr{ir::Op::CondBr, {isnull}, std::nullopt,
                           Ownership::NotAnObject, "", noexc_b, exc_b,
                           n.loc, std::nullopt});
        }

        set_block(exc_b);
        // SetRaisedException STEALS, so p_exc must not be released after it.
        mark_owned(p_exc);
        call_capi("PyErr_SetRaisedException", {p_exc}, n.loc, &ok);
        if (!ok) return false;
        forget(p_exc);
        {
            // Propagate outward on whatever encloses THIS try.
            std::uint32_t pad = make_landing_pad(n.loc);
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", pad, 0, n.loc, std::nullopt});
        }

        set_block(noexc_b);
        {
            ir::Value nn = const_null(n.loc);
            ir::Value isnull = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            emit(ir::Instr{ir::Op::Is, {p_ret, nn}, isnull, Ownership::NotAnObject,
                           "", 0, 0, n.loc, std::nullopt});
            emit(ir::Instr{ir::Op::CondBr, {isnull}, std::nullopt,
                           Ownership::NotAnObject, "", after, ret_b,
                           n.loc, std::nullopt});
        }

        set_block(ret_b);
        mark_owned(p_ret);
        if (!finish_return(p_ret, n.loc)) return false;

        // break and continue, in that order, after return. Emitted only when
        // one actually reached this cleanup: otherwise the dispatch would
        // branch to a loop that does not exist, and the shared exit path
        // reported "break outside a loop" for a try/finally containing no
        // break at all.
        set_block(after);
        if (!fin.any_jump) return true;

        // Both tests are computed BEFORE the markers are released: the
        // comparison only inspects the pointer, but reading a pointer whose
        // reference has already been dropped is the kind of detail that is
        // true until it isn't.
        ir::Value nb = const_null(n.loc), nc = const_null(n.loc);
        ir::Value is_brk = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        emit(ir::Instr{ir::Op::Is, {p_brk, nb}, is_brk, Ownership::NotAnObject,
                       "", 0, 0, n.loc, std::nullopt});
        ir::Value is_cont = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        emit(ir::Instr{ir::Op::Is, {p_cont, nc}, is_cont, Ownership::NotAnObject,
                       "", 0, 0, n.loc, std::nullopt});
        // At most one marker is non-null; Py_DecRef is Py_XDECREF, so
        // releasing both unconditionally is correct on every path.
        mark_owned(p_brk); mark_owned(p_cont);
        release(p_brk, n.loc); release(p_cont, n.loc);

        std::uint32_t brk_b = new_block("finally.break");
        std::uint32_t nobrk_b = new_block("finally.nobreak");
        emit(ir::Instr{ir::Op::CondBr, {is_brk}, std::nullopt,
                       Ownership::NotAnObject, "", nobrk_b, brk_b,
                       n.loc, std::nullopt});
        set_block(brk_b);
        if (!finish_jump(true, n.loc)) return false;

        set_block(nobrk_b);
        std::uint32_t cont_b = new_block("finally.continue");
        std::uint32_t done_b = new_block("finally.done");
        emit(ir::Instr{ir::Op::CondBr, {is_cont}, std::nullopt,
                       Ownership::NotAnObject, "", done_b, cont_b,
                       n.loc, std::nullopt});
        set_block(cont_b);
        if (!finish_jump(false, n.loc)) return false;

        set_block(done_b);
        return true;
    }

    // break/continue, honouring any enclosing finally the same way a return
    // does: hand the pending jump to the nearest cleanup instead of branching
    // straight out, so it still runs.
    bool finish_jump(bool is_break, const SourceLoc& loc) {
        pop_open_handlers(loc);
        if (!fin_stack_.empty() && loops_.size() <= fin_loop_depth_) {
            FinallyCtx* f = fin_stack_.back();
            f->any_jump = true;
            ir::Value mark = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, mark, Ownership::Owned, "",
                           0, 0, loc, std::nullopt});
            forget(mark);
            f->edge((std::uint32_t)blk_, const_null(loc), const_null(loc),
                    is_break ? mark : const_null(loc),
                    is_break ? const_null(loc) : mark);
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", f->entry, 0, loc, std::nullopt});
            return true;
        }
        if (loops_.empty())
            return err(is_break ? "break outside a loop" : "continue outside a loop",
                       is_break ? "break" : "continue", loc);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject, "",
                       is_break ? loops_.back().done : loops_.back().head,
                       0, loc, std::nullopt});
        return true;
    }

    // Complete a return, honouring any enclosing finally: the value is handed
    // to the nearest pending cleanup rather than returned directly, so every
    // cleanup between here and the function boundary still runs.
    bool finish_return(const ir::Value& v, const SourceLoc& loc) {
        pop_open_handlers(loc);
        // The returned reference is handed on, so it must NOT be released --
        // but every other live temporary must be, or an early return leaks
        // exactly what a landing pad would have freed.
        for (auto it = owned_.rbegin(); it != owned_.rend(); ++it)
            if (it->id != v.id) emit_decref(*it, loc);
        if (!fin_stack_.empty()) {
            FinallyCtx* f = fin_stack_.back();
            f->edge((std::uint32_t)blk_, const_null(loc), v,
                    const_null(loc), const_null(loc));
            forget(v);
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", f->entry, 0, loc, std::nullopt});
            return true;
        }
        emit(ir::Instr{ir::Op::Return, {v}, std::nullopt, Ownership::NotAnObject,
                       "", 0, 0, loc, std::nullopt});
        forget(v);
        return true;
    }

    bool lower_try_except(const Try& n) {
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
        // Record what is being handled, so a bare `raise` inside the handler
        // has something to re-raise.
        ir::Value prev_handled = call_capi("pyc_rt_push_handled", {exc}, n.loc, &ok);
        if (!ok) return false;
        mark_owned(prev_handled);
        frame_owned_.push_back(prev_handled);

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
            // Open for the duration of the handler body, so a return, break or
            // continue out of it pops before leaving.
            handled_stack_.push_back(prev_handled);
            for (const stmt& s2 : eh.body)
                if (!lower_stmt(s2)) { handled_stack_.pop_back(); return false; }
            handled_stack_.pop_back();
            if (terminated()) { set_block(next_b); continue; }
            call_capi("pyc_rt_pop_handled", {prev_handled}, n.loc, &ok);
            if (!ok) return false;
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});
            set_block(next_b);
        }

        // No handler matched: put the exception back and propagate. The
        // reference is STOLEN by SetRaisedException, so it must not be
        // released afterwards -- §4's table makes that automatic.
        call_capi("pyc_rt_pop_handled", {prev_handled}, n.loc, &ok);
        if (!ok) return false;
        frame_owned_.pop_back();          // prev_handled
        if (owns(prev_handled)) release(prev_handled, n.loc);
        frame_owned_.pop_back();          // exc
        call_capi("PyErr_SetRaisedException", {exc}, n.loc, &ok);
        if (!ok) return false;
        forget(exc);
        std::uint32_t pad = make_landing_pad(n.loc);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", pad, 0, n.loc, std::nullopt});

        set_block(after);
        if (owns(prev_handled)) release(prev_handled, n.loc);
        return true;
    }

    static bool exits_block(const stmt& s2) {
        bool e = false;
        std::visit(ov{
            [&](const Return&){ e = true; }, [&](const Break&){ e = true; },
            [&](const Continue&){ e = true; },
            [&](const Expr&){}, [&](const Assign&){}, [&](const AugAssign&){},
            [&](const AnnAssign&){}, [&](const If&){}, [&](const While&){},
            [&](const For&){}, [&](const AsyncFor&){}, [&](const With&){},
            [&](const AsyncWith&){}, [&](const Try&){}, [&](const TryStar&){},
            [&](const Match&){}, [&](const FunctionDef&){},
            [&](const AsyncFunctionDef&){}, [&](const ClassDef&){},
            [&](const Import&){}, [&](const ImportFrom&){}, [&](const Global&){},
            [&](const Nonlocal&){}, [&](const Pass&){}, [&](const Raise&){},
            [&](const Assert&){}, [&](const Delete&){}, [&](const TypeAlias&){},
        }, s2.v);
        return e;
    }

    // A leading string literal in a body is its docstring. CPython exposes it
    // as __doc__; without this the attribute reads as None on every function.
    bool set_docstring(const ir::Value& fv, const std::vector<stmt>& body,
                       const SourceLoc& loc) {
        if (body.empty()) return true;
        const Expr* e = std::get_if<Expr>(&body.front().v);
        if (!e) return true;
        const Constant* c = std::get_if<Constant>(&e->value->v);
        if (!c || !std::holds_alternative<ConstStr>(c->value.v)) return true;
        bool ok = true;
        ir::Value doc = lower_expr(*e->value, &ok);
        if (!ok) return false;
        ir::Value key = const_str("__doc__", loc);
        call_capi("PyObject_SetAttr", {fv, key, doc}, loc, &ok, {key, doc});
        return ok;
    }

    bool lower_classdef(const ClassDef& n) {
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
        ir::Value ccell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::CellNew, {}, ccell, Ownership::Owned, "__class__",
                       0, 0, n.loc, make_landing_pad(n.loc)});
        mark_owned(ccell);
        frame_owned_.push_back(ccell);
        class_cells_.push_back(ccell);

        class_ns_.push_back(ns);
        qual_.push_back(n.name);
        frame_owned_.push_back(ns);
        frame_owned_.push_back(bases);
        auto saved_locals = locals_;
        locals_.clear();                  // names in a class body are not fast locals
        for (const stmt& s2 : n.body)
            if (!lower_stmt(s2)) { class_ns_.pop_back(); qual_.pop_back();
                                   class_cells_.pop_back(); return false; }
        locals_ = saved_locals;
        class_ns_.pop_back();
        qual_.pop_back();
        class_cells_.pop_back();
        frame_owned_.pop_back();
        frame_owned_.pop_back();

        ir::Value cls = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::BuildClass, {bases, ns}, cls, Ownership::Owned,
                       n.name, 0, 0, n.loc, make_landing_pad(n.loc)});
        mark_owned(cls);
        if (owns(bases)) release(bases, n.loc);
        if (owns(ns)) release(ns, n.loc);
        // Fill __class__ BEFORE decorators run: CPython binds the cell to the
        // undecorated class, which is what super() in a method resolves to.
        emit(ir::Instr{ir::Op::CellSet, {ccell, cls}, std::nullopt,
                       Ownership::NotAnObject, "__class__", 0, 0, n.loc, std::nullopt});
        frame_owned_.pop_back();
        release(ccell, n.loc);
        cls = apply_decorators(n.decorator_list, cls, n.loc, &ok);
        if (!ok) return false;
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
            if (a.name == "*") {
                bool wok = true;
                std::string m2 = n.module ? *n.module : std::string();
                if (m2.empty()) return unsupported("wildcard import without a module", n.loc);
                ir::Value mod = emit_import(m2, false, n.loc);
                call_capi("pyc_rt_import_star", {mod}, n.loc, &wok, {mod});
                return wok;
            }
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
        std::uint32_t done = new_block("for.done");     // iterator exhausted
        // `else` runs only when the loop was NOT broken out of, so exhaustion
        // and break cannot share an exit. They did, which is why for/else was
        // refused. Both must still release the iterator exactly once.
        std::uint32_t brk   = new_block("for.break");
        std::uint32_t after = new_block("for.after");
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});
        set_block(head);
        ir::Value item = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::IterNext, {it}, item, Ownership::Owned, "",
                       body, done, n.loc, make_landing_pad(n.loc)});

        set_block(body);
        mark_owned(item);
        // Any assignable target works, so `for k, v in pairs` unpacks through
        // the same path a plain assignment does.
        if (!store_target(*n.target, item, n.loc)) return false;
        loops_.push_back({head, brk});
        for (const stmt& s2 : n.body) if (!lower_stmt(s2)) return false;
        loops_.pop_back();
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});

        // Broken out of: release the iterator and skip the else.
        set_block(brk);
        emit_decref(it, n.loc);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, n.loc, std::nullopt});

        // Ran to exhaustion: release the iterator, then the else body.
        set_block(done);
        frame_owned_.pop_back();
        if (owns(it)) release(it, n.loc);
        for (const stmt& s2 : n.orelse) if (!lower_stmt(s2)) return false;
        if (!terminated())
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});

        set_block(after);
        return true;
    }

    bool lower_while(const While& n) {
        std::uint32_t head = new_block("while.head");
        std::uint32_t body = new_block("while.body");
        std::uint32_t done = new_block("while.done");   // test went false
        std::uint32_t brk   = new_block("while.break");
        std::uint32_t after = new_block("while.after");
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});
        set_block(head);
        bool ok = true;
        ir::Value t = lower_predicate(*n.test, &ok);
        if (!ok) return false;
        emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt, Ownership::NotAnObject,
                       "", body, done, n.loc, std::nullopt});
        set_block(body);
        loops_.push_back({head, brk});
        for (const stmt& s2 : n.body) if (!lower_stmt(s2)) return false;
        loops_.pop_back();
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});

        set_block(brk);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, n.loc, std::nullopt});

        set_block(done);
        for (const stmt& s2 : n.orelse) if (!lower_stmt(s2)) return false;
        if (!terminated())
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});

        set_block(after);
        return true;
    }

    std::uint32_t pad_n_ = 0;
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
        // `a = b = 1` binds every target to the SAME object, left to right.
        for (std::size_t i = 0; i + 1 < a.targets.size(); ++i) {
            bool ok2 = true;
            store_target_keep(a.targets[i], v, a.loc, &ok2);
            if (!ok2) return false;
        }
        return store_target(a.targets.back(), v, a.loc);
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
            [&](const NamedExpr& n)     {
                out = lower_expr(*n.value, ok);
                if (!*ok) return;
                if (!std::holds_alternative<Name>(n.target->v)) {
                    *ok = unsupported("walrus with a non-name target", n.loc);
                    return;
                }
                // The value is BOTH bound and yielded, so the store must not
                // consume our reference -- the expression still evaluates to it.
                store_name_keep(std::get<Name>(n.target->v).id, out, n.loc);
            },
            [&](const UnaryOp& n)       { out = lower_unaryop(n, ok); },
            [&](const Lambda& n)        { out = lower_lambda(n, ok); },
            [&](const IfExp& n)         { out = lower_ifexp(n, ok); },
            [&](const Dict& n)          { out = lower_dict(n, ok); },
            [&](const Set& n)           { out = lower_set(n, ok); },
            [&](const ListComp& n)      { out = lower_comp(n.generators, &*n.elt, nullptr, "list", n.loc, ok); },
            [&](const SetComp& n)       { out = lower_comp(n.generators, &*n.elt, nullptr, "set", n.loc, ok); },
            [&](const DictComp& n)      { out = lower_comp(n.generators, &*n.value, &*n.key, "dict", n.loc, ok); },
            [&](const GeneratorExp& n)  { out = lower_genexp(n, ok); },
            [&](const Await& n)         { *ok = unsupported("await", n.loc); },
            [&](const Yield& n)         { *ok = unsupported("yield", n.loc); },
            [&](const YieldFrom& n)     { *ok = unsupported("yield from", n.loc); },
            [&](const Compare& n)       { out = lower_compare(n, ok); },
            [&](const FormattedValue& n){ out = lower_formatted(n, ok); },
            [&](const JoinedStr& n)     { out = lower_joined(n, ok); },
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
            [&](const ConstComplex& v) {
                in.op = ir::Op::ConstComplex;
                // LLVM needs a literal it can parse as a double; %.17g is
                // exact for IEEE-754 round-tripping.
                char buf[80];
                std::snprintf(buf, sizeof buf, "%.17g %.17g", v.real, v.imag);
                in.text = buf;
            },
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
        auto cit = cells_.find(n.id);
        if (cit != cells_.end()) {
            ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::LoadLocal, {}, cell, Ownership::Owned, n.id,
                           cit->second, 0, n.loc, make_landing_pad(n.loc)});
            mark_owned(cell);
            emit(ir::Instr{ir::Op::CellGet, {cell}, out, Ownership::Owned, n.id,
                           0, 0, n.loc, make_landing_pad(n.loc)});
            release(cell, n.loc);
            mark_owned(out);
            *ok = true;
            return out;
        }
        auto it = locals_.find(n.id);
        if (it != locals_.end()) {
            // May raise UnboundLocalError: a local read before assignment is
            // an error, not a fallback to the global of the same name.
            emit(ir::Instr{ir::Op::LoadLocal, {}, out, Ownership::Owned,
                           n.id, it->second, 0, n.loc, make_landing_pad(n.loc)});
        } else if (!class_ns_.empty()) {
            // A class body is LOAD_NAME territory: the namespace under
            // construction, then globals, then builtins.
            emit(ir::Instr{ir::Op::LoadClassName, {class_ns_.back()}, out,
                           Ownership::Owned, n.id, 0, 0, n.loc,
                           make_landing_pad(n.loc)});
            mark_owned(out);
            *ok = true;
            return out;
        } else {
            emit(ir::Instr{ir::Op::LoadGlobal, {}, out, Ownership::Owned,
                           n.id, 0, 0, n.loc, make_landing_pad(n.loc)});
        }
        mark_owned(out);
        *ok = true;
        return out;
    }

    // super(__class__, self): __class__ from the implicit closure cell, self
    // from local slot 0 -- the first parameter of the enclosing method.
    ir::Value lower_super_zero(const Call& c, bool* ok) {
        ir::Value fn = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::LoadGlobal, {}, fn, Ownership::Owned, "super",
                       0, 0, c.loc, make_landing_pad(c.loc)});
        mark_owned(fn);
        std::uint32_t cslot = cells_["__class__"];
        ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::LoadLocal, {}, cell, Ownership::Owned, "__class__",
                       cslot, 0, c.loc, make_landing_pad(c.loc)});
        mark_owned(cell);
        ir::Value klass = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::CellGet, {cell}, klass, Ownership::Owned, "__class__",
                       0, 0, c.loc, make_landing_pad(c.loc)});
        mark_owned(klass);
        release(cell, c.loc);
        ir::Value self = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::LoadLocal, {}, self, Ownership::Owned, "self",
                       0, 0, c.loc, make_landing_pad(c.loc)});
        mark_owned(self);
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::CallObject, {fn, klass, self}, out, Ownership::Owned,
                       "", 0, 0, c.loc, make_landing_pad(c.loc)});
        mark_owned(out);
        release(fn, c.loc); release(klass, c.loc); release(self, c.loc);
        *ok = true;
        return out;
    }

    ir::Value lower_call(const Call& c, bool* ok) {
        // Zero-argument super() is not a plain call: CPython's super() reads
        // the calling FRAME for the class cell and the first argument. pyc has
        // no Python frames, so it raises "super(): no current frame". Supply
        // both operands explicitly -- super(__class__, self) -- which is what
        // the zero-argument form means.
        if (std::holds_alternative<Name>(c.func->v)
            && std::get<Name>(c.func->v).id == "super"
            && c.args.empty() && c.keywords.empty()) {
            auto cit = cells_.find("__class__");
            if (cit != cells_.end() && !locals_.empty() && class_ns_.empty()) {
                ir::Value out = lower_super_zero(c, ok);
                if (out.valid() || !*ok) return out;
            }
            // Not resolvable: CPython raises at run time, and which message it
            // uses depends on whether the frame has arguments at all.
            if (class_ns_.empty() && fn_idx_ != 0) {
                bool sok = true;
                call_capi_imm("pyc_rt_super_fail", {},
                              (std::int64_t)(cur()->params.empty() ? 0 : 1), 0,
                              c.loc, &sok);
                *ok = sok;
                return {};
            }
        }
        ir::Value fn = lower_expr(*c.func, ok);
        if (!*ok) return {};
        bool starred = false;
        for (const expr& a : c.args)
            if (std::holds_alternative<Starred>(a.v)) starred = true;
        if (starred) return lower_call_starred(c, fn, ok);

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
    // `[*a, b]` splices a's contents rather than nesting it, so the size is
    // not known until run time: build a list, then convert.
    ir::Value lower_spliced(const std::vector<expr>& elts, const char* kind,
                            const SourceLoc& loc, bool* ok) {
        ir::Value lst = call_capi_imm("PyList_New", {}, 0, 0, loc, ok);
        if (!*ok) return {};
        mark_owned(lst);
        for (const expr& e : elts) {
            if (const Starred* st = std::get_if<Starred>(&e.v)) {
                ir::Value v = lower_expr(*st->value, ok);
                if (!*ok) return {};
                call_capi("pyc_rt_extend", {lst, v}, loc, ok, {v});
            } else {
                ir::Value v = lower_expr(e, ok);
                if (!*ok) return {};
                call_capi("PyList_Append", {lst, v}, loc, ok, {v});
            }
            if (!*ok) return {};
        }
        std::string k(kind);
        if (k == "PyList") return lst;             // already a list
        const char* conv = (k == "PyTuple") ? "PySequence_Tuple" : "PySet_New";
        ir::Value out = call_capi(conv, {lst}, loc, ok, {lst});
        if (*ok) mark_owned(out);
        return out;
    }

    ir::Value lower_sequence(const std::vector<expr>& elts, const char* prefix,
                             const SourceLoc& loc, bool* ok) {
        for (const expr& e : elts)
            if (std::holds_alternative<Starred>(e.v))
                return lower_spliced(elts, prefix, loc, ok);
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
            if (std::holds_alternative<Starred>(e.v))
                return lower_spliced(n.elts, "PySet", n.loc, ok);
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
        if (n.keys.size() != n.values.size()) {
            *ok = err("dict literal has mismatched keys and values", "Dict", n.loc);
            return {};
        }
        ir::Value d = call_capi("PyDict_New", {}, n.loc, ok);
        if (!*ok) return {};
        mark_owned(d);
        for (std::size_t i = 0; i < n.keys.size(); ++i) {
            // A null key marks `**mapping` (INTERFACES §2.3): merge rather
            // than insert.
            if (!n.keys[i] || !*n.keys[i]) {
                ir::Value m = lower_expr(n.values[i], ok);
                if (!*ok) return {};
                call_capi("PyDict_Update", {d, m}, n.loc, ok, {m});
                if (!*ok) return {};
                continue;
            }
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

    void collect_target_names(const expr& e, std::vector<std::string>& out) {
        std::visit(ov{
            [&](const Name& n){ out.push_back(n.id); },
            [&](const Tuple& t){ for (const expr& x : t.elts) collect_target_names(x, out); },
            [&](const List& l){ for (const expr& x : l.elts) collect_target_names(x, out); },
            [&](const Starred& s2){ if (s2.value) collect_target_names(*s2.value, out); },
            [&](const Attribute&){}, [&](const Subscript&){},
            [&](const BinOp&){}, [&](const BoolOp&){}, [&](const NamedExpr&){},
            [&](const UnaryOp&){}, [&](const Lambda&){}, [&](const IfExp&){},
            [&](const Dict&){}, [&](const Set&){}, [&](const ListComp&){},
            [&](const SetComp&){}, [&](const DictComp&){}, [&](const GeneratorExp&){},
            [&](const Await&){}, [&](const Yield&){}, [&](const YieldFrom&){},
            [&](const Compare&){}, [&](const Call&){}, [&](const FormattedValue&){},
            [&](const JoinedStr&){}, [&](const TemplateStr&){}, [&](const Interpolation&){},
            [&](const Constant&){}, [&](const Slice&){},
        }, e.v);
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

    // A generator expression. pyc evaluates the outer iterable eagerly -- as
    // CPython does, so an exception in it surfaces at CREATION -- collects a
    // cell per free variable, and hands both to the interpreter along with the
    // code object built at compile time.
    ir::Value lower_genexp(const GeneratorExp& n, bool* ok) {
        const GenexpEntry* gx = find_genexp(n.loc);
        if (!gx) {
            *ok = err("no compiled code object for this generator expression",
                      "generator expressions", n.loc);
            return {};
        }
        if (n.generators.empty()) {
            *ok = err("generator expression with no for-clause", "generator expressions", n.loc);
            return {};
        }
        // 1. The OUTERMOST iterable, eagerly. An ASYNC generator expression
        //    takes __aiter__ rather than __iter__ -- CPython emits GET_AITER
        //    where it emits GET_ITER for the sync form -- and its `.0` is the
        //    async iterator. Only the first clause is evaluated here; the rest
        //    are inside the compiled body.
        const bool is_async = n.generators[0].is_async;
        ir::Value seq = lower_expr(*n.generators[0].iter, ok);
        if (!*ok) return {};
        ir::Value it = call_capi(is_async ? "PyObject_GetAIter" : "PyObject_GetIter",
                                 {seq}, n.loc, ok, {seq});
        if (!*ok) return {};
        mark_owned(it);

        // 2. A cell per free variable, in co_freevars order. pyc's closure
        //    analysis already forces a local read by a nested scope into a
        //    cell, and it counts generator expressions as nested reads. A
        //    freevar with no cell is a compile error, never a guess.
        ir::Value closure;
        if (!gx->freevars.empty()) {
            closure = call_capi_imm("PyTuple_New", {},
                                    (std::int64_t)gx->freevars.size(), 0, n.loc, ok);
            if (!*ok) return {};
            mark_owned(closure);
            for (std::size_t i = 0; i < gx->freevars.size(); ++i) {
                const std::string& name = gx->freevars[i];
                auto cit = cells_.find(name);
                if (cit == cells_.end()) {
                    *ok = err("generator expression captures '" + name +
                              "', which has no closure cell here",
                              "generator expressions", n.loc);
                    return {};
                }
                ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::LoadLocal, {}, cell, Ownership::Owned, name,
                               cit->second, 0, n.loc, make_landing_pad(n.loc)});
                mark_owned(cell);
                call_capi_imm("PyTuple_SetItem", {closure, cell},
                              (std::int64_t)i, 1, n.loc, ok);   // steals cell
                if (!*ok) return {};
            }
        }

        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        ir::Instr mk{ir::Op::MakeGenexp,
                     {closure.valid() ? closure : ir::Value{}, it},
                     out, Ownership::Owned, gx->code, 0, 0, n.loc,
                     make_landing_pad(n.loc)};
        emit(std::move(mk));
        mark_owned(out);
        if (closure.valid() && owns(closure)) release(closure, n.loc);
        if (owns(it)) release(it, n.loc);
        *ok = true;
        return out;
    }

    ir::Value lower_ifexp(const IfExp& n, bool* ok) {
        ir::Value t = lower_predicate(*n.test, ok);
        if (!*ok) return {};
        std::uint32_t tb = new_block("ifexp.then");
        std::uint32_t fb = new_block("ifexp.else");
        std::uint32_t jb = new_block("ifexp.join");
        emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt, Ownership::NotAnObject,
                       "", tb, fb, n.loc, std::nullopt});
        // Each arm must be lowered against the owned set as it stands at the
        // BRANCH, not as the other arm left it. Carrying the then-value into
        // the else arm put it in that arm's landing pads, which then decref a
        // value defined only on the path not taken -- LLVM rejects it as
        // "does not dominate all uses", and without that check it would be a
        // double free on the error path.
        auto at_branch = owned_;
        set_block(tb);
        ir::Value a = lower_expr(*n.body, ok);   if (!*ok) return {};
        std::uint32_t ae = (std::uint32_t)blk_;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", jb, 0, n.loc, std::nullopt});
        owned_ = at_branch;
        set_block(fb);
        ir::Value b = lower_expr(*n.orelse, ok); if (!*ok) return {};
        std::uint32_t be = (std::uint32_t)blk_;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", jb, 0, n.loc, std::nullopt});
        set_block(jb);
        // Past the join only the phi is live, on top of what was live before.
        owned_ = at_branch;
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

    // One interpolation: conversion (!r/!s/!a) first, then format spec.
    // The order matters -- `f"{x!r:>10}"` pads the repr, not the value.
    ir::Value lower_formatted(const FormattedValue& n, bool* ok) {
        ir::Value v = lower_expr(*n.value, ok);
        if (!*ok) return {};
        // conversion is -1 when absent; otherwise the ASCII code of r/s/a.
        if (n.conversion == 's' || n.conversion == 'r' || n.conversion == 'a') {
            const char* sym = n.conversion == 'r' ? "PyObject_Repr"
                            : n.conversion == 's' ? "PyObject_Str"
                                                  : "PyObject_ASCII";
            v = call_capi(sym, {v}, n.loc, ok, {v});
            if (!*ok) return {};
            mark_owned(v);
        }
        ir::Value spec;
        if (n.format_spec && *n.format_spec) {
            spec = lower_expr(**n.format_spec, ok);
            if (!*ok) return {};
        }
        // A null spec means "no spec", which is not the same as an empty one
        // for objects with a custom __format__.
        ir::Value out = call_capi("PyObject_Format", {v, spec}, n.loc, ok,
                                  spec.valid() ? std::vector<ir::Value>{v, spec}
                                               : std::vector<ir::Value>{v});
        if (*ok) mark_owned(out);
        return out;
    }

    // f"a{b}c" is the concatenation of its parts. Built with a list and
    // PyUnicode_Join so the cost is one allocation rather than one per piece.
    ir::Value lower_joined(const JoinedStr& n, bool* ok) {
        if (n.values.empty()) return const_str("", n.loc);
        if (n.values.size() == 1) {
            ir::Value only = lower_expr(n.values[0], ok);
            if (!*ok) return {};
            // A lone literal part is already a str; a lone interpolation was
            // formatted above, so both are strings already.
            return only;
        }
        ir::Value parts = call_capi_imm("PyList_New", {}, 0, 0, n.loc, ok);
        if (!*ok) return {};
        mark_owned(parts);
        for (const expr& e : n.values) {
            ir::Value v = lower_expr(e, ok);
            if (!*ok) return {};
            call_capi("PyList_Append", {parts, v}, n.loc, ok, {v});
            if (!*ok) return {};
        }
        ir::Value sep = const_str("", n.loc);
        ir::Value out = call_capi("PyUnicode_Join", {sep, parts}, n.loc, ok,
                                  {sep, parts});
        if (*ok) mark_owned(out);
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
    // f(*xs, k=v): the positional count is only known at run time, so the
    // argument tuple is built by splicing and the call goes through
    // PyObject_Call rather than the vectorcall fast path.
    // Build the keyword dict, splicing any `**mapping` with PyDict_Update.
    // A later key wins, which is the order Python specifies.
    ir::Value build_kwargs(const std::vector<keyword>& kws,
                           const SourceLoc& loc, bool* ok) {
        ir::Value kw = call_capi("PyDict_New", {}, loc, ok);
        if (!*ok) return {};
        mark_owned(kw);
        for (const keyword& k : kws) {
            ir::Value val = lower_expr(*k.value, ok);
            if (!*ok) return {};
            if (k.arg) {
                ir::Value key = const_str(*k.arg, loc);
                call_capi("PyDict_SetItem", {kw, key, val}, loc, ok, {key, val});
            } else {
                call_capi("PyDict_Update", {kw, val}, loc, ok, {val});
            }
            if (!*ok) return {};
        }
        return kw;
    }

    ir::Value lower_call_starred(const Call& c, const ir::Value& fn, bool* ok) {
        ir::Value tup = lower_spliced(c.args, "PyTuple", c.loc, ok);
        if (!*ok) return {};
        ir::Value kw;
        if (!c.keywords.empty()) {
            kw = build_kwargs(c.keywords, c.loc, ok);
            if (!*ok) return {};
        }
        std::vector<ir::Value> consume{fn, tup};
        if (kw.valid()) consume.push_back(kw);
        ir::Value out = call_capi("PyObject_Call", {fn, tup, kw}, c.loc, ok, consume);
        if (*ok) mark_owned(out);
        return out;
    }

    ir::Value lower_call_kw(const Call& c, const ir::Value& fn,
                            const std::vector<ir::Value>& all, bool* ok) {
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
        ir::Value kw = build_kwargs(c.keywords, c.loc, ok);
        if (!*ok) return {};
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
                 ir::Module& out, DiagnosticSink& diags,
                 const std::vector<GenexpEntry>& genexps) {
    out.source_file = file;
    Lowerer l(out, diags, genexps);
    return l.lower_module(tree);
}

}  // namespace pyc
