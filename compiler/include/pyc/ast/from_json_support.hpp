#pragma once
// Hand-written support for the GENERATED deserializer.
#include "pyc/ast/generated.hpp"
#include "pyc/diagnostics.hpp"
#include "pyc/json.hpp"

#include <string_view>

namespace pyc::ast {

// Depth bounding (INTERFACES.md §1.2).
//
// The generated builders recurse. That is a deliberate deviation from §1.2's
// *preference* for an explicit worklist: building 117 heterogeneous variant
// types iteratively needs a hand-rolled state machine per node, which is more
// code and more risk than the problem warrants. §1.2's binding requirements
// are met instead -- the depth is bounded, exceeding it produces a diagnostic
// rather than a crash, and the caller sizes the stack explicitly.
//
// The measured worst case in the wild is 568 (sympy). kMaxDepth leaves two
// orders of magnitude of headroom and still fails long before an 8 MB stack.
inline constexpr int kMaxDepth = 10000;

struct JsonCtx;

struct Depth {
    int value = 0;
    Depth next() const { return Depth{value + 1}; }
    bool ok(const JsonCtx& c) const;
};

struct JsonCtx {
    const json::Doc& doc;
    DiagnosticSink& diags;
    std::string file;

    bool error(std::string msg, std::string construct = {}) const {
        diags.report(Diagnostic{Diagnostic::Severity::Error, std::move(msg),
                                SourceLoc{file, 0, 0}, std::move(construct), {}});
        return false;
    }
    // I1: an unknown node kind is a hard error naming the construct, never a
    // silent skip. This is what makes a coverage gap surface as a P2
    // COMPILE_ERROR instead of a P0 silent wrong answer.
    bool unknown_kind(std::string_view k, std::string_view sum) const {
        return error("unknown AST node kind '" + std::string(k) + "' for sum '"
                     + std::string(sum) + "'; the schema and the parser "
                     "disagree — regenerate the AST for this target",
                     std::string(k));
    }
};

inline bool Depth::ok(const JsonCtx& c) const {
    if (value < kMaxDepth) return true;
    return c.error("AST nesting exceeds " + std::to_string(kMaxDepth)
                   + " levels; refusing to recurse further",
                   "nesting-depth");
}

inline bool kind_of(const JsonCtx& c, const json::Value& v, std::string_view& out) {
    if (v.kind != json::Kind::Object) return c.error("expected an AST node object");
    const json::Value* k = c.doc.find(v, "_kind");
    if (!k || k->kind != json::Kind::String) return c.error("node has no _kind");
    out = c.doc.str_of(*k);
    return true;
}

// --- scalar fields ---------------------------------------------------------

inline bool str_one(std::string& o, const JsonCtx& c, const json::Value* v) {
    if (!v) { o.clear(); return true; }          // field absent: ast omits some
    if (v->kind == json::Kind::String) { o = std::string(c.doc.str_of(*v)); return true; }
    if (v->kind == json::Kind::Object) {         // surrogate-carrying (§2.3)
        const json::Value* raw = c.doc.find(*v, "_str_raw");
        if (raw) { o = b64_decode(c.doc.str_of(*raw)); return true; }
    }
    return c.error("expected a string field");
}
inline bool str_opt(std::optional<std::string>& o, const JsonCtx& c, const json::Value* v) {
    if (!v || v->kind == json::Kind::Null) { o.reset(); return true; }
    std::string s; if (!str_one(s, c, v)) return false;
    o = std::move(s); return true;
}
inline bool str_seq(std::vector<std::string>& o, const JsonCtx& c, const json::Value* v) {
    o.clear();
    if (!v || v->kind == json::Kind::Null) return true;
    if (v->kind != json::Kind::Array) return c.error("expected an array of strings");
    for (std::uint32_t i = 0; i < v->count; ++i) {
        std::string s; if (!str_one(s, c, &c.doc.elem(*v, i))) return false;
        o.push_back(std::move(s));
    }
    return true;
}
inline bool i64_one(std::int64_t& o, const JsonCtx& c, const json::Value* v) {
    if (!v || v->kind == json::Kind::Null) { o = 0; return true; }
    if (v->kind == json::Kind::Bool)   { o = v->boolean ? 1 : 0; return true; }
    if (v->kind != json::Kind::Number) return c.error("expected an integer field");
    o = static_cast<std::int64_t>(v->number);
    return true;
}
inline bool i64_opt(std::optional<std::int64_t>& o, const JsonCtx& c, const json::Value* v) {
    if (!v || v->kind == json::Kind::Null) { o.reset(); return true; }
    std::int64_t x; if (!i64_one(x, c, v)) return false;
    o = x; return true;
}
inline bool boolean_one(bool& o, const JsonCtx& c, const json::Value* v) {
    if (!v || v->kind == json::Kind::Null) { o = false; return true; }
    if (v->kind == json::Kind::Bool)   { o = v->boolean; return true; }
    if (v->kind == json::Kind::Number) { o = v->number != 0; return true; }
    return c.error("expected a boolean field");
}

// --- node fields -----------------------------------------------------------
// Templates, so the generated code stays a single call per field.

template <class T>
bool node_one(Box<T>& o, const JsonCtx& c, const json::Value* v, Depth d) {
    if (!v || v->kind == json::Kind::Null) { o = Box<T>(); return true; }
    T tmp;
    if (!from_json(tmp, c, *v, d)) return false;
    o = box(std::move(tmp));
    return true;
}
template <class T>
bool node_one(T& o, const JsonCtx& c, const json::Value* v, Depth d) {
    if (!v || v->kind == json::Kind::Null) return true;   // field-free sums
    return from_json(o, c, *v, d);
}
template <class T>
bool node_opt(std::optional<Box<T>>& o, const JsonCtx& c, const json::Value* v, Depth d) {
    if (!v || v->kind == json::Kind::Null) { o.reset(); return true; }
    T tmp;
    if (!from_json(tmp, c, *v, d)) return false;
    o = box(std::move(tmp));
    return true;
}
template <class T>
bool node_seq(std::vector<T>& o, const JsonCtx& c, const json::Value* v, Depth d) {
    o.clear();
    if (!v || v->kind == json::Kind::Null) return true;
    if (v->kind != json::Kind::Array) return c.error("expected an array of nodes");
    o.reserve(v->count);
    for (std::uint32_t i = 0; i < v->count; ++i) {
        T tmp;
        if (!from_json(tmp, c, c.doc.elem(*v, i), d)) return false;
        o.push_back(std::move(tmp));
    }
    return true;
}

// A list whose elements may be null: Dict.keys marks `**` entries that way,
// and arguments.kw_defaults marks a kwonly arg with no default. Dropping the
// null would silently shift every following element by one.
template <class T>
bool node_seq_opt(std::vector<std::optional<Box<T>>>& o, const JsonCtx& c,
                  const json::Value* v, Depth d) {
    o.clear();
    if (!v || v->kind == json::Kind::Null) return true;
    if (v->kind != json::Kind::Array) return c.error("expected an array of nodes");
    o.reserve(v->count);
    for (std::uint32_t i = 0; i < v->count; ++i) {
        const json::Value& e = c.doc.elem(*v, i);
        if (e.kind == json::Kind::Null) { o.emplace_back(std::nullopt); continue; }
        T tmp;
        if (!from_json(tmp, c, e, d)) return false;
        o.emplace_back(box(std::move(tmp)));
    }
    return true;
}

// --- constants (§2.3) ------------------------------------------------------

bool const_from_json(ConstantValue& o, const JsonCtx& c, const json::Value* v);

inline void loc_from_json(SourceLoc& loc, const JsonCtx& c, const json::Value& v) {
    auto get = [&](const char* k) -> int {
        const json::Value* x = c.doc.find(v, k);
        return (x && x->kind == json::Kind::Number) ? static_cast<int>(x->number) : 0;
    };
    loc.file     = c.file;
    loc.line     = get("lineno");
    loc.col      = get("col_offset");
    loc.end_line = get("end_lineno");
    loc.end_col  = get("end_col_offset");
}

}  // namespace pyc::ast
