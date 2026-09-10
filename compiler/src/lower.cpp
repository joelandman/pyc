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
#include "pyc/ast/generated_walk.hpp"
#include "pyc/genexp.hpp"
#include "pyc/ir/ir.hpp"
#include "pyc/rt/capi.hpp"

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace pyc {
// Locals provably holding only ints, and so eligible to live in a machine
// register. Reported by PYC_DUMP_INTLOCALS while the analysis is being
// validated against the corpus, ahead of anything depending on it.
std::set<std::string> int_locals(const std::vector<std::string>&,
                                 const std::vector<pyc::ast::stmt>&,
                                 const std::set<std::string>&);
std::vector<std::string> function_locals(const std::vector<std::string>&,
                                        const std::vector<pyc::ast::stmt>&);
std::set<std::string> nested_reads(const std::vector<pyc::ast::stmt>&);
std::set<std::string> declared_nonlocals(const std::vector<pyc::ast::stmt>&);
std::set<std::string> declared_globals(const std::vector<pyc::ast::stmt>&);
std::set<std::string> all_reads(const std::vector<pyc::ast::stmt>&);
std::set<std::string> all_writes(const std::vector<pyc::ast::stmt>&);
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
        mod_.functions.push_back(ir::Function{.name="__main__", .next_value=1});
        fn_idx_ = mod_.functions.size() - 1;
        cur()->blocks.push_back(ir::Block{"entry", {}});
        blk_ = 0;
        // Bound BEFORE the first statement runs. Measured: CPython answers
        // True to `"__annotate__" in vars(module)` even on the line above the
        // first annotation, and __annotations__ is CACHED on first access, so
        // binding it late produced a permanently empty dict for any module
        // that reads its own annotations.
        AnnItems mann;
        collect_annotations(mm->body, mann);
        if (!emit_annotate(mann, {})) return false;
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
    std::set<std::string> int_locals_;
    std::map<std::string, ir::Value> live_i64_;
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> int_bounds_;
    std::vector<std::optional<std::int64_t>> loop_trips_;
    struct RangeBoundScope {
        Lowerer& L;
        std::string name;
        std::optional<std::pair<std::int64_t, std::int64_t>> saved;
        RangeBoundScope(Lowerer& l, std::string n, bool have,
                        std::int64_t lo, std::int64_t hi, std::int64_t trip)
            : L(l), name(std::move(n)) {
            auto it = L.int_bounds_.find(name);
            if (it != L.int_bounds_.end()) saved = it->second;
            if (have) {
                L.int_bounds_[name] = {lo, hi};
                L.loop_trips_.push_back(trip);
            } else {
                L.int_bounds_.erase(name);
                L.loop_trips_.push_back(std::nullopt);
            }
        }
        ~RangeBoundScope() {
            if (!L.loop_trips_.empty()) L.loop_trips_.pop_back();
            if (saved) L.int_bounds_[name] = *saved;
            else L.int_bounds_.erase(name);
        }
    };
    bool force_boxed_ints_ = false;
    std::vector<const std::vector<stmt>*> body_stk_;
    std::vector<std::size_t> stmt_idx_;
    struct I64Phi {
        std::uint32_t head = 0;
        std::string name;
        std::size_t instr_idx = 0;
        ir::Value phi;
    };
    std::vector<I64Phi> i64_phis_;
    std::uint32_t range_n_ = 0;
    std::string range_ctr_;
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
        // One decref per ENTRY, not per id. owned_ is a bag, not a set:
        // mark_owned is an unconditional push_back and forget erases the first
        // match and breaks, so two entries mean two references.
        //
        // This used to de-duplicate by SSA id, because promotion to the frame
        // was a COPY -- mark_owned(v) followed by frame_owned_.push_back(v),
        // one reference in two lists -- and releasing from each emitted two
        // decrefs into one pad. The de-dup masked that double free, but it can
        // only ever be right while nothing holds two GENUINE references, which
        // nothing structurally guaranteed.
        //
        // Promotion is now a MOVE at every site (forget then push), so a value
        // is in exactly one list per reference it holds, and each entry gets
        // its own decref. Do not reintroduce the de-dup: with moves in place it
        // would UNDER-release a value that legitimately holds two references.
        for (auto it = owned_.rbegin(); it != owned_.rend(); ++it)
            emit_decref(*it, loc);
        // An exception leaving an except handler must pop that handler's
        // exc_info, exactly as return/break/continue do via pop_open_handlers.
        // Only the frame-owned decref happened here, and dropping the
        // reference is not the same as popping the stack: CPython's
        // "currently handled exception" stayed set for the rest of the
        // program, so
        //
        //     try:
        //         try: raise ValueError("inner")
        //         except ValueError: raise TypeError("outer")
        //     except TypeError: pass
        //     raise KeyError("later")
        //
        // gave the KeyError a __context__ of ValueError('inner') -- an
        // exception that had been fully handled two statements earlier. Every
        // later raise in the program inherited it, and its traceback claimed
        // "During handling of the above exception, another exception occurred"
        // about something unrelated.
        //
        // The same defect was fixed once for the return path; the unwind path
        // was missed, because there the pad already touched the value and it
        // looked handled.
        //
        // This is only safe because pyc_rt_pop_handled is may_raise = false.
        // While it was marked as raising, call_capi built a landing pad for
        // it, that pad popped handlers, and the compiler recursed until it
        // hung -- diagnosing nothing at all.
        pop_open_handlers(loc);
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
        // Unreachable: this block already ended. emit() drops instructions in
        // a terminated block, but make_landing_pad builds pads in OTHER blocks
        // and those pads still referenced the dropped values -- so
        //
        //     def f():
        //         return 3
        //         raise RuntimeError("unreachable")
        //
        // emitted `call void @Py_DecRef(ptr %v2)` in a pad for a %v2 that was
        // never defined, and the module failed to assemble. Four lines of
        // Python, and it blocked Lib/test/test_compile and test_opcodes.
        //
        // Lowering nothing is also what CPython does with the statement: it is
        // compiled for syntax and never executed.
        if (terminated()) return true;
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
            [&](const AnnAssign& n)        { ok = lower_annassign(n); },
            [&](const For& n)              { ok = lower_for(n); },
            [&](const AsyncFor& n)         { ok = unsupported("async for", n.loc); },
            [&](const While& n)            { ok = lower_while(n); },
            [&](const If& n)               { ok = lower_if(n); },
            [&](const With& n)             { ok = lower_with(n); },
            [&](const AsyncWith& n)        { ok = unsupported("async with", n.loc); },
            [&](const Match& n)            { ok = lower_match(n); },
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
                ir::Value e = lower_expr(**n.exc, &ok);
                if (!ok) return;
                if (n.cause) {
                    // Exception first, then cause: that is the order CPython's
                    // compiler emits, and it is observable when either
                    // expression has a side effect.
                    ir::Value c = lower_expr(**n.cause, &ok);
                    if (!ok) return;
                    call_capi("pyc_rt_raise_from", {e, c}, n.loc, &ok, {e, c});
                    if (!ok) return;
                    std::uint32_t pad = make_landing_pad(n.loc);
                    emit(ir::Instr{ir::Op::Br, {}, std::nullopt,
                                   Ownership::NotAnObject, "", pad, 0, n.loc,
                                   std::nullopt});
                    return;
                }
                emit(ir::Instr{ir::Op::Raise, {e}, std::nullopt,
                               Ownership::NotAnObject, "", 0, 0, n.loc,
                               make_landing_pad(n.loc)});
            },
            [&](const Try& n)              { ok = lower_try(n); },
            [&](const TryStar& n)          { ok = lower_trystar(n); },
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
            [&](const TypeAlias& n)        { ok = lower_type_alias(n); },
        }, s.v);
        return ok;
    }

    // Truthiness goes through PyObject_IsTrue like everything else -- there is
    // no fast path for "obviously a bool", because that would be a proof we
    // have not made (I2). An i64 compare is a different proof: both sides are
    // int_locals or constants, so the predicate is an icmp, not a type test.
    ir::Value lower_predicate(const expr& e, bool* ok) {
        ir::Value fast;
        if (try_int_predicate(e, &fast, ok)) return fast;
        if (!*ok) return {};
        return lower_predicate_boxed(e, ok);
    }

    ir::Value lower_predicate_boxed(const expr& e, bool* ok) {
        ir::Value v = lower_expr(e, ok);
        if (!*ok) return {};
        ir::Value t = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
        SourceLoc loc{};
        emit(ir::Instr{ir::Op::IsTrue, {v}, t, Ownership::NotAnObject,
                       "PyObject_IsTrue", 0, 0, loc, make_landing_pad(loc)});
        if (owns(v)) release(v, loc);
        return t;
    }

    struct Loop { std::uint32_t head, done; std::uint32_t boxed_head = 0; };

    // Defined further down, next to the try/finally lowering that builds it.
    struct FinallyCtx;

    // Saved lowering state while a nested function is built.
    struct FnScope {
        std::size_t fn, blk;
        std::map<std::string, std::uint32_t> locals;
        std::vector<ir::Value> owned, frame, class_ns, class_cells;
        std::vector<Loop> loops;
        std::vector<std::uint32_t> tries;
        std::map<std::string, std::uint32_t> cells;
        std::vector<std::map<std::string, std::uint32_t>> enclosing;
        // A nested function is a DIFFERENT function: the enclosing one's
        // pending cleanups and open handlers do not apply to it, and its
        // blocks are not addressable from here. See begin_function.
        std::vector<FinallyCtx*> fins;
        std::vector<ir::Value> handled;
        std::size_t fin_depth;
        std::set<std::string> ints;
        std::map<std::string, ir::Value> live_i64;
        bool force_boxed_ints;
        std::uint32_t ranges;
        std::vector<std::map<std::string, ir::Value>> type_param_env;
    };

    FnScope begin_function(const std::string& name,
                           const std::vector<std::string>& params,
                           const std::vector<std::string>& locals) {
        FnScope sc{fn_idx_, blk_, locals_, owned_, frame_owned_, class_ns_,
                   class_cells_,
                   loops_, try_stack_, cells_, enclosing_cells_,
                   fin_stack_, handled_stack_, fin_loop_depth_, int_locals_,
                   live_i64_, force_boxed_ints_, range_n_};
        sc.type_param_env = type_param_env_;
        type_param_env_.clear();
        mod_.functions.push_back(
            ir::Function{.name=name, .params=params, .next_value=1});
        fn_idx_ = mod_.functions.size() - 1;
        cur()->locals = locals;
        locals_.clear();
        for (std::uint32_t i = 0; i < locals.size(); ++i) locals_[locals[i]] = i;
        owned_.clear(); frame_owned_.clear(); class_ns_.clear();
        class_cells_.clear();
        loops_.clear(); try_stack_.clear();
        // These are per-FUNCTION and were not being cleared. A `def` written
        // inside a try/finally or a `with` therefore inherited the enclosing
        // function's pending cleanup, so its `return` branched to a block that
        // exists in a DIFFERENT function -- "use of undefined value '%bbN'",
        // and the whole module failed to assemble.
        //
        // Pre-existing for try/finally; harmless for `with` only until `with`
        // gained a cleanup of its own, at which point it reached far more code
        // because unittest suites nest defs inside `with` constantly.
        fin_stack_.clear(); handled_stack_.clear(); fin_loop_depth_ = 0;
        enclosing_cells_.push_back(sc.cells);
        cells_.clear();
        int_locals_.clear();
        live_i64_.clear();
        int_bounds_.clear();
        loop_trips_.clear();
        force_boxed_ints_ = false;
        range_n_ = 0;
        cur()->blocks.push_back(ir::Block{"entry", {}});
        blk_ = 0;
        return sc;
    }

    std::size_t end_function(const FnScope& sc) {
        std::size_t made = fn_idx_;
        fn_idx_ = sc.fn; blk_ = sc.blk; locals_ = sc.locals;
        owned_ = sc.owned; frame_owned_ = sc.frame; class_ns_ = sc.class_ns;
        class_cells_ = sc.class_cells;
        loops_ = sc.loops; try_stack_ = sc.tries;
        cells_ = sc.cells; enclosing_cells_ = sc.enclosing;
        fin_stack_ = sc.fins; handled_stack_ = sc.handled;
        fin_loop_depth_ = sc.fin_depth;
        int_locals_ = sc.ints;
        live_i64_ = sc.live_i64;
        force_boxed_ints_ = sc.force_boxed_ints;
        range_n_ = sc.ranges;
        type_param_env_ = sc.type_param_env;
        return made;
    }

    ir::Value make_function_value(std::size_t idx, const std::string& name,
                                  const SourceLoc& loc,
                                  const std::vector<ir::Value>& closure = {},
                                  ir::Value defaults = {},
                                  int vararg_slot = -1, int kwarg_slot = -1,
                                  ir::Value kwdefaults = {}) {
        ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        // args[0] = defaults, args[1] = kwdefaults, args[2..] = closure cells.
        std::vector<ir::Value> mkargs{defaults, kwdefaults};
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
                // The TARGET as well as the value. A walrus inside a
                // comprehension WRITES to the enclosing scope, so the name has
                // to be captured for the write to land there -- capturing only
                // what is read left `{k: (v := k*2) for k in ...}` writing to a
                // local slot of the synthetic function, and the enclosing `v`
                // was never assigned at all.
                [&](const NamedExpr& n){ go(*n.value); go(*n.target); },
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
        // trampoline, same slots -- including the `/` and `*` markers, which
        // are carried by nposonly and nkwonly exactly as they are for a def.
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
        const int nkwonly = (int)a.kwonlyargs.size();
        ir::Value lam_kwdefaults;
        if (nkwonly > 0) {
            bool any = false;
            for (const auto& kd : a.kw_defaults) if (kd.has_value()) any = true;
            if (any) {
                lam_kwdefaults = call_capi("PyDict_New", {}, n.loc, ok);
                if (!*ok) return {};
                mark_owned(lam_kwdefaults);
                for (std::size_t i = 0; i < a.kw_defaults.size() && i < a.kwonlyargs.size(); ++i) {
                    if (!a.kw_defaults[i].has_value()) continue;
                    ir::Value d = lower_expr(*a.kw_defaults[i].value(), ok);
                    if (!*ok) return {};
                    ir::Value k = const_str(a.kwonlyargs[i].arg, n.loc);
                    call_capi("PyDict_SetItem", {lam_kwdefaults, k, d}, n.loc, ok, {k, d});
                    if (!*ok) return {};
                }
            }
        }
        if (const GenexpEntry* gf = find_genexp(n.loc)) {
            ir::Value closure;
            if (!gf->freevars.empty()) {
                closure = call_capi_imm("PyTuple_New", {},
                                        (std::int64_t)gf->freevars.size(), 0, n.loc, ok);
                if (!*ok) return {};
                mark_owned(closure);
                for (std::size_t i = 0; i < gf->freevars.size(); ++i) {
                    const std::string& fv2 = gf->freevars[i];
                    auto cit = cells_.find(fv2);
                    if (cit == cells_.end()) {
                        *ok = err("function captures '" + fv2 +
                                  "', which has no closure cell here", "yield", n.loc);
                        return {};
                    }
                    ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                    emit(ir::Instr{ir::Op::LoadLocal, {}, cell, Ownership::Owned,
                                   fv2, cit->second, 0, n.loc, make_landing_pad(n.loc)});
                    mark_owned(cell);
                    call_capi_imm("PyTuple_SetItem", {closure, cell},
                                  (std::int64_t)i, 1, n.loc, ok);
                    if (!*ok) return {};
                }
            }
            ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::MakeGenFunc,
                           {closure.valid() ? closure : ir::Value{},
                            lam_defaults.valid() ? lam_defaults : ir::Value{},
                            lam_kwdefaults.valid() ? lam_kwdefaults : ir::Value{}},
                           fv, Ownership::Owned, gf->code, 0, 0, n.loc,
                           make_landing_pad(n.loc)});
            mark_owned(fv);
            if (closure.valid() && owns(closure)) release(closure, n.loc);
            if (lam_defaults.valid() && owns(lam_defaults)) release(lam_defaults, n.loc);
            if (lam_kwdefaults.valid() && owns(lam_kwdefaults)) release(lam_kwdefaults, n.loc);
            *ok = true;
            return fv;
        }
        std::vector<std::string> params;
        for (const arg& p : a.posonlyargs) params.push_back(p.arg);
        for (const arg& p : a.args) params.push_back(p.arg);
        for (const arg& p : a.kwonlyargs) params.push_back(p.arg);
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
        cur()->nposonly = (int)a.posonlyargs.size();
        cur()->nkwonly = nkwonly;
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
                                            lam_defaults, vararg_slot, kwarg_slot,
                                            lam_kwdefaults);
        // Guarded, NOT unconditional: the lambda path restores the owned set
        // differently from the def path, and re-marking here double-freed the
        // captured cell -- `return lambda x: x + n` crashed with no output.
        for (const ir::Value& c : closure_cells) if (owns(c)) release(c, n.loc);
        if (lam_defaults.valid() && owns(lam_defaults)) release(lam_defaults, n.loc);
        if (lam_kwdefaults.valid() && owns(lam_kwdefaults)) release(lam_kwdefaults, n.loc);
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

        // A name this comprehension reads is, by that fact alone, a CELL in
        // the enclosing function: nested_reads() treats a comprehension body
        // as nested, so `n` in `[n * i for i in ...]` gets a cell slot purely
        // because the comprehension mentions it. LoadLocal on that slot yields
        // the CELL, not the value -- so the hidden argument has to be marked
        // as a cell inside the synthetic function too, or the body reads the
        // box instead of its contents. It did, and the result was a P0: a cell
        // has no __bool__ and no __len__, so `[bool(flag) for ...]` answered
        // True for a False flag, at exit 0, with no diagnostic.
        //
        // The cell is passed THROUGH rather than dereferenced here, so a read
        // in the body goes through the same cell the enclosing scope holds.
        // Dereferencing at capture time would snapshot the value and diverge
        // if anything rebinds it while the comprehension runs (I2).
        std::vector<ir::Value> capvals;
        std::vector<std::string> cell_caps;
        for (const std::string& c : caps) {
            ir::Value v = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            auto it = locals_.find(c);
            emit(ir::Instr{ir::Op::LoadLocal, {}, v, Ownership::Owned, c,
                           it->second, 0, loc, make_landing_pad(loc)});
            mark_owned(v);
            capvals.push_back(v);
            if (cells_.count(c)) cell_caps.push_back(c);
        }

        std::vector<std::string> params{".0"};
        for (const std::string& c : caps) params.push_back(c);
        std::vector<std::string> locals = params;
        for (const std::string& x : vars) locals.push_back(x);
        locals.push_back(".acc");

        std::string fname = std::string("<") + kind + "comp>";
        FnScope sc = begin_function(fname, params, locals);
        // begin_function clears cells_; re-declare the captured cells so
        // lower_name emits cell.get for them rather than a raw load.
        for (const std::string& c : cell_caps) cells_[c] = locals_[c];
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
            // MOVE, not copy: the frame takes the only reference, so the
            // value is in exactly one ownership list and a landing pad decrefs
            // it exactly once (issue #9).
            mark_owned(it0); forget(it0);
            frame_owned_.push_back(it0);

            // Nest the generators: each one is a loop whose body is the
            // next, and the innermost body accumulates. Written recursively
            // because the shape IS recursive -- flattening it would need an
            // explicit stack of loop headers for no benefit.
            std::function<bool(std::size_t, const ir::Value&)> nest =
                [&](std::size_t gi, const ir::Value& iter_v) -> bool {
                    const comprehension& gg = gens[gi];
                    std::uint32_t head = new_block("comp.head");
                    (void)0;
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
                        mark_owned(subit); forget(subit);
                        frame_owned_.push_back(subit);
                        if (!nest(gi + 1, subit)) return false;
                        frame_owned_.pop_back();
                        emit_decref(subit, loc);
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
                emit_decref(it0, loc);   // frame held the only reference
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
        ir::Value kwdefaults;
        if (!a.kwonlyargs.empty()) {
            bool any = false;
            for (const auto& kd : a.kw_defaults) if (kd.has_value()) any = true;
            if (any) {
                kwdefaults = call_capi("PyDict_New", {}, n.loc, &dok);
                if (!dok) return false;
                mark_owned(kwdefaults);
                for (std::size_t i = 0; i < a.kw_defaults.size() && i < a.kwonlyargs.size(); ++i) {
                    if (!a.kw_defaults[i].has_value()) continue;
                    ir::Value d = lower_expr(*a.kw_defaults[i].value(), &dok);
                    if (!dok) return false;
                    ir::Value k = const_str(a.kwonlyargs[i].arg, n.loc);
                    call_capi("PyDict_SetItem", {kwdefaults, k, d}, n.loc, &dok, {k, d});
                    if (!dok) return false;
                }
            }
        }
        std::map<std::string, ir::Value> tp_cells;
        ir::Value tp_tuple;
        if (!n.type_params.empty()) {
            std::vector<std::pair<std::string, ir::Value>> bindings;
            if (!emit_type_params(n.type_params, n.loc, &tp_tuple, &bindings))
                return false;
            for (auto& [nm, tv] : bindings) {
                ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::CellNew, {}, cell, Ownership::Owned, nm,
                               0, 0, n.loc, make_landing_pad(n.loc)});
                mark_owned(cell);
                emit(ir::Instr{ir::Op::CellSet, {cell, tv}, std::nullopt,
                               Ownership::NotAnObject, nm, 0, 0, n.loc, std::nullopt});
                tp_cells[nm] = cell;
                if (owns(tv)) release(tv, n.loc);
            }
        }
        return lower_cpython_function(n.name, n.decorator_list, defaults, n.loc,
                                      kwdefaults, tp_cells, tp_tuple);
    }

    bool lower_cpython_function(const std::string& name,
                                const std::vector<expr>& decorators,
                                ir::Value defaults, const SourceLoc& loc,
                                ir::Value kwdefaults = {},
                                std::map<std::string, ir::Value> extra_cells = {},
                                ir::Value tp_tuple = {}) {
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
                ir::Value cell;
                if (auto eit = extra_cells.find(fv2); eit != extra_cells.end()) {
                    cell = eit->second;
                    emit(ir::Instr{ir::Op::IncRef, {cell}, std::nullopt,
                                   Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
                } else {
                    auto cit = cells_.find(fv2);
                    if (cit == cells_.end())
                        return err("function captures '" + fv2 +
                                   "', which has no closure cell here", "yield", loc);
                    cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                    emit(ir::Instr{ir::Op::LoadLocal, {}, cell, Ownership::Owned,
                                   fv2, cit->second, 0, loc, make_landing_pad(loc)});
                    mark_owned(cell);
                }
                call_capi_imm("PyTuple_SetItem", {closure, cell},
                              (std::int64_t)i, 1, loc, &cok);      // steals
                if (!cok) return false;
            }
        }
        ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::MakeGenFunc,
                       {closure.valid() ? closure : ir::Value{},
                        defaults.valid() ? defaults : ir::Value{},
                        kwdefaults.valid() ? kwdefaults : ir::Value{}},
                       fv, Ownership::Owned, gf->code, 0, 0, loc,
                       make_landing_pad(loc)});
        mark_owned(fv);
        if (closure.valid() && owns(closure)) release(closure, loc);
        if (defaults.valid() && owns(defaults)) release(defaults, loc);
        if (kwdefaults.valid() && owns(kwdefaults)) release(kwdefaults, loc);
        if (tp_tuple.valid()) {
            ir::Value key = const_str("__type_params__", loc);
            bool tok = true;
            call_capi("PyObject_SetAttr", {fv, key, tp_tuple}, loc, &tok, {key});
            if (!tok) return false;
            if (owns(tp_tuple)) release(tp_tuple, loc);
        }
        for (auto& [nm, cell] : extra_cells)
            if (owns(cell)) release(cell, loc);
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

        // Positional params first, then keyword-only. They are ordinary slots;
        // what makes them keyword-only is that the trampoline's positional
        // binding stops at nargs.
        // Order is load-bearing: positional-only first, then ordinary, then
        // keyword-only. The trampoline binds positionally from 0 and by
        // keyword from nposonly, so the `/` and `*` markers are expressed
        // entirely by where the counts fall in this one list.
        std::vector<std::string> params;
        for (const arg& p : a.posonlyargs) params.push_back(p.arg);
        for (const arg& p : a.args) params.push_back(p.arg);
        for (const arg& p : a.kwonlyargs) params.push_back(p.arg);
        const int nkwonly = (int)a.kwonlyargs.size();
        const int nposonly = (int)a.posonlyargs.size();

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

        // Keyword-only defaults are a DICT keyed by name, not a tuple:
        // kw_defaults is parallel to kwonlyargs with a hole where a parameter
        // is required, and a tuple cannot carry a hole. Evaluated here, once,
        // in the enclosing scope, exactly like the positional defaults.
        ir::Value kwdefaults;
        if (nkwonly > 0) {
            bool any = false;
            for (const auto& kd : a.kw_defaults) if (kd.has_value()) any = true;
            if (any) {
                kwdefaults = call_capi("PyDict_New", {}, n.loc, &dok);
                if (!dok) return false;
                mark_owned(kwdefaults);
                for (std::size_t i = 0; i < a.kw_defaults.size() && i < a.kwonlyargs.size(); ++i) {
                    if (!a.kw_defaults[i].has_value()) continue;   // required
                    ir::Value d = lower_expr(*a.kw_defaults[i].value(), &dok);
                    if (!dok) return false;
                    ir::Value k = const_str(a.kwonlyargs[i].arg, n.loc);
                    call_capi("PyDict_SetItem", {kwdefaults, k, d}, n.loc, &dok, {k, d});
                    if (!dok) return false;
                }
            }
        }

        // The qualname is fixed BEFORE the body is lowered, because lowering
        // the body pushes this function's own scope onto qual_.
        const std::string fn_qualname = qualname(n.name);

        std::map<std::string, ir::Value> tp_cells;
        ir::Value tp_tuple;
        if (!n.type_params.empty()) {
            std::vector<std::pair<std::string, ir::Value>> bindings;
            if (!emit_type_params(n.type_params, n.loc, &tp_tuple, &bindings))
                return false;
            for (auto& [nm, tv] : bindings) {
                ir::Value cell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::CellNew, {}, cell, Ownership::Owned, nm,
                               0, 0, n.loc, make_landing_pad(n.loc)});
                mark_owned(cell);
                emit(ir::Instr{ir::Op::CellSet, {cell, tv}, std::nullopt,
                               Ownership::NotAnObject, nm, 0, 0, n.loc, std::nullopt});
                tp_cells[nm] = cell;
                if (owns(tv)) release(tv, n.loc);
            }
        }

        // A def containing yield, and every async def, has its body compiled
        // by CPython and run by the interpreter (rebuild/GENERATORS.md).
        if (find_genexp(n.loc))
            return lower_cpython_function(n.name, n.decorator_list, defaults, n.loc,
                                          kwdefaults, tp_cells, tp_tuple);

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
        // Same reasoning, and it was missed: a `return` inside a def written
        // in a try/finally or a `with` body handed its value to the ENCLOSING
        // function's pending cleanup and branched to that cleanup's block --
        // "use of undefined value '%bbN'", and the module failed to assemble.
        // Pre-existing for try/finally, and reached far more code once `with`
        // gained a cleanup of its own, because unittest suites nest defs
        // inside `with` constantly.
        auto outer_fins = fin_stack_;
        auto outer_handled = handled_stack_;
        auto outer_fin_depth = fin_loop_depth_;
        // frame_owned_ must be saved too. A method lowered inside a class body
        // would otherwise inherit the class's namespace dict, and its landing
        // pads would emit a decref for a value defined in a DIFFERENT
        // function -- which LLVM rejects as "does not dominate all uses".
        auto outer_frame = frame_owned_;
        auto outer_class_ns = class_ns_;
        auto outer_class_cells = class_cells_;
        auto outer_cells = cells_;
        auto outer_enclosing = enclosing_cells_;
        auto outer_ints = int_locals_;
        auto outer_live = live_i64_;
        auto outer_force_boxed = force_boxed_ints_;
        auto outer_ranges = range_n_;
        auto outer_tp_env = type_param_env_;

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

        if (std::getenv("PYC_DUMP_INTLOCALS")) {
            std::set<std::string> ints = int_locals(slotnames, n.body, inner);
            std::fprintf(stderr, "intlocals %s:", n.name.c_str());
            for (const std::string& l : own_locals)
                std::fprintf(stderr, " %s%s", l.c_str(), ints.count(l) ? "*" : "");
            std::fprintf(stderr, "\n");
        }
        // A name is FREE when this function (or something nested in it) reads
        // it, it is not local here, and an enclosing function holds it in a
        // cell. Searching outward is what makes depth-3 nesting work.
        std::vector<std::string> freevars;
        {
            // Every name mentioned in this body, nested scopes included.
            //
            // This used to be `inner` plus stmt_names(), and stmt_names()
            // funnels through free_locals(), which reports only names present
            // in the ENCLOSING scope's locals_. That is right for a
            // comprehension's hidden arguments and wrong here, because
            // lower_classdef legitimately clears locals_ before lowering a
            // class body -- names in a class body are not fast locals. So a
            // method of a class defined inside a function saw an EMPTY set of
            // directly-read names, found no free variables, and fell through
            // to load.global. `output.append(...)` inside such a method raised
            // NameError while the class BODY reading the same local worked.
            //
            // all_reads() is over-approximate on purpose. The filters below
            // are what make it exact: a name that this function binds is its
            // own local, and a name with no cell in any enclosing scope is not
            // free. Over-reporting costs an unused closure cell; under-
            // reporting reads the wrong variable.
            std::set<std::string> reads = all_reads(n.body);
            reads.insert(inner.begin(), inner.end());
            // `global x` means module scope even if an enclosing function has
            // a cell named x. Capturing it would read a different variable.
            for (const std::string& g : declared_globals(n.body)) reads.erase(g);
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
        for (const auto& [nm, cell] : tp_cells) {
            bool have = false;
            for (const std::string& f2 : freevars) if (f2 == nm) { have = true; break; }
            if (!have) freevars.push_back(nm);
        }
        std::vector<ir::Value> closure_cells;
        for (const std::string& fv2 : freevars) {
            if (auto tit = tp_cells.find(fv2); tit != tp_cells.end()) {
                emit(ir::Instr{ir::Op::IncRef, {tit->second}, std::nullopt,
                               Ownership::NotAnObject, "", 0, 0, n.loc, std::nullopt});
                closure_cells.push_back(tit->second);
                continue;
            }
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

        mod_.functions.push_back(ir::Function{
            .name=n.name, .params=params, .nkwonly=nkwonly,
            .nposonly=nposonly, .next_value=1});
        fn_idx_ = mod_.functions.size() - 1;
        const std::size_t fn_index = fn_idx_;
        cur()->locals = all_locals;
        cur()->cellvars = cellvars;
        cur()->freevars = freevars;
        cur()->int_locals = int_locals(slotnames, n.body, inner);
        int_locals_ = cur()->int_locals;
        live_i64_.clear();
        int_bounds_.clear();
        loop_trips_.clear();
        force_boxed_ints_ = false;
        range_n_ = 0;
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
        fin_stack_.clear();
        handled_stack_.clear();
        fin_loop_depth_ = 0;
        frame_owned_.clear();
        class_ns_.clear();          // a method body is not a class body
        class_cells_.clear();       // nested defs LoadLocal __class__, not the class-body SSA
        type_param_env_.clear();
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
        fin_stack_ = outer_fins; handled_stack_ = outer_handled;
        fin_loop_depth_ = outer_fin_depth;
        frame_owned_ = outer_frame; class_ns_ = outer_class_ns;
        class_cells_ = outer_class_cells;
        cells_ = outer_cells; enclosing_cells_ = outer_enclosing;
        qual_ = outer_qual;
        int_locals_ = outer_ints;
        live_i64_ = outer_live;
        force_boxed_ints_ = outer_force_boxed;
        range_n_ = outer_ranges;
        type_param_env_ = outer_tp_env;
        if (!ok) return false;

        // Bind the callable in the enclosing scope, by the same store path any
        // other assignment uses -- a def is not special.
        ir::Value fv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        // Reference the function by INDEX, not name. Two classes can each
        // define `who`, and looking it up by name both picked the wrong one
        // and emitted a duplicate LLVM symbol.
        // target/target_else carry the *args and **kwargs slot indices,
        // biased by one so 0 can mean "absent".
        // args[0] defaults, args[1] kwdefaults, args[2..] the closure cells.
        std::vector<ir::Value> mkargs{defaults.valid() ? defaults : ir::Value{},
                                      kwdefaults.valid() ? kwdefaults : ir::Value{}};
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
        if (kwdefaults.valid() && owns(kwdefaults)) release(kwdefaults, n.loc);
        if (tp_tuple.valid()) {
            ir::Value key = const_str("__type_params__", n.loc);
            bool tok = true;
            call_capi("PyObject_SetAttr", {fv, key, tp_tuple}, n.loc, &tok, {key});
            if (!tok) return false;
            if (owns(tp_tuple)) release(tp_tuple, n.loc);
        }
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
                    auto lit = locals_.find(n2.id);
                    if (lit != locals_.end()) {
                        emit(ir::Instr{ir::Op::DelLocal, {}, std::nullopt,
                                       Ownership::NotAnObject, n2.id, lit->second,
                                       0, n.loc, make_landing_pad(n.loc)});
                        return;
                    }
                    ir::Value r = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
                    emit(ir::Instr{ir::Op::DelGlobal, {}, r, Ownership::NotAnObject,
                                   n2.id, 0, 0, n.loc, make_landing_pad(n.loc)});
                },
                [&](const Tuple& n2) {
                    Delete inner{n2.elts, n.loc};
                    ok = lower_delete(inner);
                },
                [&](const List& n2) {
                    Delete inner{n2.elts, n.loc};
                    ok = lower_delete(inner);
                },
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
    std::vector<std::map<std::string, ir::Value>> type_param_env_;

    using AnnItems = std::vector<std::pair<std::string, const expr*>>;

    // Every simple-name annotation belonging to THIS scope, in source order.
    // Descends into compound statements, which are the same scope, and stops
    // at a nested def or class, which are not. Measured: `if c: x: int = 5` at
    // module level does record x.
    static void collect_annotations(const std::vector<stmt>& body, AnnItems& out) {
        for (const stmt& s2 : body) {
            std::visit(ov{
                [&](const AnnAssign& a){
                    if (a.simple && std::holds_alternative<Name>(a.target->v))
                        out.emplace_back(std::get<Name>(a.target->v).id, &*a.annotation);
                },
                [&](const If& x){ collect_annotations(x.body, out);
                                  collect_annotations(x.orelse, out); },
                [&](const While& x){ collect_annotations(x.body, out);
                                     collect_annotations(x.orelse, out); },
                [&](const For& x){ collect_annotations(x.body, out);
                                   collect_annotations(x.orelse, out); },
                [&](const AsyncFor& x){ collect_annotations(x.body, out); },
                [&](const With& x){ collect_annotations(x.body, out); },
                [&](const AsyncWith& x){ collect_annotations(x.body, out); },
                [&](const Try& x){ collect_annotations(x.body, out);
                                   collect_annotations(x.orelse, out);
                                   collect_annotations(x.finalbody, out);
                                   for (const excepthandler& h : x.handlers)
                                       collect_annotations(
                                           std::get<ExceptHandler>(h.v).body, out); },
                [&](const TryStar& x){ collect_annotations(x.body, out); },
                [&](const Match& x){ for (const match_case& c : x.cases)
                                         collect_annotations(c.body, out); },
                // A nested scope keeps its own annotations, or discards them:
                // a function body's are recorded nowhere at all.
                [&](const auto&){},
            }, s2.v);
        }
    }
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

    bool lower_stmt_list(const std::vector<stmt>& body) {
        body_stk_.push_back(&body);
        stmt_idx_.push_back(0);
        for (std::size_t i = 0; i < body.size(); ++i) {
            stmt_idx_.back() = i;
            if (!lower_stmt(body[i])) {
                body_stk_.pop_back();
                stmt_idx_.pop_back();
                return false;
            }
        }
        body_stk_.pop_back();
        stmt_idx_.pop_back();
        return true;
    }

    void merge_live_i64(const std::map<std::string, ir::Value>& live_then,
                        std::uint32_t then_end,
                        const std::map<std::string, ir::Value>& live_else,
                        std::uint32_t else_end,
                        const SourceLoc& loc) {
        std::map<std::string, ir::Value> next;
        for (const auto& [name, tv] : live_then) {
            auto ev = live_else.find(name);
            if (ev == live_else.end()) continue;
            if (tv.id == ev->second.id) {
                next[name] = tv;
                continue;
            }
            ir::Value phi = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            ir::Instr in{ir::Op::Phi, {tv, ev->second}, phi, Ownership::NotAnObject,
                         "", 0, 0, loc, std::nullopt};
            in.phi_blocks = {then_end, else_end};
            cur()->blocks[blk_].instrs.insert(cur()->blocks[blk_].instrs.begin(),
                                              std::move(in));
            next[name] = phi;
        }
        live_i64_ = std::move(next);
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
        auto live_in = live_i64_;
        set_block(then_b);
        if (!lower_stmt_list(n.body)) return false;
        bool then_ok = !terminated();
        std::uint32_t then_end = (std::uint32_t)blk_;
        auto live_then = live_i64_;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join_b, 0, n.loc, std::nullopt});
        live_i64_ = live_in;
        set_block(else_b);
        if (!lower_stmt_list(n.orelse)) return false;
        bool else_ok = !terminated();
        std::uint32_t else_end = (std::uint32_t)blk_;
        auto live_else = live_i64_;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join_b, 0, n.loc, std::nullopt});
        set_block(join_b);
        if (then_ok && else_ok)
            merge_live_i64(live_then, then_end, live_else, else_end, n.loc);
        else if (then_ok) live_i64_ = std::move(live_then);
        else if (else_ok) live_i64_ = std::move(live_else);
        else live_i64_.clear();
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

    bool try_aug_int(const AugAssign& n) {
        if (force_boxed_ints_) return false;
        const Name* nm = n.target ? std::get_if<Name>(&n.target->v) : nullptr;
        if (!nm || !int_locals_.count(nm->id) || !class_ns_.empty()) return false;
        ir::Op iop;
        if (std::holds_alternative<Add>(n.op.v)) iop = ir::Op::IntAddOvf;
        else if (std::holds_alternative<Sub>(n.op.v)) iop = ir::Op::IntSubOvf;
        else if (std::holds_alternative<Mult>(n.op.v)) iop = ir::Op::IntMulOvf;
        else return false;
        if (!is_int_expr(*n.value)) return false;
        auto it = locals_.find(nm->id);
        if (it == locals_.end()) return false;
        std::uint32_t deopt = new_block("int.aug.deopt");
        ir::Value lhs, rhs, out;
        bool fast = emit_int_rvalue(*n.target, &lhs, deopt, n.loc)
                 && emit_int_rvalue(*n.value, &rhs, deopt, n.loc);
        if (!fast) {
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", deopt, 0, n.loc, std::nullopt});
        } else {
            out = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            std::uint32_t okb = new_block("int.aug.ok");
            ir::Instr in{iop, {lhs, rhs}, out, Ownership::NotAnObject,
                         "", 0, 0, n.loc, std::nullopt};
            in.target = okb;
            in.target_else = deopt;
            if (iop == ir::Op::IntAddOvf) {
                std::int64_t vlo, vhi;
                auto sb = int_bounds_.find(nm->id);
                if (sb != int_bounds_.end() && sb->second.first >= 0
                    && expr_i64_bounds(*n.value, &vlo, &vhi) && vlo >= 0
                    && !loop_trips_.empty()) {
                    bool tok = true;
                    std::int64_t T = 1;
                    for (const auto& t : loop_trips_) {
                        if (!t) { tok = false; break; }
                        if (__builtin_mul_overflow(T, *t, &T)) { tok = false; break; }
                    }
                    std::int64_t acc, tot;
                    if (tok && !__builtin_mul_overflow(T, vhi, &acc)
                        && !__builtin_add_overflow(sb->second.second, acc, &tot))
                        in.text = "nsw";
                }
            }
            emit(std::move(in));
            set_block(okb);
            emit(ir::Instr{ir::Op::IntStore, {out}, std::nullopt,
                           Ownership::NotAnObject, nm->id, it->second, 0, n.loc,
                           std::nullopt});
            if (loop_boxed_head()) {
                std::uint32_t fast_blk = (std::uint32_t)blk_;
                set_block(deopt);
                if (!redo_aug_boxed(n, iop, nm->id)) return false;
                if (!deopt_to_boxed_loop(n.loc)) return false;
                set_block(fast_blk);
                live_i64_[nm->id] = out;
                return true;
            }
        }
        std::uint32_t done = new_block("int.aug.done");
        if (fast) {
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", done, 0, n.loc, std::nullopt});
        }
        set_block(deopt);
        if (!redo_aug_boxed(n, iop, nm->id)) return false;
        if (deopt_to_boxed_loop(n.loc)) {
            set_block(done);
            if (fast && out.valid()) live_i64_[nm->id] = out;
            return true;
        }
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", done, 0, n.loc, std::nullopt});
        set_block(done);
        live_i64_.erase(nm->id);
        return true;
    }

    bool redo_aug_boxed(const AugAssign& n, ir::Op iop, const std::string& name) {
        bool ok = true;
        ir::Value cur_v = lower_expr(*n.target, &ok);
        ir::Value rhs_b = ok ? lower_expr(*n.value, &ok) : ir::Value{};
        const char* sym = iop == ir::Op::IntAddOvf ? "PyNumber_InPlaceAdd"
                        : iop == ir::Op::IntSubOvf ? "PyNumber_InPlaceSubtract"
                                                   : "PyNumber_InPlaceMultiply";
        ir::Value boxed;
        if (ok) boxed = call_capi(sym, {cur_v, rhs_b}, n.loc, &ok, {cur_v, rhs_b});
        if (!ok) return false;
        mark_owned(boxed);
        store_name(name, boxed, n.loc);
        return true;
    }

    // `x += y` is NOT `x = x + y`: it calls the in-place slot, so a list
    // extends in place and a tuple does not. Using the binary operator would
    // change observable behaviour for every mutable type.
    bool lower_augassign(const AugAssign& n) {
        if (try_aug_int(n)) return true;
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

        // Ownership differs by target kind, and getting it wrong here is a
        // DOUBLE FREE rather than a leak. call_capi does not register its
        // result as owned -- callers do -- but lower_expr already has, via
        // lower_name. Marking unconditionally therefore pushed the plain-name
        // case onto owned_ twice, and mark_owned is an unconditional
        // push_back, so the landing pad emitted `decref %27` twice.
        //
        // The effect: any augmented assignment on a BARE NAME whose operator
        // raises dropped one reference too many, freeing the object while the
        // binding still pointed at it. `m += 1` inside a plain try/except was
        // enough; every later use of `m` was a dangling pointer. Attribute and
        // subscript targets were never affected, which is what localises it.
        ir::Value cur_v;
        if (has_key) {
            cur_v = call_capi("PyObject_GetItem", {base, key}, n.loc, &ok);
            if (!ok) return false;
            mark_owned(cur_v);
        } else if (has_base) {
            ir::Value nm = const_str(std::get<Attribute>(n.target->v).attr, n.loc);
            cur_v = call_capi("PyObject_GetAttr", {base, nm}, n.loc, &ok, {nm});
            if (!ok) return false;
            mark_owned(cur_v);
        } else {
            cur_v = lower_expr(*n.target, &ok);            // already owned
            if (!ok) return false;
        }
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

    // `x: int = 5`, `x: int`, `obj.a: int = 5`, `d[k]: int = 5`.
    //
    // Two independent halves, and conflating them is the trap. The ASSIGNMENT
    // is ordinary and always happens when a value is present. The ANNOTATION
    // is never evaluated here -- PEP 649 defers it to an __annotate__ function
    // -- and is recorded only for a SIMPLE NAME target in a class or module
    // body. Measured against CPython 3.14.7:
    //
    //   x: int = 5     module   binds x, records the annotation
    //   y: str         module   records the annotation, binds NOTHING
    //   a: int = 3     function binds a; annotations are not recorded at all
    //   b: float       function does nothing whatsoever
    //   o.attr: U = 7  assigns; `U` is NOT evaluated even if undefined
    //   d["k"]: U = 9  assigns; same
    //
    // The "not evaluated" part is the whole point of the feature and the
    // reason eager evaluation was rejected: the blocked Lib/test files are
    // largely annotation-machinery tests, whose annotations are deliberately
    // unresolvable (`pipe: str | undefined`, `attriberr: obj.missing`).
    bool lower_annassign(const AnnAssign& n) {
        bool ok = true;
        if (n.value) {
            ir::Value v = lower_expr(**n.value, &ok);
            if (!ok) return false;
            if (!store_target(*n.target, v, n.loc)) return false;
        }
        // A simple name in a class or module body is the only case CPython
        // records. `simple` is the parser's own flag for "bare name, not
        // parenthesised"; `(x): int` is deliberately not recorded, by CPython
        // and here.
        // Nothing else happens here. The annotation is collected by a
        // pre-scan of the scope and evaluated only inside __annotate__.
        (void)n;
        return true;
    }

    // Build `__annotate__(format)` for the scope just lowered and bind it, so
    // CPython's __annotations__ descriptor finds it. Verified against 3.14.7:
    // a class made by type() with __annotate__ in its namespace, and a module
    // global of that name, both yield working __annotations__.
    //
    // `format` is accepted and ignored. Measured: CPython's own generated
    // annotate evaluates for format 1 (VALUE) and 2 (STRING) alike -- both
    // raise NameError on an undefined annotation -- so matching that is what
    // the evidence supports. If a test demands NotImplementedError for a
    // format, the differential harness will say so.
    bool emit_annotate(const AnnItems& items, const SourceLoc& loc) {
        if (items.empty()) return true;

        bool ok = true;
        // A class body's annotations may read names bound in that body
        // (`local = int` then `x: local`), so the namespace under construction
        // has to be reachable from inside. It arrives as a DEFAULT ARGUMENT
        // rather than a hidden parameter or a closure cell, because the caller
        // is CPython's own descriptor and it calls __annotate__(format) with
        // exactly one argument. The dict is fully populated by then.
        const bool in_class = !class_ns_.empty();
        ir::Value defaults;
        if (in_class) {
            ir::Value ns = class_ns_.back();
            defaults = call_capi_imm("PyTuple_New", {}, 1, 0, loc, &ok);
            if (!ok) return false;
            mark_owned(defaults);
            emit(ir::Instr{ir::Op::IncRef, {ns}, std::nullopt,
                           Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
            call_capi_imm("PyTuple_SetItem", {defaults, ns}, 0, 1, loc, &ok);
            if (!ok) return false;
        }

        std::vector<std::string> params{"format"};
        std::vector<std::string> locals = params;
        if (in_class) { params.push_back(".ns"); locals.push_back(".ns"); }

        FnScope sc = begin_function("__annotate__", params, locals);
        auto saved_ns = class_ns_;
        class_ns_.clear();
        if (in_class) {
            ir::Value ns = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::LoadLocal, {}, ns, Ownership::Owned, ".ns",
                           1, 0, loc, make_landing_pad(loc)});
            mark_owned(ns); forget(ns);
            frame_owned_.push_back(ns);
            class_ns_.push_back(ns);
        }
        ir::Value d = call_capi("PyDict_New", {}, loc, &ok);
        if (ok) {
            mark_owned(d); forget(d);
            frame_owned_.push_back(d);
            for (const auto& [name, ann] : items) {
                ir::Value v = lower_expr(*ann, &ok);
                if (!ok) break;
                ir::Value k = const_str(name, loc);
                call_capi("PyDict_SetItem", {d, k, v}, loc, &ok, {k, v});
                if (!ok) break;
            }
        }
        if (ok) {
            frame_owned_.pop_back();          // d, handed to the return
            if (in_class && !frame_owned_.empty()) {
                emit_decref(frame_owned_.back(), loc);
                frame_owned_.pop_back();
            }
            emit(ir::Instr{ir::Op::Return, {d}, std::nullopt,
                           Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
        }
        class_ns_ = saved_ns;
        std::size_t idx = end_function(sc);
        if (!ok) return false;

        ir::Value fn = make_function_value(idx, qualname("__annotate__"), loc,
                                           {}, defaults);
        store_name("__annotate__", fn, loc);
        if (owns(fn)) release(fn, loc);
        if (in_class && owns(defaults)) release(defaults, loc);
        return true;
    }

    bool lower_with(const With& n) { return lower_with_item(n, 0); }

    // `with a as x, b as y:` is exactly `with a as x:` wrapping
    // `with b as y:`, which is how CPython compiles it. Recursing on the item
    // index desugars it without synthesising AST nodes: item `idx` sets up its
    // own __enter__/__exit__, and its body is either the next item or, at the
    // last one, the real body. Each item therefore gets its own landing pad and
    // its own __exit__, which is what makes the second manager's cleanup run
    // when the first one's body raises.
    // __exit__ must run on EVERY exit path, including return, break and
    // continue. This used to REFUSE those -- but the refusal scanned only the
    // body's DIRECT children, and `If` was on the allowed list, so
    //
    //     with CM():
    //         if c:
    //             return "early"
    //
    // compiled, skipped __exit__, and exited 0 with the wrong output: a P0
    // silent wrong answer, in the guard whose stated purpose was to prevent
    // exactly that. A file handle stayed open, a lock stayed held.
    //
    // Deepening the scan would have closed the hole and kept refusing 20
    // Lib/test files. Instead `with` now carries a FinallyCtx, which is the
    // machinery try/finally already uses for the same problem: an early exit
    // hands its pending return value or jump marker to the nearest cleanup
    // rather than branching past it, the cleanup runs once, and then dispatches
    // on what was pending. Nesting falls out for free -- after this __exit__
    // runs, finish_return routes to the NEXT enclosing cleanup.
    bool lower_with_item(const With& n, std::size_t idx) {
        bool ok = true;
        const withitem& w = n.items[idx];
        ir::Value mgr = lower_expr(*w.context_expr, &ok);
        if (!ok) return false;
        // __exit__ is looked up BEFORE __enter__ runs, as CPython does: a
        // manager missing __exit__ must fail before any setup happens.
        ir::Value exitf = call_capi("pyc_rt_cm_exit", {mgr}, n.loc, &ok);
        if (!ok) return false;
        mark_owned(exitf); forget(exitf);
        frame_owned_.push_back(exitf);
        ir::Value entered = call_capi("pyc_rt_cm_enter", {mgr}, n.loc, &ok, {mgr});
        if (!ok) return false;
        mark_owned(entered);
        if (w.optional_vars) { if (!store_target(**w.optional_vars, entered, n.loc)) return false; }
        else if (owns(entered)) release(entered, n.loc);

        FinallyCtx fin;
        fin.entry = new_block("with.cleanup");
        std::uint32_t dispatch = new_block("with.unwind");
        std::uint32_t after    = new_block("with.after");
        auto live_in = live_i64_;
        try_stack_.push_back(dispatch);
        fin_stack_.push_back(&fin);
        // A break or continue targeting a loop INSIDE this body branches
        // normally; only one leaving the body has to run __exit__ first.
        std::size_t saved_fin_depth = fin_loop_depth_;
        fin_loop_depth_ = loops_.size();
        if (idx + 1 < n.items.size()) {
            if (!lower_with_item(n, idx + 1)) ok = false;
        } else {
            for (const stmt& s2 : n.body) if (!lower_stmt(s2)) { ok = false; break; }
        }
        fin_loop_depth_ = saved_fin_depth;
        fin_stack_.pop_back();
        try_stack_.pop_back();
        if (!ok) return false;

        // Normal completion is just another edge into the cleanup, carrying
        // nothing. Recorded only if control can actually fall out: a body
        // ending in `return` leaves the block terminated, and an edge from a
        // terminated block would be a duplicate phi predecessor.
        if (!terminated()) {
            fin.edge((std::uint32_t)blk_, const_null(n.loc), const_null(n.loc),
                     const_null(n.loc), const_null(n.loc));
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", fin.entry, 0, n.loc, std::nullopt});
        }

        set_block(fin.entry);
        if (fin.pred_blocks.empty()) {
            // Nothing reaches the cleanup -- the body always raised. The block
            // still needs a terminator to be well formed; it is unreachable,
            // so __exit__ must NOT run here. The exception path below is what
            // actually calls it.
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});
        } else {
        ir::Value p_ret = emit_phi(fin.pred_ret, fin.pred_blocks, n.loc);
        ir::Value p_brk, p_cont;
        if (fin.any_jump) {
            p_brk  = emit_phi(fin.pred_brk,  fin.pred_blocks, n.loc);
            p_cont = emit_phi(fin.pred_cont, fin.pred_blocks, n.loc);
            forget(p_brk); forget(p_cont);
        }
        // __exit__ can run arbitrary code and can raise, so a pending return
        // value has to be frame-owned across it or a landing pad leaks it.
        forget(p_ret);
        frame_owned_.push_back(p_ret);
        call_capi("pyc_rt_exit_normal", {exitf}, n.loc, &ok);
        frame_owned_.pop_back();
        if (!ok) return false;

        std::uint32_t ret_b   = new_block("with.return");
        std::uint32_t jumps_b = new_block("with.jumps");
        {
            ir::Value nn = const_null(n.loc);
            ir::Value isnull = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            emit(ir::Instr{ir::Op::Is, {p_ret, nn}, isnull, Ownership::NotAnObject,
                           "", 0, 0, n.loc, std::nullopt});
            emit(ir::Instr{ir::Op::CondBr, {isnull}, std::nullopt,
                           Ownership::NotAnObject, "", jumps_b, ret_b,
                           n.loc, std::nullopt});
        }

        // Each early exit releases exitf itself. `after` is left to do its own,
        // so every path through the region drops exactly one reference.
        set_block(ret_b);
        emit_decref(exitf, n.loc);
        mark_owned(p_ret);
        if (!finish_return(p_ret, n.loc)) return false;

        set_block(jumps_b);
        if (!fin.any_jump) {
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});
        } else {
            ir::Value nb = const_null(n.loc), nc = const_null(n.loc);
            ir::Value is_brk = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            emit(ir::Instr{ir::Op::Is, {p_brk, nb}, is_brk, Ownership::NotAnObject,
                           "", 0, 0, n.loc, std::nullopt});
            ir::Value is_cont = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            emit(ir::Instr{ir::Op::Is, {p_cont, nc}, is_cont, Ownership::NotAnObject,
                           "", 0, 0, n.loc, std::nullopt});
            // At most one marker is non-null and Py_DecRef is Py_XDECREF, so
            // releasing both unconditionally is right on every path.
            mark_owned(p_brk); mark_owned(p_cont);
            release(p_brk, n.loc); release(p_cont, n.loc);

            std::uint32_t brk_b   = new_block("with.break");
            std::uint32_t nobrk_b = new_block("with.nobreak");
            emit(ir::Instr{ir::Op::CondBr, {is_brk}, std::nullopt,
                           Ownership::NotAnObject, "", nobrk_b, brk_b,
                           n.loc, std::nullopt});
            set_block(brk_b);
            emit_decref(exitf, n.loc);
            reload_live_i64_from_slots(live_in, n.loc);
            if (!finish_jump(true, n.loc)) return false;

            set_block(nobrk_b);
            std::uint32_t cont_b = new_block("with.continue");
            emit(ir::Instr{ir::Op::CondBr, {is_cont}, std::nullopt,
                           Ownership::NotAnObject, "", after, cont_b,
                           n.loc, std::nullopt});
            set_block(cont_b);
            emit_decref(exitf, n.loc);
            reload_live_i64_from_slots(live_in, n.loc);
            if (!finish_jump(false, n.loc)) return false;
        }
        }

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
        emit_decref(exitf, n.loc);
        reload_live_i64_from_slots(live_in, n.loc);
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

    // A machine integer operand. §4's param_kinds marks these 'i', so codegen
    // widens them to the C parameter's width instead of passing a pointer.
    ir::Value int_const(std::int64_t v, const SourceLoc& loc) {
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        emit(ir::Instr{ir::Op::IntConst, {}, out, Ownership::NotAnObject,
                       std::to_string(v), 0, 0, loc, std::nullopt});
        return out;
    }

    ir::Value const_null(const SourceLoc& loc) {
        ir::Value v = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::ConstNull, {}, v, Ownership::AlwaysNull, "",
                       0, 0, loc, std::nullopt});
        return v;
    }

    bool lower_try(const Try& n) {
        if (n.finalbody.empty()) return lower_try_except(n, false);
        return lower_try_finally(n, false);
    }

    bool lower_trystar(const TryStar& n) {
        Try t{n.body, n.handlers, n.orelse, n.finalbody, n.loc};
        if (n.finalbody.empty()) return lower_try_except(t, true);
        return lower_try_finally(t, true);
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
    bool lower_try_finally(const Try& n, bool star) {
        FinallyCtx fin;
        fin.entry = new_block("finally.body");
        std::uint32_t catch_b = new_block("finally.catch");
        std::uint32_t after   = new_block("try.after");
        auto live_in = live_i64_;

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
            ok = lower_try_except(inner, star);
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
        reload_live_i64_from_slots(live_in, n.loc);
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
        reload_live_i64_from_slots(live_in, n.loc);
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
        {
            bool gok = true;
            call_capi("pyc_rt_gil_acquire", {}, loc, &gok);
            if (!gok) return false;
        }
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
        if (!is_break && loops_.back().boxed_head)
            add_i64_phi_incoming(loops_.back().head, (std::uint32_t)blk_);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject, "",
                       is_break ? loops_.back().done : loops_.back().head,
                       0, loc, std::nullopt});
        return true;
    }

    // Complete a return, honouring any enclosing finally: the value is handed
    // to the nearest pending cleanup rather than returned directly, so every
    // cleanup between here and the function boundary still runs.
    bool finish_return(const ir::Value& v, const SourceLoc& loc) {
        {
            bool gok = true;
            call_capi("pyc_rt_gil_acquire", {}, loc, &gok);
            if (!gok) return false;
        }
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

    bool lower_try_except(const Try& n, bool star) {
        if (n.handlers.empty()) return unsupported("try without except", n.loc);

        std::uint32_t dispatch = new_block("except.dispatch");
        std::uint32_t after    = new_block("try.after");
        auto live_in = live_i64_;

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
        mark_owned(exc); forget(exc);
        frame_owned_.push_back(exc);
        // Record what is being handled, so a bare `raise` inside the handler
        // has something to re-raise.
        ir::Value prev_handled = call_capi("pyc_rt_push_handled", {exc}, n.loc, &ok);
        if (!ok) return false;
        mark_owned(prev_handled); forget(prev_handled);
        frame_owned_.push_back(prev_handled);

        ir::Value remaining = exc;
        for (const excepthandler& h : n.handlers) {
            const ExceptHandler& eh = std::get<ExceptHandler>(h.v);
            std::uint32_t body_b = new_block("except.body");
            std::uint32_t next_b = new_block("except.next");
            ir::Value bound = exc;
            ir::Value rest;
            if (star) {
                if (!eh.type) return unsupported("bare except*", n.loc);
                ir::Value ty = lower_expr(**eh.type, &ok);
                if (!ok) return false;
                ir::Value pair = call_capi("pyc_rt_except_star_split",
                                           {remaining, ty}, n.loc, &ok, {ty});
                if (!ok) return false;
                mark_owned(pair);
                ir::Value i0 = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::ConstInt, {}, i0, Ownership::Owned, "0",
                               0, 0, n.loc, std::nullopt});
                mark_owned(i0);
                ir::Value i1 = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::ConstInt, {}, i1, Ownership::Owned, "1",
                               0, 0, n.loc, std::nullopt});
                mark_owned(i1);
                bound = call_capi("PyObject_GetItem", {pair, i0}, n.loc, &ok, {i0});
                if (!ok) return false;
                mark_owned(bound);
                rest = call_capi("PyObject_GetItem", {pair, i1}, n.loc, &ok, {i1});
                if (!ok) return false;
                mark_owned(rest);
                if (owns(pair)) release(pair, n.loc);
                // Pads after the try must not decref these: they are defined
                // only on the exception path. Manual decref on each arm.
                forget(bound); forget(rest);
                ir::Value nn = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::ConstNone, {}, nn, Ownership::Owned, "",
                               0, 0, n.loc, std::nullopt});
                ir::Value isnone = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
                emit(ir::Instr{ir::Op::Is, {bound, nn}, isnone, Ownership::NotAnObject,
                               "", 0, 0, n.loc, std::nullopt});
                std::uint32_t nomatch_b = new_block("exceptstar.nomatch");
                emit(ir::Instr{ir::Op::CondBr, {isnone}, std::nullopt,
                               Ownership::NotAnObject, "", nomatch_b, body_b,
                               n.loc, std::nullopt});
                set_block(nomatch_b);
                emit_decref(bound, n.loc);
                remaining = rest;
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", next_b, 0, n.loc, std::nullopt});
            } else if (eh.type) {
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
            reload_live_i64_from_slots(live_in, n.loc);
            if (eh.name) store_name(*eh.name, bound, n.loc);   // INCREFs; bound stays ours
            if (star) {
                emit_decref(bound, n.loc);
                mark_owned(rest);           // return from the handler releases it
            }
            // Open for the duration of the handler body, so a return, break or
            // continue out of it pops before leaving.
            handled_stack_.push_back(prev_handled);
            for (const stmt& s2 : eh.body)
                if (!lower_stmt(s2)) { handled_stack_.pop_back(); return false; }
            handled_stack_.pop_back();
            if (terminated()) { set_block(next_b); continue; }
            if (star) {
                forget(rest);
                remaining = rest;
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", next_b, 0, n.loc, std::nullopt});
            } else {
                call_capi("pyc_rt_pop_handled", {prev_handled}, n.loc, &ok);
                if (!ok) return false;
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", after, 0, n.loc, std::nullopt});
            }
            set_block(next_b);
        }

        // No handler matched (or except* leftover): put the exception back
        // and propagate. SetRaisedException STEALS.
        call_capi("pyc_rt_pop_handled", {prev_handled}, n.loc, &ok);
        if (!ok) return false;
        frame_owned_.pop_back();          // prev_handled
        emit_decref(prev_handled, n.loc);
        frame_owned_.pop_back();          // exc
        if (star) emit_decref(exc, n.loc);  // remaining is a split rest, not exc
        ir::Value reraised = star ? remaining : exc;
        if (star) {
            ir::Value nn = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, nn, Ownership::Owned, "",
                           0, 0, n.loc, std::nullopt});
            ir::Value isnone = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            emit(ir::Instr{ir::Op::Is, {remaining, nn}, isnone, Ownership::NotAnObject,
                           "", 0, 0, n.loc, std::nullopt});
            std::uint32_t reraise_b = new_block("exceptstar.reraise");
            std::uint32_t done_b = new_block("exceptstar.done");
            emit(ir::Instr{ir::Op::CondBr, {isnone}, std::nullopt,
                           Ownership::NotAnObject, "", done_b, reraise_b,
                           n.loc, std::nullopt});
            set_block(done_b);
            emit_decref(remaining, n.loc);
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});
            set_block(reraise_b);
        }
        call_capi("PyErr_SetRaisedException", {reraised}, n.loc, &ok);
        if (!ok) return false;
        forget(reraised);
        if (!star) forget(exc);
        std::uint32_t pad = make_landing_pad(n.loc);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", pad, 0, n.loc, std::nullopt});

        set_block(after);
        if (owns(prev_handled)) release(prev_handled, n.loc);
        reload_live_i64_from_slots(live_in, n.loc);
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


    // --- structural pattern matching (PEP 634) -----------------------------
    //
    // The binding model is the part that is easy to get wrong, so it is
    // stated here and tested directly. Captures are collected into SSA
    // TEMPORARIES while the pattern is tested, and only stored to their names
    // once the WHOLE pattern has matched. The guard runs after those stores.
    // Measured against CPython, that produces the observable asymmetry:
    //
    //   pattern fails            -> captures are NOT visible afterwards
    //   pattern matches, guard fails -> captures ARE visible afterwards
    //
    // Binding as each sub-pattern succeeds would get the first of those wrong.
    struct Capture { std::string name; ir::Value value; };

    // A capture takes its OWN reference the moment it is made, rather than
    // borrowing out of the helper tuple until lower_match binds it. That is
    // what lets a pattern release its `parts` as soon as the items are
    // extracted -- and it is the only formulation that works for or-patterns,
    // where an alternative's temporaries exist solely on its own path, so no
    // shared release list at the join could name them safely.
    void capture(std::vector<Capture>& caps, const std::string& name,
                 const ir::Value& v, const SourceLoc& loc) {
        emit(ir::Instr{ir::Op::IncRef, {v}, std::nullopt,
                       Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
        caps.push_back(Capture{name, v});
    }

    bool lower_match(const Match& n) {
        bool ok = true;
        ir::Value subject = lower_expr(*n.subject, &ok);
        if (!ok) return false;
        // The subject outlives every case test, so it is frame-owned rather
        // than a statement temporary. Promoted as a MOVE, and only when it is
        // actually owned: pushing an unowned value would have a landing pad
        // decref something this frame never owned, while the normal path (which
        // tested owns()) did not -- the two disagreed.
        const bool subj_owned = owns(subject);
        if (subj_owned) { forget(subject); frame_owned_.push_back(subject); }

        std::uint32_t after = new_block("match.after");
        auto live_in = live_i64_;
        for (const match_case& c : n.cases) {
            live_i64_ = live_in;
            std::uint32_t fail = new_block("match.next");
            std::vector<Capture> caps;
            if (!lower_pattern(*c.pattern, subject, fail, caps)) {
                if (subj_owned) frame_owned_.pop_back();
                return false;
            }
            // Pattern matched: NOW the names are bound.
            for (const Capture& cap : caps) {
                // The reference was taken in capture(); store_name consumes it.
                mark_owned(cap.value);
                store_name(cap.name, cap.value, n.loc);
            }
            std::uint32_t body_b = new_block("match.body");
            if (c.guard) {
                ir::Value g = lower_predicate(**c.guard, &ok);
                if (!ok) { if (subj_owned) frame_owned_.pop_back(); return false; }
                emit(ir::Instr{ir::Op::CondBr, {g}, std::nullopt,
                               Ownership::NotAnObject, "", body_b, fail,
                               n.loc, std::nullopt});
            } else {
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", body_b, 0, n.loc, std::nullopt});
            }
            set_block(body_b);
            for (const stmt& s2 : c.body)
                if (!lower_stmt(s2)) { if (subj_owned) frame_owned_.pop_back(); return false; }
            if (!terminated())
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", after, 0, n.loc, std::nullopt});
            set_block(fail);
        }
        // No case matched: match is not exhaustive, and that is not an error.
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, n.loc, std::nullopt});
        set_block(after);
        if (subj_owned) { frame_owned_.pop_back(); emit_decref(subject, n.loc); }
        live_i64_.clear();
        return true;
    }

    // The helpers return Py_None for "did not match", which is distinct from
    // an error: an error arrives as a null and goes to the landing pad. Shared
    // by the sequence, mapping and class patterns.
    void branch_if_no_match(const ir::Value& parts, std::uint32_t fail,
                            const char* label, const SourceLoc& loc) {
        ir::Value none = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::ConstNone, {}, none, Ownership::Owned, "",
                       0, 0, loc, std::nullopt});
        mark_owned(none);
        ir::Value isnone = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        emit(ir::Instr{ir::Op::Is, {parts, none}, isnone, Ownership::NotAnObject,
                       "", 0, 0, loc, std::nullopt});
        release(none, loc);
        std::uint32_t cont = new_block(label);
        emit(ir::Instr{ir::Op::CondBr, {isnone}, std::nullopt,
                       Ownership::NotAnObject, "", fail, cont, loc, std::nullopt});
        set_block(cont);
    }

    // Emit a test of `pat` against `subj`, branching to `fail` when it does
    // not match. Captures are appended, not stored.
    bool lower_pattern(const pattern& pat, const ir::Value& subj,
                       std::uint32_t fail, std::vector<Capture>& caps) {
        bool ok = true;
        std::visit(ov{
            [&](const MatchValue& p) {
                // Compared with ==, unlike a singleton pattern.
                ir::Value want = lower_expr(*p.value, &ok);
                if (!ok) return;
                ir::Value cmp = call_capi_imm("PyObject_RichCompare", {subj, want},
                                              2 /*Py_EQ*/, 2, p.loc, &ok, {want});
                if (!ok) return;
                mark_owned(cmp);
                ir::Value t = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
                emit(ir::Instr{ir::Op::IsTrue, {cmp}, t, Ownership::NotAnObject,
                               "PyObject_IsTrue", 0, 0, p.loc, make_landing_pad(p.loc)});
                release(cmp, p.loc);
                std::uint32_t cont = new_block("pat.cont");
                emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt,
                               Ownership::NotAnObject, "", cont, fail, p.loc, std::nullopt});
                set_block(cont);
            },
            [&](const MatchSingleton& p) {
                // None/True/False are compared by IDENTITY, not ==.
                // Only None, True and False can appear here.
                ir::Value want = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                ir::Instr mk{ir::Op::ConstNone, {}, want, Ownership::Owned, "",
                             0, 0, p.loc, std::nullopt};
                std::visit(ov{
                    [&](const ConstNone&)     { mk.op = ir::Op::ConstNone; },
                    [&](const ConstBool& v)   { mk.op = ir::Op::ConstBool;
                                                mk.text = v.value ? "True" : "False"; },
                    [&](const ConstBigInt&)   { ok = unsupported("this singleton pattern", p.loc); },
                    [&](const ConstFloat&)    { ok = unsupported("this singleton pattern", p.loc); },
                    [&](const ConstStr&)      { ok = unsupported("this singleton pattern", p.loc); },
                    [&](const ConstBytes&)    { ok = unsupported("this singleton pattern", p.loc); },
                    [&](const ConstComplex&)  { ok = unsupported("this singleton pattern", p.loc); },
                    [&](const ConstEllipsis&) { ok = unsupported("this singleton pattern", p.loc); },
                    [&](const ConstTuple&)    { ok = unsupported("this singleton pattern", p.loc); },
                    [&](const ConstFrozenSet&){ ok = unsupported("this singleton pattern", p.loc); },
                }, p.value.v);
                if (!ok) return;
                emit(std::move(mk));
                mark_owned(want);
                ir::Value t = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
                emit(ir::Instr{ir::Op::Is, {subj, want}, t, Ownership::NotAnObject,
                               "", 0, 0, p.loc, std::nullopt});
                if (owns(want)) release(want, p.loc);
                std::uint32_t cont = new_block("pat.cont");
                emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt,
                               Ownership::NotAnObject, "", cont, fail, p.loc, std::nullopt});
                set_block(cont);
            },
            [&](const MatchAs& p) {
                // `_` (no name, no pattern) matches anything and binds nothing.
                if (p.pattern && !lower_pattern(**p.pattern, subj, fail, caps)) {
                    ok = false;
                    return;
                }
                if (p.name) capture(caps, *p.name, subj, p.loc);
            },
            [&](const MatchOr& p) {
                // Alternatives must bind the same names -- CPython rejects
                // anything else at compile time, and pyc inherits that check
                // through the compile() validation in pyc_parse.
                std::uint32_t done = new_block("pat.or.done");
                std::vector<std::uint32_t> pred_blocks;
                std::vector<std::vector<Capture>> alt_caps;
                for (std::size_t i = 0; i < p.patterns.size(); ++i) {
                    bool last = (i + 1 == p.patterns.size());
                    std::uint32_t next = last ? fail : new_block("pat.or.next");
                    std::vector<Capture> mine;
                    if (!lower_pattern(p.patterns[i], subj, next, mine)) { ok = false; return; }
                    pred_blocks.push_back((std::uint32_t)blk_);
                    alt_caps.push_back(std::move(mine));
                    emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                                   "", done, 0, p.loc, std::nullopt});
                    if (!last) set_block(next);
                }
                set_block(done);
                // Each alternative binds the same names but with a different
                // value, so they meet in a phi -- one per captured name.
                if (!alt_caps.empty() && !alt_caps[0].empty()) {
                    for (std::size_t k = 0; k < alt_caps[0].size(); ++k) {
                        std::vector<ir::Value> vals;
                        for (const auto& ac : alt_caps) {
                            if (k >= ac.size()) { ok = false; return; }
                            vals.push_back(ac[k].value);
                        }
                        ir::Value merged = emit_phi(vals, pred_blocks, p.loc);
                        forget(merged);
                        caps.push_back(Capture{alt_caps[0][k].name, merged});
                    }
                }
            },
            [&](const MatchSequence& p) {
                // At most one star, and it splits the pattern into a fixed
                // prefix and a fixed suffix.
                std::size_t star = p.patterns.size();
                for (std::size_t i = 0; i < p.patterns.size(); ++i)
                    if (std::holds_alternative<MatchStar>(p.patterns[i].v)) {
                        if (star != p.patterns.size()) {
                            ok = err("more than one starred name in a sequence pattern",
                                     "sequence patterns", p.loc);
                            return;
                        }
                        star = i;
                    }
                const bool has_star = star != p.patterns.size();
                std::int64_t nbefore = (std::int64_t)(has_star ? star : p.patterns.size());
                std::int64_t nafter  = (std::int64_t)(has_star ? p.patterns.size() - star - 1 : 0);

                ir::Value parts = call_capi("pyc_rt_match_sequence",
                                            {subj, int_const(nbefore, p.loc),
                                             int_const(nafter, p.loc),
                                             int_const(has_star ? 1 : 0, p.loc)},
                                            p.loc, &ok);
                if (!ok) return;
                mark_owned(parts);
                // `parts` is a NEW reference. Every exit from here -- no match,
                // a sub-pattern failing, or success -- has to drop it, or it
                // leaks on the path taken (issue #5). Failures go through a
                // cleanup block rather than straight to `fail`.
                std::uint32_t drop = new_block("pat.seq.drop");
                branch_if_no_match(parts, drop, "pat.seq", p.loc);
                // The extracted values outlive the sub-pattern tests.
                forget(parts);
                frame_owned_.push_back(parts);
                std::int64_t k = 0;
                for (std::size_t i = 0; i < p.patterns.size(); ++i) {
                    ir::Value item = call_capi_imm("PyTuple_GetItem", {parts},
                                                   k++, 1, p.loc, &ok);
                    if (!ok) return;               // borrowed: not owned here
                    const pattern* sub = &p.patterns[i];
                    if (i == star) {
                        // The star binds the collected list itself.
                        const MatchStar& st = std::get<MatchStar>(sub->v);
                        if (st.name) capture(caps, *st.name, item, p.loc);
                        continue;
                    }
                    if (!lower_pattern(*sub, item, drop, caps)) { ok = false; return; }
                }
                frame_owned_.pop_back();
                // Success: the captures took their own references in
                // capture(), so nothing borrows out of `parts` any more.
                std::uint32_t seq_ok = new_block("pat.seq.ok");
                emit_decref(parts, p.loc);
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", seq_ok, 0, p.loc, std::nullopt});
                set_block(drop);
                emit_decref(parts, p.loc);
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", fail, 0, p.loc, std::nullopt});
                set_block(seq_ok);
            },
            [&](const MatchMapping& p)  {
                ir::Value keys = call_capi_imm("PyTuple_New", {},
                                               (std::int64_t)p.keys.size(), 0, p.loc, &ok);
                if (!ok) return;
                mark_owned(keys);
                for (std::size_t i = 0; i < p.keys.size(); ++i) {
                    ir::Value k = lower_expr(p.keys[i], &ok);
                    if (!ok) return;
                    call_capi_imm("PyTuple_SetItem", {keys, k},
                                  (std::int64_t)i, 1, p.loc, &ok);   // steals
                    if (!ok) return;
                }
                const bool want_rest = p.rest.has_value();
                ir::Value parts = call_capi("pyc_rt_match_mapping",
                                            {subj, keys, int_const(want_rest ? 1 : 0, p.loc)},
                                            p.loc, &ok, {keys});
                if (!ok) return;
                mark_owned(parts);
                std::uint32_t drop = new_block("pat.map.drop");
                branch_if_no_match(parts, drop, "pat.map", p.loc);
                forget(parts);
                frame_owned_.push_back(parts);
                for (std::size_t i = 0; i < p.patterns.size(); ++i) {
                    ir::Value item = call_capi_imm("PyTuple_GetItem", {parts},
                                                   (std::int64_t)i, 1, p.loc, &ok);
                    if (!ok) return;
                    if (!lower_pattern(p.patterns[i], item, drop, caps)) { ok = false; return; }
                }
                if (want_rest) {
                    ir::Value rest = call_capi_imm("PyTuple_GetItem", {parts},
                                                   (std::int64_t)p.patterns.size(), 1,
                                                   p.loc, &ok);
                    if (!ok) return;
                    capture(caps, *p.rest, rest, p.loc);
                }
                frame_owned_.pop_back();
                // Same contract as the sequence pattern: captures hold their
                // own references, so `parts` is released on success, and every
                // failure exit goes through `drop` (issue #5).
                std::uint32_t map_ok = new_block("pat.map.ok");
                emit_decref(parts, p.loc);
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", map_ok, 0, p.loc, std::nullopt});
                set_block(drop);
                emit_decref(parts, p.loc);
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", fail, 0, p.loc, std::nullopt});
                set_block(map_ok);
            },
            [&](const MatchClass& p)    {
                ir::Value cls = lower_expr(*p.cls, &ok);
                if (!ok) return;
                ir::Value kwnames;
                if (!p.kwd_attrs.empty()) {
                    kwnames = call_capi_imm("PyTuple_New", {},
                                            (std::int64_t)p.kwd_attrs.size(), 0, p.loc, &ok);
                    if (!ok) return;
                    mark_owned(kwnames);
                    for (std::size_t i = 0; i < p.kwd_attrs.size(); ++i) {
                        ir::Value nm = const_str(p.kwd_attrs[i], p.loc);
                        call_capi_imm("PyTuple_SetItem", {kwnames, nm},
                                      (std::int64_t)i, 1, p.loc, &ok);   // steals
                        if (!ok) return;
                    }
                }
                ir::Value parts = call_capi("pyc_rt_match_class",
                                            {subj, cls,
                                             int_const((std::int64_t)p.patterns.size(), p.loc),
                                             kwnames.valid() ? kwnames : ir::Value{}},
                                            p.loc, &ok,
                                            kwnames.valid()
                                                ? std::vector<ir::Value>{cls, kwnames}
                                                : std::vector<ir::Value>{cls});
                if (!ok) return;
                mark_owned(parts);
                std::uint32_t drop = new_block("pat.cls.drop");
                branch_if_no_match(parts, drop, "pat.cls", p.loc);
                forget(parts);
                frame_owned_.push_back(parts);
                for (std::size_t i = 0; i < p.patterns.size(); ++i) {
                    ir::Value item = call_capi_imm("PyTuple_GetItem", {parts},
                                                   (std::int64_t)i, 1, p.loc, &ok);
                    if (!ok) return;
                    if (!lower_pattern(p.patterns[i], item, drop, caps)) { ok = false; return; }
                }
                for (std::size_t i = 0; i < p.kwd_patterns.size(); ++i) {
                    ir::Value item = call_capi_imm("PyTuple_GetItem", {parts},
                                                   (std::int64_t)(p.patterns.size() + i), 1,
                                                   p.loc, &ok);
                    if (!ok) return;
                    if (!lower_pattern(p.kwd_patterns[i], item, drop, caps)) { ok = false; return; }
                }
                frame_owned_.pop_back();
                // Same contract as the sequence pattern: captures hold their
                // own references, so `parts` is released on success, and every
                // failure exit goes through `drop` instead of straight to
                // `fail` (issue #5).
                std::uint32_t cls_ok = new_block("pat.cls.ok");
                emit_decref(parts, p.loc);
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", cls_ok, 0, p.loc, std::nullopt});
                set_block(drop);
                emit_decref(parts, p.loc);
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", fail, 0, p.loc, std::nullopt});
                set_block(cls_ok);
            },
            [&](const MatchStar& p)     {
                // Only reachable inside a sequence pattern, which handles it.
                ok = err("starred pattern outside a sequence pattern",
                         "star patterns", p.loc);
            },
        }, pat.v);
        return ok;
    }

    bool lower_classdef(const ClassDef& n) {
        bool ok = true;
        // Bases first: they are ordinary expressions evaluated in the
        // ENCLOSING scope, before the body runs.
        ir::Value bases;
        bool star_base = false;
        for (const expr& b : n.bases)
            if (std::holds_alternative<Starred>(b.v)) { star_base = true; break; }
        if (star_base) {
            bases = lower_spliced(n.bases, "PyTuple", n.loc, &ok);
            if (!ok) return false;
        } else {
            bases = call_capi_imm("PyTuple_New", {},
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
        }

        // `class C(B, metaclass=M, **kw)`. The keywords are evaluated here, in
        // the enclosing scope and after the bases, which is the order CPython
        // uses. `metaclass=` is consumed by pyc_rt_class_meta and removed, so
        // it never reaches the metaclass call or __init_subclass__; everything
        // else is forwarded to both.
        ir::Value kwds;
        if (!n.keywords.empty()) {
            kwds = call_capi("PyDict_New", {}, n.loc, &ok);
            if (!ok) return false;
            mark_owned(kwds);
            for (const keyword& k : n.keywords) {
                ir::Value v = lower_expr(*k.value, &ok);
                if (!ok) return false;
                if (!k.arg) {
                    // `**kw` in a class statement merges into the same dict.
                    call_capi("PyDict_Update", {kwds, v}, n.loc, &ok, {v});
                } else {
                    ir::Value key = const_str(*k.arg, n.loc);
                    call_capi("PyDict_SetItem", {kwds, key, v}, n.loc, &ok, {key, v});
                }
                if (!ok) return false;
            }
        } else {
            kwds = const_null(n.loc);
        }

        ir::Value clsname = const_str(n.name, n.loc);
        ir::Value meta = call_capi("pyc_rt_class_meta", {bases, kwds}, n.loc, &ok);
        if (!ok) return false;
        mark_owned(meta); forget(meta);
        frame_owned_.push_back(meta);

        // The namespace comes from the METACLASS, not from PyDict_New. That is
        // load-bearing: enum.EnumMeta.__prepare__ returns an _EnumDict that
        // records member order and rejects duplicates, and building an enum in
        // a plain dict produces a class that is wrong rather than one that
        // fails.
        ir::Value ns = call_capi("pyc_rt_class_prepare",
                                 {meta, clsname, bases, kwds}, n.loc, &ok,
                                 {clsname});
        if (!ok) return false;
        mark_owned(ns);

        // The body is lowered INLINE in the enclosing function, with stores
        // redirected into ns. A class body is not a closure: it executes once,
        // immediately, which is why it needs no separate ir::Function.
        ir::Value ccell = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::CellNew, {}, ccell, Ownership::Owned, "__class__",
                       0, 0, n.loc, make_landing_pad(n.loc)});
        mark_owned(ccell); forget(ccell);
        frame_owned_.push_back(ccell);
        class_cells_.push_back(ccell);

        class_ns_.push_back(ns);
        const std::string qn_str = qualname(n.name);
        qual_.push_back(n.name);
        {
            ir::Value qn = const_str(qn_str, n.loc);
            store_name("__qualname__", qn, n.loc);
        }
        forget(ns); forget(bases);
        frame_owned_.push_back(ns);
        frame_owned_.push_back(bases);
        if (!n.keywords.empty()) { forget(kwds); frame_owned_.push_back(kwds); }
        auto saved_locals = locals_;
        locals_.clear();                  // names in a class body are not fast locals
        std::vector<std::pair<std::string, ir::Value>> class_tps;
        ir::Value class_tp_tuple;
        if (!n.type_params.empty()) {
            if (!emit_type_params(n.type_params, n.loc, &class_tp_tuple, &class_tps))
                return false;
            for (auto& [nm, tv] : class_tps)
                store_name_keep(nm, tv, n.loc);
        }
        // A class body is not a call, so C1a's function trampoline never runs.
        // Push a frame whose f_locals is the namespace; locals() then sees
        // `y` rather than the enclosing module. The capsule destructor pops
        // it, so a landing pad that decrefs the guard unwinds the frame.
        ir::Value guard = call_capi("pyc_rt_push_frame", {clsname, ns}, n.loc, &ok);
        if (!ok) { class_ns_.pop_back(); qual_.pop_back();
                   class_cells_.pop_back(); return false; }
        mark_owned(guard);
        forget(guard);
        std::uint32_t cls_unwind = new_block("class.unwind");
        std::uint32_t cls_after  = new_block("class.after");
        try_stack_.push_back(cls_unwind);
        // Emitted while class_ns_ names this body's namespace, which is what
        // __annotate__ captures, and before the body so the timing matches the
        // module case.
        AnnItems cann;
        collect_annotations(n.body, cann);
        if (!emit_annotate(cann, n.loc)) { try_stack_.pop_back(); class_ns_.pop_back();
                                           qual_.pop_back(); class_cells_.pop_back();
                                           return false; }
        for (const stmt& s2 : n.body)
            if (!lower_stmt(s2)) { try_stack_.pop_back(); class_ns_.pop_back();
                                   qual_.pop_back(); class_cells_.pop_back();
                                   return false; }
        try_stack_.pop_back();
        for (auto& [nm, tv] : class_tps) {
            ir::Value key = const_str(nm, n.loc);
            call_capi("pyc_rt_del_if_same", {ns, key, tv}, n.loc, &ok, {key});
            if (!ok) return false;
        }
        locals_ = saved_locals;
        class_ns_.pop_back();
        qual_.pop_back();
        class_cells_.pop_back();
        if (!terminated()) {
            emit_decref(guard, n.loc);
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", cls_after, 0, n.loc, std::nullopt});
        }
        set_block(cls_unwind);
        emit_decref(guard, n.loc);
        {
            std::uint32_t pad = make_landing_pad(n.loc);
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", pad, 0, n.loc, std::nullopt});
        }
        set_block(cls_after);
        // frame_owned_ is a stack and landing pads snapshot it, so every pop
        // must mirror its push in LIFO order. Pushed here, in order:
        // meta, ccell, ns, bases, and kwds when there are any.
        if (!n.keywords.empty()) frame_owned_.pop_back();   // kwds
        frame_owned_.pop_back();                            // bases
        frame_owned_.pop_back();                            // ns

        ir::Value cls = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::BuildClass, {bases, ns, meta, kwds}, cls,
                       Ownership::Owned, n.name, 0, 0, n.loc,
                       make_landing_pad(n.loc)});
        mark_owned(cls);
        if (!n.keywords.empty()) emit_decref(kwds, n.loc);
        emit_decref(bases, n.loc);
        emit_decref(ns, n.loc);
        // Fill __class__ BEFORE decorators run: CPython binds the cell to the
        // undecorated class, which is what super() in a method resolves to.
        emit(ir::Instr{ir::Op::CellSet, {ccell, cls}, std::nullopt,
                       Ownership::NotAnObject, "__class__", 0, 0, n.loc, std::nullopt});
        if (class_tp_tuple.valid()) {
            ir::Value key = const_str("__type_params__", n.loc);
            call_capi("PyObject_SetAttr", {cls, key, class_tp_tuple}, n.loc, &ok, {key});
            if (!ok) return false;
            if (owns(class_tp_tuple)) release(class_tp_tuple, n.loc);
            for (auto& [nm, tv] : class_tps)
                if (owns(tv)) release(tv, n.loc);
        }
        frame_owned_.pop_back();                            // ccell
        emit_decref(ccell, n.loc);
        frame_owned_.pop_back();                            // meta
        emit_decref(meta, n.loc);
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
        const std::int64_t level = n.level ? *n.level : 0;
        for (const alias& a : n.names)
            if (a.name == "*") {
                bool wok = true;
                std::string m2 = n.module ? *n.module : std::string();
                if (m2.empty() && level == 0)
                    return unsupported("wildcard import without a module", n.loc);
                ir::Value modname = const_str(m2, n.loc);
                ir::Value names = const_str("*", n.loc);
                ir::Value mod = call_capi("pyc_rt_import_from",
                    {modname, names, int_const(level, n.loc)}, n.loc, &wok,
                    {modname, names});
                if (!wok) return false;
                mark_owned(mod);
                call_capi("pyc_rt_import_star", {mod}, n.loc, &wok, {mod});
                return wok;
            }
        std::string mod = n.module ? *n.module : std::string();
        if (mod.empty() && level == 0)
            return unsupported("import from an unnamed module", n.loc);

        bool ok = true;
        // Import WITH a fromlist. `import X` then getattr is not the same
        // thing: the fromlist is what makes the import machinery load X.Y when
        // Y is a submodule and bind it on X. Without it `from test import
        // support` raised AttributeError, which is how 113 Lib/test files died.
        std::string csv;
        for (const alias& a : n.names) {
            if (!csv.empty()) csv += ",";
            csv += a.name;
        }
        ir::Value modname = const_str(mod, n.loc);
        ir::Value names = const_str(csv, n.loc);
        ir::Value m = call_capi("pyc_rt_import_from",
                                {modname, names, int_const(level, n.loc)},
                                n.loc, &ok, {modname, names});
        if (!ok) return false;
        mark_owned(m);
        for (const alias& a : n.names) {
            ir::Value key = const_str(a.name, n.loc);
            // getattr with CPython's sys.modules fallback, not a bare getattr.
            ir::Value v = call_capi("pyc_rt_import_attr", {m, key}, n.loc, &ok, {key});
            if (!ok) return false;
            mark_owned(v);
            store_name(a.asname ? *a.asname : a.name, v, n.loc);
        }
        if (owns(m)) release(m, n.loc);
        return true;
    }

    bool is_native_range_for(const For& n) const {
        if (force_boxed_ints_) return false;
        const Name* t = n.target ? std::get_if<Name>(&n.target->v) : nullptr;
        if (!t || !int_locals_.count(t->id) || !class_ns_.empty())
            return false;
        if (locals_.find(t->id) == locals_.end()) return false;
        const Call* c = n.iter ? std::get_if<Call>(&n.iter->v) : nullptr;
        if (!c || c->args.empty() || c->args.size() > 3 || !c->keywords.empty())
            return false;
        const Name* f = std::get_if<Name>(&c->func->v);
        if (!f || f->id != "range") return false;
        for (const expr& a : c->args)
            if (std::holds_alternative<Starred>(a.v)) return false;
        return true;
    }

    // Guarded native `for i in range(...)`. The analysis types the target on
    // syntax alone; whether `range` is the builtin is decided here at run
    // time. Rebuild/UNBOXING.md: unguarded lowering of this is a silent
    // wrong answer.
    bool lower_for_range(const For& n) {
        const Call& c = std::get<Call>(n.iter->v);
        bool ok = true;
        ir::Value fn = lower_expr(*c.func, &ok);
        if (!ok) return false;
        std::vector<ir::Value> args{fn};
        for (const expr& a : c.args) {
            ir::Value v = lower_expr(a, &ok);
            if (!ok) return false;
            args.push_back(v);
        }
        std::uint32_t rid = range_n_++;
        ir::Value g = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        ir::Instr rg{ir::Op::RangeGuard, args, g, Ownership::NotAnObject,
                     "", rid, 0, n.loc, std::nullopt};
        rg.has_imm = true;
        rg.imm = (std::int64_t)c.args.size();
        emit(std::move(rg));

        std::uint32_t native_e = new_block("range.native");
        std::uint32_t boxed_e = new_block("range.boxed");
        std::uint32_t join = new_block("range.join");
        emit(ir::Instr{ir::Op::CondBr, {g}, std::nullopt, Ownership::NotAnObject,
                       "", native_e, boxed_e, n.loc, std::nullopt});

        set_block(native_e);
        for (const ir::Value& a : args) if (owns(a)) emit_decref(a, n.loc);
        ir::Value it_n = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::ConstNull, {}, it_n, Ownership::AlwaysNull, "",
                       0, 0, n.loc, std::nullopt});
        ir::Value stop_n = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        ir::Instr rb{ir::Op::RangeBound, {}, stop_n, Ownership::NotAnObject,
                     "e", 0, 0, n.loc, std::nullopt};
        rb.has_imm = true;
        rb.imm = (std::int64_t)rid;
        emit(std::move(rb));
        ir::Value start_n = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        ir::Instr rbi{ir::Op::RangeBound, {}, start_n, Ownership::NotAnObject,
                      "i", 0, 0, n.loc, std::nullopt};
        rbi.has_imm = true;
        rbi.imm = (std::int64_t)rid;
        emit(std::move(rbi));
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join, 0, n.loc, std::nullopt});
        std::uint32_t native_end = (std::uint32_t)blk_;

        set_block(boxed_e);
        ir::Value seq = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        std::vector<ir::Value> call_args = args;
        emit(ir::Instr{ir::Op::CallObject, std::move(call_args), seq,
                       Ownership::Owned, "", 0, 0, n.loc, make_landing_pad(n.loc)});
        mark_owned(seq);
        for (const ir::Value& a : args) if (owns(a)) emit_decref(a, n.loc);
        ir::Value it_b = call_capi("PyObject_GetIter", {seq}, n.loc, &ok, {seq});
        if (!ok) return false;
        mark_owned(it_b);
        ir::Value stop_b = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        emit(ir::Instr{ir::Op::I64Const, {}, stop_b, Ownership::NotAnObject,
                       "0", 0, 0, n.loc, std::nullopt});
        ir::Value start_b = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        emit(ir::Instr{ir::Op::I64Const, {}, start_b, Ownership::NotAnObject,
                       "0", 0, 0, n.loc, std::nullopt});
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join, 0, n.loc, std::nullopt});
        std::uint32_t boxed_end = (std::uint32_t)blk_;

        set_block(join);
        for (const ir::Value& a : args) forget(a);
        forget(it_b);
        ir::Value it = emit_phi({it_n, it_b}, {native_end, boxed_end}, n.loc);
        forget(it);
        frame_owned_.push_back(it);
        ir::Value stop = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        {
            ir::Instr sp{ir::Op::Phi, {stop_n, stop_b}, stop, Ownership::NotAnObject,
                         "", 0, 0, n.loc, std::nullopt};
            sp.phi_blocks = {native_end, boxed_end};
            cur()->blocks[blk_].instrs.insert(cur()->blocks[blk_].instrs.begin(),
                                              std::move(sp));
        }
        ir::Value start = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        {
            ir::Instr sp{ir::Op::Phi, {start_n, start_b}, start, Ownership::NotAnObject,
                         "", 0, 0, n.loc, std::nullopt};
            sp.phi_blocks = {native_end, boxed_end};
            cur()->blocks[blk_].instrs.insert(cur()->blocks[blk_].instrs.begin(),
                                              std::move(sp));
        }

        std::uint32_t after = new_block("for.after");
        auto outer_live = live_i64_;
        if (!emit_for_range_loop(n, g, it, rid, after, false, stop, start))
            return false;
        set_block(after);
        rejoin_outer_live(std::move(outer_live), n.loc);
        return true;
    }

    bool emit_for_range_loop(const For& n, ir::Value g, ir::Value it,
                             std::uint32_t rid, std::uint32_t after, bool clone,
                             ir::Value stop, ir::Value start = {}) {
        const Name& tn = std::get<Name>(n.target->v);
        std::int64_t rlo = 0, rhi = 0, rtrip = 0;
        const Call& rc = std::get<Call>(n.iter->v);
        bool have_rb = const_range_bounds(rc, &rlo, &rhi, &rtrip);
        RangeBoundScope rbs{*this, tn.id, have_rb, rlo, rhi, rtrip};
        auto slot = locals_.find(tn.id);
        std::uint32_t boxed = 0;
        if (clone) {
            live_i64_.clear();
            frame_owned_.push_back(it);
        } else {
            boxed = prepare_i64_loop(n.body, n.loc, tn.id);
        }
        if (boxed && start.valid()) live_i64_[tn.id] = start;
        struct CtrScope {
            std::string& slot;
            std::string old;
            ~CtrScope() { slot = std::move(old); }
        } ctr_scope{range_ctr_, range_ctr_};
        if (boxed) range_ctr_ = tn.id;
        std::uint32_t pre = (std::uint32_t)blk_;
        std::uint32_t head = new_block("for.head");
        std::uint32_t nat_next = new_block("range.next");
        std::uint32_t box_next = new_block("range.iter");
        std::uint32_t body_n = new_block("range.body.n");
        std::uint32_t body_b = new_block("range.body.b");
        std::uint32_t body = new_block("for.body");
        std::uint32_t done = new_block("for.done");
        std::uint32_t brk = new_block("for.break");
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});

        set_block(head);
        if (boxed) begin_i64_loop(head, pre, brk, n.body, boxed, n.loc);
        else {
            live_i64_.clear();
            loops_.push_back({head, brk});
        }
        { bool pok = true; call_capi("pyc_rt_periodic", {}, n.loc, &pok);
          if (!pok) return false; }
        emit(ir::Instr{ir::Op::CondBr, {g}, std::nullopt, Ownership::NotAnObject,
                       "", nat_next, box_next, n.loc, std::nullopt});
        auto live_at_pred = live_i64_;

        set_block(nat_next);
        ir::Value iv = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
        ir::Value ctr;
        if (auto cit = live_i64_.find(tn.id); cit != live_i64_.end())
            ctr = cit->second;
        ir::Instr nxt{ir::Op::RangeNext, {stop, ctr}, iv, Ownership::NotAnObject,
                      "", body_n, done, n.loc, std::nullopt};
        nxt.has_imm = true;
        nxt.imm = (std::int64_t)rid;
        {
            const Call& rc = std::get<Call>(n.iter->v);
            if (rc.args.size() <= 2) nxt.text = "1";
        }
        emit(std::move(nxt));
        set_block(body_n);
        emit(ir::Instr{ir::Op::IntStore, {iv}, std::nullopt, Ownership::NotAnObject,
                       tn.id, slot->second, 0, n.loc, std::nullopt});
        auto live_n = live_i64_;
        if (boxed && int_locals_.count(tn.id)) live_n[tn.id] = iv;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", body, 0, n.loc, std::nullopt});
        std::uint32_t nat_end = (std::uint32_t)blk_;

        set_block(box_next);
        ir::Value item = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::IterNext, {it}, item, Ownership::Owned, "",
                       body_b, done, n.loc, make_landing_pad(n.loc)});
        set_block(body_b);
        mark_owned(item);
        if (!store_target(*n.target, item, n.loc)) return false;
        if (boxed && int_locals_.count(tn.id) && slot != locals_.end()) {
            ir::Value bv = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            std::uint32_t okb = new_block("range.i.load");
            std::uint32_t fail = new_block("range.i.fail");
            ir::Instr in{ir::Op::IntLoad, {}, bv, Ownership::NotAnObject,
                         tn.id, 0, 0, n.loc, make_landing_pad(n.loc)};
            in.has_imm = true;
            in.imm = slot->second;
            in.target = okb;
            in.target_else = fail;
            emit(std::move(in));
            set_block(fail);
            {
                bool saved_fb = force_boxed_ints_;
                auto saved_live = live_i64_;
                auto saved_fo = frame_owned_;
                auto saved_own = owned_;
                force_boxed_ints_ = true;
                live_i64_.clear();
                for (const stmt& s2 : n.body) if (!lower_stmt(s2)) return false;
                force_boxed_ints_ = saved_fb;
                if (!terminated())
                    emit(ir::Instr{ir::Op::Br, {}, std::nullopt,
                                   Ownership::NotAnObject, "", boxed, 0, n.loc,
                                   std::nullopt});
                live_i64_ = std::move(saved_live);
                frame_owned_ = std::move(saved_fo);
                owned_ = std::move(saved_own);
            }
            set_block(okb);
            live_i64_[tn.id] = bv;
        }
        auto live_b = live_i64_;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", body, 0, n.loc, std::nullopt});
        std::uint32_t box_end = (std::uint32_t)blk_;

        set_block(body);
        if (boxed) merge_live_i64(live_n, nat_end, live_b, box_end, n.loc);
        if (boxed) {
            for (std::size_t i = 0; i < n.body.size(); ++i) {
                stmt_idx_.back() = i;
                if (!lower_stmt(n.body[i])) return false;
            }
            finish_i64_loop_body(head);
        } else {
            for (const stmt& s2 : n.body) if (!lower_stmt(s2)) return false;
            loops_.pop_back();
        }
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});

        set_block(brk);
        emit_decref(it, n.loc);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, n.loc, std::nullopt});

        set_block(done);
        if (!frame_owned_.empty()) frame_owned_.pop_back();
        emit_decref(it, n.loc);
        live_i64_ = live_at_pred;
        for (const stmt& s2 : n.orelse) if (!lower_stmt(s2)) return false;
        if (!terminated())
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});

        if (clone) return true;
        set_block(after);
        return finish_boxed_clone(boxed, after, n.loc, [&]{
            std::uint32_t ca = new_block("for.clone.after");
            if (!emit_for_range_loop(n, g, it, rid, ca, true, stop, start))
                return false;
            set_block(ca);
            return true;
        });
    }

    bool lower_for_over_iter(ir::Value it, const For& n, std::uint32_t after) {
        const Name* tn = n.target ? std::get_if<Name>(&n.target->v) : nullptr;
        std::string skip = tn ? tn->id : std::string{};
        std::uint32_t boxed = prepare_i64_loop(n.body, n.loc, skip);
        std::uint32_t pre = (std::uint32_t)blk_;
        std::uint32_t head = new_block("for.head");
        std::uint32_t body = new_block("for.body");
        std::uint32_t done = new_block("for.done");
        std::uint32_t brk = new_block("for.break");
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});
        set_block(head);
        if (boxed) begin_i64_loop(head, pre, brk, n.body, boxed, n.loc);
        else {
            live_i64_.clear();
            loops_.push_back({head, brk});
        }
        { bool pok = true; call_capi("pyc_rt_periodic", {}, n.loc, &pok);
          if (!pok) return false; }
        ir::Value item = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::IterNext, {it}, item, Ownership::Owned, "",
                       body, done, n.loc, make_landing_pad(n.loc)});
        auto live_at_pred = live_i64_;

        set_block(body);
        mark_owned(item);
        if (!store_target(*n.target, item, n.loc)) return false;
        if (boxed) {
            for (std::size_t i = 0; i < n.body.size(); ++i) {
                stmt_idx_.back() = i;
                if (!lower_stmt(n.body[i])) return false;
            }
            finish_i64_loop_body(head);
        } else {
            for (const stmt& s2 : n.body) if (!lower_stmt(s2)) return false;
            loops_.pop_back();
        }
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});

        set_block(brk);
        emit_decref(it, n.loc);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, n.loc, std::nullopt});

        set_block(done);
        if (!frame_owned_.empty()) frame_owned_.pop_back();
        emit_decref(it, n.loc);
        live_i64_ = live_at_pred;
        for (const stmt& s2 : n.orelse) if (!lower_stmt(s2)) return false;
        if (!terminated())
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});
        if (boxed) {
            set_block(after);
            if (!finish_boxed_clone(boxed, after, n.loc, [&]{
                return lower_for_over_iter(it, n, after);
            })) return false;
        }
        return true;
    }

    bool lower_for(const For& n) {
        if (is_native_range_for(n)) return lower_for_range(n);

        bool ok = true;
        ir::Value seq = lower_expr(*n.iter, &ok);
        if (!ok) return false;
        // The ITERATOR PROTOCOL, not a type test. This is the single place
        // that decides how `for` traverses, so a user class defining
        // __iter__ works exactly as a list does -- the divergence the old
        // tree had between comprehensions and sum() is not expressible here.
        ir::Value it = call_capi("PyObject_GetIter", {seq}, n.loc, &ok, {seq});
        if (!ok) return false;
        mark_owned(it); forget(it);
        frame_owned_.push_back(it);
        // `else` runs only when the loop was NOT broken out of, so exhaustion
        // and break cannot share an exit. Both must still release the iterator
        // exactly once.
        std::uint32_t after = new_block("for.after");
        auto outer_live = live_i64_;
        if (!lower_for_over_iter(it, n, after)) return false;
        set_block(after);
        rejoin_outer_live(std::move(outer_live), n.loc);
        return true;
    }

    bool lower_while(const While& n) {
        auto outer_live = live_i64_;
        std::uint32_t boxed = prepare_i64_loop(n.body, n.loc, {},
                                              nested_reads_expr(*n.test));
        std::uint32_t pre = (std::uint32_t)blk_;
        std::uint32_t head = new_block("while.head");
        std::uint32_t body = new_block("while.body");
        std::uint32_t done = new_block("while.done");
        std::uint32_t brk   = new_block("while.break");
        std::uint32_t after = new_block("while.after");
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});
        set_block(head);
        begin_i64_loop(head, pre, brk, n.body, boxed, n.loc);
        bool ok = true;
        call_capi("pyc_rt_periodic", {}, n.loc, &ok);
        if (!ok) return false;
        ir::Value t = lower_predicate(*n.test, &ok);
        if (!ok) return false;
        emit(ir::Instr{ir::Op::CondBr, {t}, std::nullopt, Ownership::NotAnObject,
                       "", body, done, n.loc, std::nullopt});
        auto live_at_pred = live_i64_;
        set_block(body);
        bool gil_free = boxed && !stmts_have_nested_loop(n.body)
                     && stmts_gil_free(n.body);
        if (gil_free) {
            bool gok = true;
            call_capi("pyc_rt_gil_release", {}, n.loc, &gok);
            if (!gok) return false;
        }
        for (std::size_t i = 0; i < n.body.size(); ++i) {
            stmt_idx_.back() = i;
            if (!lower_stmt(n.body[i])) return false;
        }
        if (gil_free && !terminated()) {
            bool gok = true;
            call_capi("pyc_rt_gil_acquire", {}, n.loc, &gok);
            if (!gok) return false;
        }
        finish_i64_loop_body(head);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", head, 0, n.loc, std::nullopt});

        set_block(brk);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, n.loc, std::nullopt});

        set_block(done);
        live_i64_ = live_at_pred;
        for (const stmt& s2 : n.orelse) if (!lower_stmt(s2)) return false;
        if (!terminated())
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", after, 0, n.loc, std::nullopt});

        set_block(after);
        if (!finish_boxed_clone(boxed, after, n.loc, [&]{ return lower_while(n); }))
            return false;
        rejoin_outer_live(std::move(outer_live), n.loc);
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

    static bool parse_i64(const std::string& digits, std::int64_t* out) {
        errno = 0;
        char* end = nullptr;
        long long v = std::strtoll(digits.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end) return false;
        *out = (std::int64_t)v;
        return true;
    }

    bool const_i64_expr(const expr& e, std::int64_t* v) const {
        if (const Constant* c = std::get_if<Constant>(&e.v)) {
            const ConstBigInt* i = std::get_if<ConstBigInt>(&c->value.v);
            return i && parse_i64(i->digits, v);
        }
        if (const Name* n = std::get_if<Name>(&e.v)) {
            auto it = int_bounds_.find(n->id);
            if (it != int_bounds_.end() && it->second.first == it->second.second) {
                *v = it->second.first;
                return true;
            }
        }
        return false;
    }

    bool const_range_bounds(const Call& c, std::int64_t* lo, std::int64_t* hi,
                            std::int64_t* trip) const {
        if (c.args.empty() || c.args.size() > 2 || !c.keywords.empty())
            return false;
        std::int64_t start = 0, stop = 0;
        if (c.args.size() == 1) {
            if (!const_i64_expr(c.args[0], &stop)) return false;
        } else {
            if (!const_i64_expr(c.args[0], &start)
             || !const_i64_expr(c.args[1], &stop))
                return false;
        }
        if (start >= stop) return false;
        std::int64_t t;
        if (__builtin_sub_overflow(stop, start, &t)) return false;
        *lo = start;
        *hi = stop - 1;
        *trip = t;
        return true;
    }

    static bool combine_add(std::int64_t a0, std::int64_t a1, std::int64_t b0,
                            std::int64_t b1, std::int64_t* lo, std::int64_t* hi) {
        std::int64_t c[4];
        if (__builtin_add_overflow(a0, b0, &c[0])
         || __builtin_add_overflow(a0, b1, &c[1])
         || __builtin_add_overflow(a1, b0, &c[2])
         || __builtin_add_overflow(a1, b1, &c[3]))
            return false;
        *lo = *hi = c[0];
        for (int i = 1; i < 4; ++i) {
            if (c[i] < *lo) *lo = c[i];
            if (c[i] > *hi) *hi = c[i];
        }
        return true;
    }
    static bool combine_sub(std::int64_t a0, std::int64_t a1, std::int64_t b0,
                            std::int64_t b1, std::int64_t* lo, std::int64_t* hi) {
        std::int64_t c[4];
        if (__builtin_sub_overflow(a0, b0, &c[0])
         || __builtin_sub_overflow(a0, b1, &c[1])
         || __builtin_sub_overflow(a1, b0, &c[2])
         || __builtin_sub_overflow(a1, b1, &c[3]))
            return false;
        *lo = *hi = c[0];
        for (int i = 1; i < 4; ++i) {
            if (c[i] < *lo) *lo = c[i];
            if (c[i] > *hi) *hi = c[i];
        }
        return true;
    }
    static bool combine_mul(std::int64_t a0, std::int64_t a1, std::int64_t b0,
                            std::int64_t b1, std::int64_t* lo, std::int64_t* hi) {
        std::int64_t c[4];
        if (__builtin_mul_overflow(a0, b0, &c[0])
         || __builtin_mul_overflow(a0, b1, &c[1])
         || __builtin_mul_overflow(a1, b0, &c[2])
         || __builtin_mul_overflow(a1, b1, &c[3]))
            return false;
        *lo = *hi = c[0];
        for (int i = 1; i < 4; ++i) {
            if (c[i] < *lo) *lo = c[i];
            if (c[i] > *hi) *hi = c[i];
        }
        return true;
    }

    bool expr_i64_bounds(const expr& e, std::int64_t* lo, std::int64_t* hi) const {
        return std::visit(ov{
            [&](const Constant& c) -> bool {
                const ConstBigInt* i = std::get_if<ConstBigInt>(&c.value.v);
                std::int64_t v;
                if (!i || !parse_i64(i->digits, &v)) return false;
                *lo = *hi = v;
                return true;
            },
            [&](const Name& n) -> bool {
                auto it = int_bounds_.find(n.id);
                if (it == int_bounds_.end()) return false;
                *lo = it->second.first;
                *hi = it->second.second;
                return true;
            },
            [&](const BinOp& n) -> bool {
                std::int64_t l0, l1, r0, r1;
                if (!expr_i64_bounds(*n.left, &l0, &l1)
                 || !expr_i64_bounds(*n.right, &r0, &r1))
                    return false;
                if (std::holds_alternative<Add>(n.op.v))
                    return combine_add(l0, l1, r0, r1, lo, hi);
                if (std::holds_alternative<Sub>(n.op.v))
                    return combine_sub(l0, l1, r0, r1, lo, hi);
                if (std::holds_alternative<Mult>(n.op.v))
                    return combine_mul(l0, l1, r0, r1, lo, hi);
                return false;
            },
            [&](const UnaryOp& n) -> bool {
                if (std::holds_alternative<UAdd>(n.op.v))
                    return expr_i64_bounds(*n.operand, lo, hi);
                if (!std::holds_alternative<USub>(n.op.v)) return false;
                std::int64_t a, b, x, y;
                if (!expr_i64_bounds(*n.operand, &a, &b)) return false;
                if (__builtin_sub_overflow(0, b, &x)
                 || __builtin_sub_overflow(0, a, &y))
                    return false;
                *lo = x < y ? x : y;
                *hi = x > y ? x : y;
                return true;
            },
            [&](const auto&) -> bool { return false; },
        }, e.v);
    }

    bool result_fits_i64(const expr& e) const {
        std::int64_t lo, hi;
        return expr_i64_bounds(e, &lo, &hi);
    }

    bool is_int_expr(const expr& e) const {
        return std::visit(ov{
            [&](const Constant& c) {
                const ConstBigInt* i = std::get_if<ConstBigInt>(&c.value.v);
                std::int64_t tmp;
                return i && parse_i64(i->digits, &tmp);
            },
            [&](const Name& n) {
                return int_locals_.count(n.id) > 0 || live_i64_.count(n.id) > 0;
            },
            [&](const BinOp& n) {
                return (std::holds_alternative<Add>(n.op.v)
                     || std::holds_alternative<Sub>(n.op.v)
                     || std::holds_alternative<Mult>(n.op.v))
                    && is_int_expr(*n.left) && is_int_expr(*n.right);
            },
            [&](const UnaryOp& n) {
                return (std::holds_alternative<UAdd>(n.op.v)
                     || std::holds_alternative<USub>(n.op.v))
                    && is_int_expr(*n.operand);
            },
            [&](const auto&) { return false; },
        }, e.v);
    }

    bool stmts_have_def(const std::vector<stmt>& body) const {
        struct F : ast::WalkSink {
            bool found = false;
            void on(std::string_view k) override {
                if (k == "FunctionDef" || k == "AsyncFunctionDef" || k == "ClassDef")
                    found = true;
            }
        } f;
        for (const stmt& s : body) ast::walk(s, f);
        return f.found;
    }

    bool stmts_have_cfg_join(const std::vector<stmt>& body) const {
        for (const stmt& s : body) {
            if (std::holds_alternative<Match>(s.v)
             || std::holds_alternative<TryStar>(s.v)
             || std::holds_alternative<AsyncWith>(s.v))
                return true;
            if (const If* n = std::get_if<If>(&s.v)) {
                if (stmts_have_cfg_join(n->body) || stmts_have_cfg_join(n->orelse))
                    return true;
            }
            if (const Try* n = std::get_if<Try>(&s.v)) {
                if (stmts_have_cfg_join(n->body) || stmts_have_cfg_join(n->orelse)
                    || stmts_have_cfg_join(n->finalbody))
                    return true;
                for (const excepthandler& h : n->handlers) {
                    const ExceptHandler* eh = std::get_if<ExceptHandler>(&h.v);
                    if (eh && stmts_have_cfg_join(eh->body)) return true;
                }
            }
            if (const With* n = std::get_if<With>(&s.v)) {
                if (stmts_have_cfg_join(n->body)) return true;
            }
        }
        return false;
    }

    bool stmts_gil_free(const std::vector<stmt>& body) const {
        struct F : ast::WalkSink {
            bool bad = false;
            void on(std::string_view k) override {
                if (k == "Call" || k == "Attribute" || k == "Subscript"
                    || k == "Raise" || k == "Await" || k == "Yield"
                    || k == "YieldFrom" || k == "List" || k == "Dict"
                    || k == "Set" || k == "Tuple" || k == "ListComp"
                    || k == "DictComp" || k == "SetComp" || k == "GeneratorExp"
                    || k == "Lambda" || k == "JoinedStr" || k == "FormattedValue"
                    || k == "Starred" || k == "Try" || k == "TryStar"
                    || k == "With" || k == "AsyncWith" || k == "Match"
                    || k == "For" || k == "AsyncFor")
                    bad = true;
            }
        } f;
        for (const stmt& s : body) ast::walk(s, f);
        return !f.bad;
    }

    bool stmts_have_nested_loop(const std::vector<stmt>& body) const {
        for (const stmt& s : body) {
            if (std::holds_alternative<For>(s.v)
             || std::holds_alternative<AsyncFor>(s.v)
             || std::holds_alternative<While>(s.v))
                return true;
            if (const If* n = std::get_if<If>(&s.v)) {
                if (stmts_have_nested_loop(n->body) || stmts_have_nested_loop(n->orelse))
                    return true;
            }
            if (const Try* n = std::get_if<Try>(&s.v)) {
                if (stmts_have_nested_loop(n->body) || stmts_have_nested_loop(n->orelse)
                    || stmts_have_nested_loop(n->finalbody))
                    return true;
                for (const excepthandler& h : n->handlers) {
                    const ExceptHandler* eh = std::get_if<ExceptHandler>(&h.v);
                    if (eh && stmts_have_nested_loop(eh->body)) return true;
                }
            }
            if (const With* n = std::get_if<With>(&s.v)) {
                if (stmts_have_nested_loop(n->body)) return true;
            }
        }
        return false;
    }

    bool nested_loops_phiable(const std::vector<stmt>& body) const {
        for (const stmt& s : body) {
            if (const For* n = std::get_if<For>(&s.v)) {
                if (!can_i64_phis(n->body) || !nested_loops_phiable(n->orelse))
                    return false;
            } else if (const While* n = std::get_if<While>(&s.v)) {
                if (!can_i64_phis(n->body) || !nested_loops_phiable(n->orelse))
                    return false;
            } else if (std::holds_alternative<AsyncFor>(s.v)) {
                return false;
            } else if (const If* n = std::get_if<If>(&s.v)) {
                if (!nested_loops_phiable(n->body) || !nested_loops_phiable(n->orelse))
                    return false;
            } else if (const Try* n = std::get_if<Try>(&s.v)) {
                if (!nested_loops_phiable(n->body) || !nested_loops_phiable(n->orelse)
                    || !nested_loops_phiable(n->finalbody))
                    return false;
                for (const excepthandler& h : n->handlers) {
                    const ExceptHandler* eh = std::get_if<ExceptHandler>(&h.v);
                    if (eh && !nested_loops_phiable(eh->body)) return false;
                }
            } else if (const With* n = std::get_if<With>(&s.v)) {
                if (!nested_loops_phiable(n->body)) return false;
            }
        }
        return true;
    }

    bool can_i64_phis(const std::vector<stmt>& body) const {
        return !force_boxed_ints_ && !int_locals_.empty()
            && !stmts_have_def(body) && !stmts_have_cfg_join(body)
            && nested_loops_phiable(body);
    }

    void rejoin_outer_live(std::map<std::string, ir::Value> outer,
                           const SourceLoc& loc) {
        auto inner = live_i64_;
        std::uint32_t deopt = loop_boxed_head();
        if (inner.empty()) {
            live_i64_ = std::move(outer);
            return;
        }
        if (!deopt) {
            // Loop-head phis do not dominate the boxed clone's exit, which
            // joins here. Drop them; a later use IntLoads from the slot.
            live_i64_.clear();
            return;
        }
        live_i64_ = std::move(outer);
        for (const auto& [name, _] : inner) {
            if (!int_locals_.count(name) || cells_.count(name)) continue;
            auto it = locals_.find(name);
            if (it == locals_.end()) continue;
            ir::Value iv = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            std::uint32_t okb = new_block("int.rejoin");
            ir::Instr in{ir::Op::IntLoad, {}, iv, Ownership::NotAnObject,
                         name, 0, 0, loc, make_landing_pad(loc)};
            in.has_imm = true;
            in.imm = it->second;
            in.target = okb;
            in.target_else = deopt;
            emit(std::move(in));
            set_block(okb);
            live_i64_[name] = iv;
        }
    }

    void reload_live_i64_from_slots(const std::map<std::string, ir::Value>& names,
                                    const SourceLoc& loc) {
        std::uint32_t deopt = loop_boxed_head();
        live_i64_.clear();
        if (!deopt) return;
        for (const auto& [name, _] : names) {
            if (!int_locals_.count(name) || cells_.count(name)) continue;
            auto it = locals_.find(name);
            if (it == locals_.end()) continue;
            ir::Value iv = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            std::uint32_t okb = new_block("int.reload");
            ir::Instr in{ir::Op::IntLoad, {}, iv, Ownership::NotAnObject,
                         name, 0, 0, loc, make_landing_pad(loc)};
            in.has_imm = true;
            in.imm = it->second;
            in.target = okb;
            in.target_else = deopt;
            emit(std::move(in));
            set_block(okb);
            live_i64_[name] = iv;
        }
    }

    std::uint32_t loop_boxed_head() const {
        return loops_.empty() ? 0 : loops_.back().boxed_head;
    }

    bool is_param(const std::string& name) const {
        for (const std::string& p : cur()->params) if (p == name) return true;
        return false;
    }

    std::uint32_t prepare_i64_loop(const std::vector<stmt>& body,
                                   const SourceLoc& loc,
                                   const std::string& skip = {},
                                   std::set<std::string> extra = {}) {
        if (!can_i64_phis(body)) {
            live_i64_.clear();
            return 0;
        }
        std::uint32_t boxed = new_block("loop.boxed");
        std::set<std::string> names = all_reads(body);
        std::set<std::string> writes = all_writes(body);
        for (const std::string& name : names) {
            if (!int_locals_.count(name) || live_i64_.count(name)) continue;
            if (!skip.empty() && name == skip) continue;
            if (writes.count(name)) continue;
            auto it = locals_.find(name);
            if (it == locals_.end() || cells_.count(name)) continue;
            ir::Value iv = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            std::uint32_t okb = new_block("int.preload");
            ir::Instr in{ir::Op::IntLoad, {}, iv, Ownership::NotAnObject,
                         name, 0, 0, loc, make_landing_pad(loc)};
            in.has_imm = true;
            in.imm = it->second;
            in.target = okb;
            in.target_else = boxed;
            emit(std::move(in));
            set_block(okb);
            live_i64_[name] = iv;
        }
        names.insert(extra.begin(), extra.end());
        for (const std::string& name : names) {
            if (live_i64_.count(name) || int_locals_.count(name)) continue;
            if (!skip.empty() && name == skip) continue;
            auto it = locals_.find(name);
            if (it == locals_.end() || cells_.count(name)) continue;
            if (!is_param(name) || writes.count(name)) continue;
            ir::Value obj = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::LoadLocal, {}, obj, Ownership::Owned, name,
                           it->second, 0, loc, make_landing_pad(loc)});
            mark_owned(obj);
            ir::Value iv = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            std::uint32_t okb = new_block("int.unbox");
            std::uint32_t fail = new_block("int.unbox.fail");
            emit(ir::Instr{ir::Op::IntUnbox, {obj}, iv, Ownership::NotAnObject,
                           name, okb, fail, loc, std::nullopt});
            set_block(fail);
            emit_decref(obj, loc);
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", boxed, 0, loc, std::nullopt});
            set_block(okb);
            release(obj, loc);
            live_i64_[name] = iv;
        }
        return boxed;
    }

    void begin_i64_loop(std::uint32_t head, std::uint32_t pre, std::uint32_t brk,
                        const std::vector<stmt>& body, std::uint32_t boxed,
                        const SourceLoc& loc) {
        if (boxed) open_i64_phis(head, pre, loc);
        loops_.push_back({head, brk, boxed});
        body_stk_.push_back(&body);
        stmt_idx_.push_back(0);
    }

    void finish_i64_loop_body(std::uint32_t head) {
        body_stk_.pop_back();
        stmt_idx_.pop_back();
        std::uint32_t boxed = loops_.empty() ? 0 : loops_.back().boxed_head;
        if (!loops_.empty()) loops_.pop_back();
        if (!boxed) return;
        if (!terminated()) close_i64_phis(head);
        else {
            while (!i64_phis_.empty() && i64_phis_.back().head == head)
                i64_phis_.pop_back();
        }
    }

    void open_i64_phis(std::uint32_t head, std::uint32_t pre, const SourceLoc& loc) {
        std::map<std::string, ir::Value> next;
        std::size_t idx = 0;
        for (const auto& [name, val] : live_i64_) {
            ir::Value phi = cur()->fresh(ir::Type{ir::Type::Kind::Int64, {}});
            ir::Instr in{ir::Op::Phi, {val}, phi, Ownership::NotAnObject, "",
                         0, 0, loc, std::nullopt};
            in.phi_blocks = {pre};
            cur()->blocks[head].instrs.insert(
                cur()->blocks[head].instrs.begin() + (long)idx, std::move(in));
            i64_phis_.push_back(I64Phi{head, name, idx, phi});
            next[name] = phi;
            ++idx;
        }
        live_i64_ = std::move(next);
    }

    void add_i64_phi_incoming(std::uint32_t head, std::uint32_t from) {
        if (!range_ctr_.empty()) {
            auto lv = live_i64_.find(range_ctr_);
            if (lv != live_i64_.end()) {
                ir::Type ty{ir::Type::Kind::Int64, {}};
                ir::Value one = cur()->fresh(ty);
                emit(ir::Instr{ir::Op::I64Const, {}, one, Ownership::NotAnObject,
                               "1", 0, 0, {}, std::nullopt});
                ir::Value nxt = cur()->fresh(ty);
                std::uint32_t okb = new_block("range.inc");
                ir::Instr ad{ir::Op::IntAddOvf, {lv->second, one}, nxt,
                             Ownership::NotAnObject, "nsw", 0, 0, {},
                             std::nullopt};
                ad.target = okb;
                ad.target_else = okb;
                emit(std::move(ad));
                set_block(okb);
                live_i64_[range_ctr_] = nxt;
                from = okb;
            }
        }
        for (auto it = i64_phis_.rbegin(); it != i64_phis_.rend(); ++it) {
            if (it->head != head) break;
            ir::Value inc = it->phi;
            auto lv = live_i64_.find(it->name);
            if (lv != live_i64_.end()) inc = lv->second;
            ir::Instr& phi = cur()->blocks[head].instrs[it->instr_idx];
            phi.args.push_back(inc);
            phi.phi_blocks.push_back(from);
        }
    }

    void close_i64_phis(std::uint32_t head) {
        add_i64_phi_incoming(head, (std::uint32_t)blk_);
        while (!i64_phis_.empty() && i64_phis_.back().head == head)
            i64_phis_.pop_back();
    }

    bool deopt_to_boxed_loop(const SourceLoc& loc) {
        std::uint32_t bh = loop_boxed_head();
        if (!bh) return false;
        {
            bool gok = true;
            call_capi("pyc_rt_gil_acquire", {}, loc, &gok);
            if (!gok) return false;
        }
        bool saved_fb = force_boxed_ints_;
        auto saved_live = live_i64_;
        force_boxed_ints_ = true;
        live_i64_.clear();
        for (std::size_t fi = body_stk_.size(); fi > 0; --fi) {
            const auto& body = *body_stk_[fi - 1];
            for (std::size_t k = stmt_idx_[fi - 1] + 1; k < body.size(); ++k)
                if (!lower_stmt(body[k])) {
                    force_boxed_ints_ = saved_fb;
                    live_i64_ = saved_live;
                    return false;
                }
        }
        force_boxed_ints_ = saved_fb;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", bh, 0, loc, std::nullopt});
        live_i64_ = std::move(saved_live);
        return true;
    }

    bool finish_boxed_clone(std::uint32_t boxed, std::uint32_t after,
                            const SourceLoc& loc, const auto& lower_boxed) {
        if (!boxed) return true;
        set_block(boxed);
        force_boxed_ints_ = true;
        auto saved = live_i64_;
        auto saved_fo = frame_owned_;
        auto saved_own = owned_;
        live_i64_.clear();
        if (!lower_boxed()) return false;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", after, 0, loc, std::nullopt});
        force_boxed_ints_ = false;
        live_i64_ = std::move(saved);
        frame_owned_ = std::move(saved_fo);
        owned_ = std::move(saved_own);
        set_block(after);
        return true;
    }

    // Emit an i64 rvalue. On deopt (overflow, boxed slot) branch to deopt.
    // Returns false if the shape is not unboxable (caller uses boxed path).
    bool emit_int_rvalue(const expr& e, ir::Value* out, std::uint32_t deopt,
                         const SourceLoc& loc) {
        ir::Type ty{ir::Type::Kind::Int64, {}};
        return std::visit(ov{
            [&](const Constant& c) -> bool {
                const ConstBigInt* i = std::get_if<ConstBigInt>(&c.value.v);
                std::int64_t v;
                if (!i || !parse_i64(i->digits, &v)) return false;
                *out = cur()->fresh(ty);
                emit(ir::Instr{ir::Op::I64Const, {}, *out, Ownership::NotAnObject,
                               i->digits, 0, 0, loc, std::nullopt});
                return true;
            },
            [&](const Name& n) -> bool {
                auto it = locals_.find(n.id);
                if (it == locals_.end()) return false;
                auto lv = live_i64_.find(n.id);
                if (lv != live_i64_.end()) { *out = lv->second; return true; }
                if (!int_locals_.count(n.id)) return false;
                *out = cur()->fresh(ty);
                std::uint32_t okb = new_block("int.load.ok");
                ir::Instr in{ir::Op::IntLoad, {}, *out, Ownership::NotAnObject,
                             n.id, 0, 0, loc, make_landing_pad(loc)};
                in.has_imm = true;
                in.imm = it->second;
                in.target = okb;
                in.target_else = deopt;
                emit(std::move(in));
                set_block(okb);
                return true;
            },
            [&](const BinOp& n) -> bool {
                ir::Op op;
                if (std::holds_alternative<Add>(n.op.v)) op = ir::Op::IntAddOvf;
                else if (std::holds_alternative<Sub>(n.op.v)) op = ir::Op::IntSubOvf;
                else if (std::holds_alternative<Mult>(n.op.v)) op = ir::Op::IntMulOvf;
                else return false;
                auto as_i64 = [&](const expr& e, std::int64_t* v) {
                    const Constant* c = std::get_if<Constant>(&e.v);
                    if (!c) return false;
                    const ConstBigInt* i = std::get_if<ConstBigInt>(&c->value.v);
                    return i && parse_i64(i->digits, v);
                };
                std::int64_t lv, rv, ovv;
                if (as_i64(*n.left, &lv) && as_i64(*n.right, &rv)) {
                    bool overflow = false;
                    if (op == ir::Op::IntAddOvf)
                        overflow = __builtin_add_overflow(lv, rv, &ovv);
                    else if (op == ir::Op::IntSubOvf)
                        overflow = __builtin_sub_overflow(lv, rv, &ovv);
                    else
                        overflow = __builtin_mul_overflow(lv, rv, &ovv);
                    if (!overflow) {
                        *out = cur()->fresh(ty);
                        emit(ir::Instr{ir::Op::I64Const, {}, *out, Ownership::NotAnObject,
                                       std::to_string(ovv), 0, 0, loc, std::nullopt});
                        return true;
                    }
                }
                ir::Value l, r;
                if (!emit_int_rvalue(*n.left, &l, deopt, loc)) return false;
                if (!emit_int_rvalue(*n.right, &r, deopt, loc)) return false;
                *out = cur()->fresh(ty);
                std::uint32_t okb = new_block("int.op.ok");
                ir::Instr in{op, {l, r}, *out, Ownership::NotAnObject,
                             "", 0, 0, loc, std::nullopt};
                in.target = okb;
                in.target_else = deopt;
                std::int64_t blo, bhi;
                if (expr_i64_bounds(e, &blo, &bhi)) in.text = "nsw";
                emit(std::move(in));
                set_block(okb);
                return true;
            },
            [&](const UnaryOp& n) -> bool {
                if (std::holds_alternative<UAdd>(n.op.v))
                    return emit_int_rvalue(*n.operand, out, deopt, loc);
                if (!std::holds_alternative<USub>(n.op.v)) return false;
                ir::Value x;
                if (!emit_int_rvalue(*n.operand, &x, deopt, loc)) return false;
                *out = cur()->fresh(ty);
                std::uint32_t okb = new_block("int.neg.ok");
                ir::Instr in{ir::Op::IntNegOvf, {x}, *out, Ownership::NotAnObject,
                             "", 0, 0, loc, std::nullopt};
                in.target = okb;
                in.target_else = deopt;
                emit(std::move(in));
                set_block(okb);
                return true;
            },
            [&](const auto&) -> bool { return false; },
        }, e.v);
    }

    bool try_store_int(const std::string& name, const expr& value,
                       const SourceLoc& loc) {
        if (force_boxed_ints_) return false;
        if (!int_locals_.count(name) || !class_ns_.empty()) return false;
        if (!is_int_expr(value)) return false;
        auto it = locals_.find(name);
        if (it == locals_.end()) return false;
        std::uint32_t deopt = new_block("int.deopt");
        ir::Value iv;
        if (!emit_int_rvalue(value, &iv, deopt, loc)) {
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", deopt, 0, loc, std::nullopt});
        } else {
            emit(ir::Instr{ir::Op::IntStore, {iv}, std::nullopt, Ownership::NotAnObject,
                           name, it->second, 0, loc, std::nullopt});
            if (loop_boxed_head()) {
                std::uint32_t fast_blk = (std::uint32_t)blk_;
                set_block(deopt);
                if (!redo_store_boxed(name, value, it->second, loc)) return false;
                if (!deopt_to_boxed_loop(loc)) return false;
                set_block(fast_blk);
                live_i64_[name] = iv;
                {
                    std::int64_t blo, bhi;
                    if (expr_i64_bounds(value, &blo, &bhi))
                        int_bounds_[name] = {blo, bhi};
                    else
                        int_bounds_.erase(name);
                }
                return true;
            }
            live_i64_[name] = iv;
            {
                std::int64_t blo, bhi;
                if (expr_i64_bounds(value, &blo, &bhi))
                    int_bounds_[name] = {blo, bhi};
                else
                    int_bounds_.erase(name);
            }
        }
        std::uint32_t done = new_block("int.done");
        if (iv.valid()) {
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", done, 0, loc, std::nullopt});
        }
        set_block(deopt);
        if (!redo_store_boxed(name, value, it->second, loc)) return false;
        if (deopt_to_boxed_loop(loc)) {
            set_block(done);
            return true;
        }
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", done, 0, loc, std::nullopt});
        set_block(done);
        live_i64_.erase(name);
        return true;
    }

    bool redo_store_boxed(const std::string& name, const expr& value,
                          std::uint32_t slot, const SourceLoc& loc) {
        bool ok = true;
        ir::Value boxed = lower_expr(value, &ok);
        if (!ok) return false;
        emit(ir::Instr{ir::Op::StoreLocal, {boxed}, std::nullopt,
                       Ownership::NotAnObject, name, slot, 0, loc, std::nullopt});
        if (owns(boxed)) release(boxed, loc);
        return true;
    }

    bool lower_assign(const Assign& a) {
        if (a.targets.size() == 1) {
            if (const Name* nm = std::get_if<Name>(&a.targets[0].v)) {
                if (try_store_int(nm->id, *a.value, a.loc)) return true;
            }
        }
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
            [&](const TemplateStr& n)   { out = lower_template(n, ok); },
            [&](const Interpolation& n) { out = lower_interpolation(n, ok); },
            [&](const Attribute& n)     { out = lower_attribute(n, ok); },
            [&](const Subscript& n)     { out = lower_subscript(n, ok); },
            [&](const Starred& n)       { *ok = unsupported("star-unpacking", n.loc); },
            [&](const List& n)          { out = lower_sequence(n.elts, "PyList", n.loc, ok); },
            [&](const Tuple& n)         { out = lower_sequence(n.elts, "PyTuple", n.loc, ok); },
            [&](const Slice& n)         { out = lower_slice(n, ok); },
        }, e.v);
        return out;
    }

    ir::Value lower_const_tuple(const std::vector<ConstantValue>& items,
                                const SourceLoc& loc, bool* ok) {
        ir::Value seq = call_capi_imm("PyTuple_New", {},
                                      (std::int64_t)items.size(), 0, loc, ok);
        if (!*ok) return {};
        mark_owned(seq);
        for (std::size_t i = 0; i < items.size(); ++i) {
            ir::Value v = lower_const_value(items[i], loc, ok);
            if (!*ok) return {};
            call_capi_imm("PyTuple_SetItem", {seq, v}, (std::int64_t)i, 1, loc, ok);
            if (!*ok) return {};
        }
        return seq;
    }

    ir::Value lower_const_value(const ConstantValue& cv, const SourceLoc& loc,
                                bool* ok) {
        if (const ConstTuple* t = std::get_if<ConstTuple>(&cv.v))
            return lower_const_tuple(t->items, loc, ok);
        if (const ConstFrozenSet* f = std::get_if<ConstFrozenSet>(&cv.v)) {
            ir::Value tup = lower_const_tuple(f->items, loc, ok);
            if (!*ok) return {};
            ir::Value out = call_capi("PyFrozenSet_New", {tup}, loc, ok, {tup});
            if (*ok) mark_owned(out);
            return out;
        }
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        ir::Instr in{ir::Op::ConstNone, {}, out, Ownership::Owned, "", 0, 0,
                     loc, std::nullopt};
        std::visit(ov{
            [&](const ConstBigInt& v) { in.op = ir::Op::ConstInt;   in.text = v.digits; },
            [&](const ConstFloat& v)  {
                in.op = ir::Op::ConstFloat;
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.17g", v.value);
                in.text = buf;
            },
            [&](const ConstStr& v)    { in.op = ir::Op::ConstStr;   in.text = v.value; },
            [&](const ConstBytes& v)  { in.op = ir::Op::ConstBytes; in.text = v.value; },
            [&](const ConstBool& v)   { in.op = ir::Op::ConstBool;  in.text = v.value ? "True" : "False"; },
            [&](const ConstNone&)     { in.op = ir::Op::ConstNone; },
            [&](const ConstEllipsis&) { in.op = ir::Op::ConstEllipsis; },
            [&](const ConstComplex& v) {
                in.op = ir::Op::ConstComplex;
                char buf[80];
                std::snprintf(buf, sizeof buf, "%.17g %.17g", v.real, v.imag);
                in.text = buf;
            },
            [&](const ConstTuple&) {},
            [&](const ConstFrozenSet&) {},
        }, cv.v);
        emit(std::move(in));
        mark_owned(out);
        return out;
    }

    ir::Value lower_const(const Constant& c, bool* ok) {
        return lower_const_value(c.value, c.loc, ok);
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
            for (auto eit = type_param_env_.rbegin(); eit != type_param_env_.rend(); ++eit) {
                auto fit = eit->find(n.id);
                if (fit == eit->end()) continue;
                out = call_capi("pyc_rt_newref", {fit->second}, n.loc, ok);
                if (*ok) mark_owned(out);
                return out;
            }
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
        ir::Value key;
        if (const Starred* st = std::get_if<Starred>(&n.slice->v)) {
            ir::Value v = lower_expr(*st->value, ok);
            if (!*ok) return {};
            key = call_capi("PySequence_Tuple", {v}, n.loc, ok, {v});
            if (*ok) mark_owned(key);
        } else {
            key = lower_expr(*n.slice, ok);
        }
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

    ir::Value emit_bool_phi(const std::vector<ir::Value>& vals,
                            const std::vector<std::uint32_t>& blocks,
                            const SourceLoc& loc) {
        ir::Value out = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
        ir::Instr in{ir::Op::Phi, vals, out, Ownership::NotAnObject, "",
                     0, 0, loc, std::nullopt};
        in.phi_blocks = blocks;
        cur()->blocks[blk_].instrs.insert(cur()->blocks[blk_].instrs.begin(),
                                          std::move(in));
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

    int rich_opid(const cmpop& op) const {
        int id = -1;
        std::visit(ov{
            [&](const Lt&)    { id = 0; },
            [&](const LtE&)   { id = 1; },
            [&](const Eq&)    { id = 2; },
            [&](const NotEq&) { id = 3; },
            [&](const Gt&)    { id = 4; },
            [&](const GtE&)   { id = 5; },
            [&](const Is&)    {},
            [&](const IsNot&) {},
            [&](const In&)    {},
            [&](const NotIn&) {},
        }, op.v);
        return id;
    }

    bool can_int_compare(const Compare& n) const {
        if (!class_ns_.empty()) return false;
        if (n.ops.size() != n.comparators.size() || n.ops.empty()) return false;
        if (!is_int_expr(*n.left)) return false;
        for (std::size_t i = 0; i < n.ops.size(); ++i) {
            if (rich_opid(n.ops[i]) < 0) return false;
            if (!is_int_expr(n.comparators[i])) return false;
        }
        return true;
    }

    bool emit_int_cmp_i32(const Compare& n, ir::Value* out, std::uint32_t deopt,
                          const SourceLoc& loc) {
        ir::Value left;
        if (!emit_int_rvalue(*n.left, &left, deopt, loc)) return false;
        if (n.ops.size() == 1) {
            ir::Value right;
            if (!emit_int_rvalue(n.comparators[0], &right, deopt, loc)) return false;
            *out = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
            ir::Instr in{ir::Op::IntCmp, {left, right}, *out, Ownership::NotAnObject,
                         "", 0, 0, loc, std::nullopt};
            in.has_imm = true;
            in.imm = rich_opid(n.ops[0]);
            emit(std::move(in));
            return true;
        }
        std::uint32_t false_b = new_block("icmp.false");
        std::uint32_t join = new_block("icmp.join");
        ir::Value last;
        std::uint32_t last_end = 0;
        for (std::size_t i = 0; i < n.ops.size(); ++i) {
            ir::Value right;
            if (!emit_int_rvalue(n.comparators[i], &right, deopt, loc)) return false;
            ir::Value c = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
            ir::Instr in{ir::Op::IntCmp, {left, right}, c, Ownership::NotAnObject,
                         "", 0, 0, loc, std::nullopt};
            in.has_imm = true;
            in.imm = rich_opid(n.ops[i]);
            emit(std::move(in));
            if (i + 1 == n.ops.size()) {
                last = c;
                last_end = (std::uint32_t)blk_;
                emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                               "", join, 0, loc, std::nullopt});
            } else {
                std::uint32_t next = new_block("icmp.next");
                emit(ir::Instr{ir::Op::CondBr, {c}, std::nullopt, Ownership::NotAnObject,
                               "", next, false_b, loc, std::nullopt});
                set_block(next);
                left = right;
            }
        }
        set_block(false_b);
        ir::Value z = cur()->fresh(ir::Type{ir::Type::Kind::Bool, {}});
        emit(ir::Instr{ir::Op::IntConst, {}, z, Ownership::NotAnObject,
                       "0", 0, 0, loc, std::nullopt});
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", join, 0, loc, std::nullopt});
        std::uint32_t false_end = (std::uint32_t)blk_;
        set_block(join);
        *out = emit_bool_phi({last, z}, {last_end, false_end}, loc);
        return true;
    }

    bool try_int_predicate(const expr& e, ir::Value* out, bool* ok) {
        if (force_boxed_ints_) return false;
        const Compare* n = std::get_if<Compare>(&e.v);
        if (!n || !can_int_compare(*n)) return false;
        std::uint32_t deopt = new_block("icmp.deopt");
        ir::Value fast;
        if (!emit_int_cmp_i32(*n, &fast, deopt, n->loc)) {
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", deopt, 0, n->loc, std::nullopt});
            set_block(deopt);
            return false;
        }
        std::uint32_t done = new_block("icmp.done");
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", done, 0, n->loc, std::nullopt});
        std::uint32_t fast_end = (std::uint32_t)blk_;
        set_block(deopt);
        ir::Value boxed = lower_predicate_boxed(e, ok);
        if (!*ok) return false;
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", done, 0, n->loc, std::nullopt});
        std::uint32_t deopt_end = (std::uint32_t)blk_;
        set_block(done);
        *out = emit_bool_phi({fast, boxed}, {fast_end, deopt_end}, n->loc);
        return true;
    }

    ir::Value lower_compare_boxed(const Compare& n, bool* ok) {
        if (n.ops.size() > 1) return lower_chained_compare(n, ok);
        ir::Value l = lower_expr(*n.left, ok);        if (!*ok) return {};
        ir::Value r = lower_expr(n.comparators[0], ok); if (!*ok) return {};
        return compare_one(l, r, n.ops[0], n.loc, ok);
    }

    bool try_int_compare_box(const Compare& n, ir::Value* out, bool* ok) {
        if (force_boxed_ints_ || !can_int_compare(n)) return false;
        std::uint32_t deopt = new_block("icmp.box.deopt");
        ir::Value pred;
        if (!emit_int_cmp_i32(n, &pred, deopt, n.loc)) {
            emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                           "", deopt, 0, n.loc, std::nullopt});
            set_block(deopt);
            return false;
        }
        std::uint32_t done = new_block("icmp.box.done");
        ir::Value fast_b = box_bool(pred, n.loc, ok);
        if (!*ok) return false;
        // Leave owned_ before the deopt arm is lowered: a pad built there
        // that decrefs fast_b would not dominate (the ifexp scar).
        forget(fast_b);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", done, 0, n.loc, std::nullopt});
        std::uint32_t fast_end = (std::uint32_t)blk_;
        set_block(deopt);
        ir::Value boxed = lower_compare_boxed(n, ok);
        if (!*ok) return false;
        forget(boxed);
        emit(ir::Instr{ir::Op::Br, {}, std::nullopt, Ownership::NotAnObject,
                       "", done, 0, n.loc, std::nullopt});
        std::uint32_t deopt_end = (std::uint32_t)blk_;
        set_block(done);
        *out = emit_phi({fast_b, boxed}, {fast_end, deopt_end}, n.loc);
        return true;
    }

    ir::Value lower_compare(const Compare& n, bool* ok) {
        if (n.ops.size() != n.comparators.size() || n.ops.empty()) {
            *ok = err("malformed comparison", "Compare", n.loc);
            return {};
        }
        ir::Value boxed;
        if (try_int_compare_box(n, &boxed, ok)) return boxed;
        if (!*ok) return {};
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
    bool emit_type_params(const std::vector<type_param>& tps, const SourceLoc& loc,
                          ir::Value* tup,
                          std::vector<std::pair<std::string, ir::Value>>* binds) {
        bool ok = true;
        *tup = call_capi_imm("PyTuple_New", {}, (std::int64_t)tps.size(), 0, loc, &ok);
        if (!ok) return false;
        mark_owned(*tup);
        for (std::size_t i = 0; i < tps.size(); ++i) {
            std::string nm;
            int kind = 0;
            const expr* bound = nullptr;
            const expr* deflt = nullptr;
            std::visit(ov{
                [&](const TypeVar& t) {
                    nm = t.name; kind = 0;
                    if (t.bound) bound = &**t.bound;
                    if (t.default_value) deflt = &**t.default_value;
                },
                [&](const TypeVarTuple& t) {
                    nm = t.name; kind = 1;
                    if (t.default_value) deflt = &**t.default_value;
                },
                [&](const ParamSpec& t) {
                    nm = t.name; kind = 2;
                    if (t.default_value) deflt = &**t.default_value;
                },
            }, tps[i].v);
            ir::Value kn = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstInt, {}, kn, Ownership::Owned,
                           std::to_string(kind), 0, 0, loc, std::nullopt});
            mark_owned(kn);
            ir::Value namestr = const_str(nm, loc);
            ir::Value b, d;
            if (bound) {
                b = lower_expr(*bound, &ok);
                if (!ok) return false;
            } else {
                b = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::ConstNone, {}, b, Ownership::Owned, "",
                               0, 0, loc, std::nullopt});
                mark_owned(b);
            }
            if (deflt) {
                d = lower_expr(*deflt, &ok);
                if (!ok) return false;
            } else {
                d = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
                emit(ir::Instr{ir::Op::ConstNone, {}, d, Ownership::Owned, "",
                               0, 0, loc, std::nullopt});
                mark_owned(d);
            }
            ir::Value tv = call_capi("pyc_rt_type_param", {kn, namestr, b, d},
                                     loc, &ok, {kn, namestr, b, d});
            if (!ok) return false;
            mark_owned(tv);
            emit(ir::Instr{ir::Op::IncRef, {tv}, std::nullopt,
                           Ownership::NotAnObject, "", 0, 0, loc, std::nullopt});
            call_capi_imm("PyTuple_SetItem", {*tup, tv}, (std::int64_t)i, 1, loc, &ok);
            if (!ok) return false;
            binds->push_back({nm, tv});
        }
        return true;
    }

    bool lower_type_alias(const TypeAlias& n) {
        const Name* nm = n.name ? std::get_if<Name>(&n.name->v) : nullptr;
        if (!nm) return unsupported("type alias target", n.loc);
        bool ok = true;
        ir::Value namestr = const_str(nm->id, n.loc);
        ir::Value params;
        std::vector<std::pair<std::string, ir::Value>> bindings;
        const bool in_class = !class_ns_.empty();
        if (n.type_params.empty()) {
            params = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, params, Ownership::Owned, "",
                           0, 0, n.loc, std::nullopt});
            mark_owned(params);
        } else {
            if (!emit_type_params(n.type_params, n.loc, &params, &bindings))
                return false;
            if (in_class) {
                for (auto& [k, v] : bindings) store_name_keep(k, v, n.loc);
            } else {
                std::map<std::string, ir::Value> env;
                for (auto& [k, v] : bindings) env[k] = v;
                type_param_env_.push_back(std::move(env));
            }
        }
        ir::Value val = lower_expr(*n.value, &ok);
        if (!n.type_params.empty()) {
            if (in_class) {
                for (auto& [k, v] : bindings) {
                    ir::Value key = const_str(k, n.loc);
                    call_capi("pyc_rt_del_if_same", {class_ns_.back(), key, v},
                              n.loc, &ok, {key});
                    if (!ok) return false;
                }
            } else {
                type_param_env_.pop_back();
            }
        }
        if (!ok) return false;
        ir::Value ta = call_capi("pyc_rt_type_alias", {namestr, val, params},
                                 n.loc, &ok, {namestr, val, params});
        if (!ok) return false;
        mark_owned(ta);
        store_name(nm->id, ta, n.loc);
        for (auto& [k, v] : bindings) if (owns(v)) release(v, n.loc);
        return true;
    }

    ir::Value lower_interpolation(const Interpolation& n, bool* ok) {
        ir::Value val = lower_expr(*n.value, ok);
        if (!*ok) return {};
        ir::Value expr = const_str(n.str, n.loc);
        ir::Value conv;
        if (n.conversion == 's' || n.conversion == 'r' || n.conversion == 'a') {
            conv = const_str(std::string(1, (char)n.conversion), n.loc);
        } else {
            conv = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
            emit(ir::Instr{ir::Op::ConstNone, {}, conv, Ownership::Owned, "",
                           0, 0, n.loc, std::nullopt});
            mark_owned(conv);
        }
        ir::Value spec;
        if (n.format_spec && *n.format_spec) {
            spec = lower_expr(**n.format_spec, ok);
            if (!*ok) return {};
        } else {
            spec = const_str("", n.loc);
        }
        ir::Value out = call_capi("pyc_rt_interpolation", {val, expr, conv, spec},
                                  n.loc, ok, {val, expr, conv, spec});
        if (*ok) mark_owned(out);
        return out;
    }

    ir::Value lower_template(const TemplateStr& n, bool* ok) {
        ir::Value parts = call_capi_imm("PyList_New", {}, 0, 0, n.loc, ok);
        if (!*ok) return {};
        mark_owned(parts);
        for (const expr& e : n.values) {
            ir::Value v = lower_expr(e, ok);
            if (!*ok) return {};
            call_capi("PyList_Append", {parts, v}, n.loc, ok, {v});
            if (!*ok) return {};
        }
        ir::Value out = call_capi("pyc_rt_template", {parts}, n.loc, ok, {parts});
        if (*ok) mark_owned(out);
        return out;
    }

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

    // Building a str ALLOCATES, so it can fail, and an unchecked failure is a
    // null passed straight into the next C-API call.
    //
    // CPython never hits this: its name constants are interned into co_names
    // at compile time, so an attribute access allocates nothing. pyc creates a
    // fresh str per access at run time, which makes every one of them a
    // failure point that CPython does not have.
    //
    // It is reachable. Lib/test/test_pyexpat's test_error_path_no_crash
    // deliberately installs a no-memory hook, and the very next attribute name
    // came back null and went into PyObject_GetAttr as the name -- SIGSEGV
    // where CPython raises MemoryError. codegen's check() was already there and
    // did nothing, because it returns early without an error edge and this site
    // never attached one.
    ir::Value const_str(const std::string& text, const SourceLoc& loc) {
        ir::Value v = cur()->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::ConstStr, {}, v, Ownership::Owned, text,
                       0, 0, loc, make_landing_pad(loc)});
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
