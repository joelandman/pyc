#include "pyc/ast/from_json_support.hpp"

#include <array>

namespace pyc {

std::string b64_decode(std::string_view in) {
    static constexpr std::string_view kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> rev{};
    rev.fill(-1);
    for (int i = 0; i < 64; ++i) rev[static_cast<unsigned char>(kAlpha[i])] = i;

    std::string out;
    out.reserve(in.size() / 4 * 3);
    int acc = 0, bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\n' || ch == '\r') continue;
        int d = rev[static_cast<unsigned char>(ch)];
        if (d < 0) continue;
        acc = (acc << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

namespace ast {

bool const_from_json(ConstantValue& o, const JsonCtx& c, const json::Value* v) {
    if (!v || v->kind != json::Kind::Object)
        return c.error("Constant.value must be a tagged object");
    const json::Value* t = c.doc.find(*v, "t");
    if (!t || t->kind != json::Kind::String)
        return c.error("Constant.value has no type tag");
    std::string_view tag = c.doc.str_of(*t);
    const json::Value* val = c.doc.find(*v, "v");

    auto text = [&]() -> std::string {
        return (val && val->kind == json::Kind::String)
                   ? std::string(c.doc.str_of(*val)) : std::string();
    };

    if (tag == "none")     { o.v = ConstNone{};     return true; }
    if (tag == "ellipsis") { o.v = ConstEllipsis{}; return true; }
    if (tag == "bool")     { o.v = ConstBool{val && val->kind == json::Kind::Bool
                                             && val->boolean}; return true; }
    // Arbitrary precision: kept as decimal TEXT. Parsing it into an int64_t
    // here is precisely how the old runtime wrapped math.factorial(25).
    if (tag == "int")      { o.v = ConstBigInt{text()};  return true; }
    if (tag == "float")    { o.v = ConstFloat{std::strtod(text().c_str(), nullptr)}; return true; }
    if (tag == "complex") {
        const json::Value* re = c.doc.find(*v, "re");
        const json::Value* im = c.doc.find(*v, "im");
        auto num = [&](const json::Value* x) {
            return x && x->kind == json::Kind::String
                       ? std::strtod(std::string(c.doc.str_of(*x)).c_str(), nullptr) : 0.0;
        };
        o.v = ConstComplex{num(re), num(im)};
        return true;
    }
    if (tag == "str")      { o.v = ConstStr{text()}; return true; }
    if (tag == "str_raw")  { o.v = ConstStr{b64_decode(text())}; return true; }
    if (tag == "bytes")    { o.v = ConstBytes{b64_decode(text())}; return true; }
    if (tag == "tuple" || tag == "frozenset") {
        std::vector<ConstantValue> items;
        if (val && val->kind == json::Kind::Array) {
            items.reserve(val->count);
            for (std::uint32_t i = 0; i < val->count; ++i) {
                ConstantValue cv;
                if (!const_from_json(cv, c, &c.doc.elem(*val, i))) return false;
                items.push_back(std::move(cv));
            }
        }
        if (tag == "tuple") o.v = ConstTuple{std::move(items)};
        else                o.v = ConstFrozenSet{std::move(items)};
        return true;
    }
    return c.error("unknown constant tag '" + std::string(tag) + "'",
                   std::string(tag));
}

}  // namespace ast
}  // namespace pyc
