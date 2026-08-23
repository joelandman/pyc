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
        mod_.functions.push_back(ir::Function{"__main__", {}, {}, 1});
        fn_ = &mod_.functions.back();
        fn_->blocks.push_back(ir::Block{"entry", {}});
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
    ir::Function* fn_ = nullptr;
    std::size_t blk_ = 0;

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

    void emit(ir::Instr i) { fn_->blocks[blk_].instrs.push_back(std::move(i)); }

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
        ir::Value out = fn_->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        std::vector<ir::Value> saved = args;
        ir::Instr in{ir::Op::CallCApi, std::move(args), out,
                     sym->returns, symbol, 0, 0, loc, std::nullopt};
        if (sym->may_raise) in.on_error = kErrorBlock;
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

    static constexpr std::uint32_t kErrorBlock = 0xFFFFFFFFu;  // placeholder

    // --- statements --------------------------------------------------------

    bool lower_stmt(const stmt& s) {
        bool ok = true;
        std::visit(ov{
            [&](const Expr& e)   { ok = lower_expr_stmt(e); },
            [&](const Assign& a) { ok = lower_assign(a); },
            [&](const Pass&)     { /* nothing to emit */ },
            // Everything else is a diagnostic, one arm each. No generic arm:
            // a node kind added upstream must break this build (I4).
            [&](const FunctionDef& n)      { ok = unsupported("function definitions", n.loc); },
            [&](const AsyncFunctionDef& n) { ok = unsupported("async function definitions", n.loc); },
            [&](const ClassDef& n)         { ok = unsupported("class definitions", n.loc); },
            [&](const Return& n)           { ok = unsupported("return", n.loc); },
            [&](const Delete& n)           { ok = unsupported("del", n.loc); },
            [&](const AugAssign& n)        { ok = unsupported("augmented assignment", n.loc); },
            [&](const AnnAssign& n)        { ok = unsupported("annotated assignment", n.loc); },
            [&](const For& n)              { ok = unsupported("for loops", n.loc); },
            [&](const AsyncFor& n)         { ok = unsupported("async for", n.loc); },
            [&](const While& n)            { ok = unsupported("while loops", n.loc); },
            [&](const If& n)               { ok = unsupported("if statements", n.loc); },
            [&](const With& n)             { ok = unsupported("with", n.loc); },
            [&](const AsyncWith& n)        { ok = unsupported("async with", n.loc); },
            [&](const Match& n)            { ok = unsupported("match", n.loc); },
            [&](const Raise& n)            { ok = unsupported("raise", n.loc); },
            [&](const Try& n)              { ok = unsupported("try", n.loc); },
            [&](const TryStar& n)          { ok = unsupported("try/except*", n.loc); },
            [&](const Assert& n)           { ok = unsupported("assert", n.loc); },
            [&](const Import& n)           { ok = unsupported("import", n.loc); },
            [&](const ImportFrom& n)       { ok = unsupported("from-import", n.loc); },
            [&](const Global& n)           { ok = unsupported("global", n.loc); },
            [&](const Nonlocal& n)         { ok = unsupported("nonlocal", n.loc); },
            [&](const Break& n)            { ok = unsupported("break", n.loc); },
            [&](const Continue& n)         { ok = unsupported("continue", n.loc); },
            [&](const TypeAlias& n)        { ok = unsupported("type aliases", n.loc); },
        }, s.v);
        return ok;
    }

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
        emit(ir::Instr{ir::Op::StoreGlobal, {v}, std::nullopt,
                       Ownership::NotAnObject, n->id, 0, 0, a.loc, std::nullopt});
        // PyDict_SetItem-style stores INCREF; our temporary reference is then
        // surplus and must go, or every assignment leaks.
        if (owns(v)) release(v, a.loc);
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
        ir::Value out = fn_->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
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
        ir::Value out = fn_->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        emit(ir::Instr{ir::Op::LoadGlobal, {}, out, Ownership::Owned,
                       n.id, 0, 0, n.loc, kErrorBlock});
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
        ir::Value out = fn_->fresh(ir::Type{ir::Type::Kind::Boxed, {}});
        std::vector<ir::Value> saved = args;
        emit(ir::Instr{ir::Op::CallObject, std::move(args), out,
                       Ownership::Owned, "", 0, 0, c.loc, kErrorBlock});
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
