#include "pyc/json.hpp"

#include <cmath>
#include <cstdlib>

namespace pyc::json {
namespace {

struct Frame {
    std::uint32_t value_index;
    bool is_object;
    std::vector<std::uint32_t> kids;   // array element value indices
    std::vector<Member> mems;          // object members
};

class Parser {
public:
    Parser(std::string_view t, Doc& d, ParseError& e) : t_(t), d_(d), e_(e) {}

    bool run() {
        skip_ws();
        if (!parse_value()) return false;
        skip_ws();
        if (p_ != t_.size()) return fail("trailing content after top-level value");
        return true;
    }

private:
    std::string_view t_;
    Doc& d_;
    ParseError& e_;
    std::size_t p_ = 0;

    bool fail(const char* msg) {
        e_.message = msg;
        e_.offset = p_;
        e_.line = 1;
        for (std::size_t i = 0; i < p_ && i < t_.size(); ++i)
            if (t_[i] == '\n') ++e_.line;
        return false;
    }

    void skip_ws() {
        while (p_ < t_.size()) {
            char c = t_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p_;
            else break;
        }
    }

    bool lit(std::string_view s) {
        if (t_.compare(p_, s.size(), s) != 0) return false;
        p_ += s.size();
        return true;
    }

    std::uint32_t emit(Kind k) {
        d_.values.push_back(Value{});
        d_.values.back().kind = k;
        return static_cast<std::uint32_t>(d_.values.size() - 1);
    }

    // The whole point: an explicit stack, so nesting depth costs heap, not
    // C stack. No recursion anywhere in this function.
    bool parse_value() {
        std::vector<Frame> stack;

        for (;;) {
            skip_ws();
            if (p_ >= t_.size()) return fail("unexpected end of input");

            std::uint32_t idx = 0;   // set by every path below
            char c = t_[p_];

            if (c == '{' || c == '[') {
                ++p_;
                idx = emit(c == '{' ? Kind::Object : Kind::Array);
                skip_ws();
                char close = (c == '{') ? '}' : ']';
                if (p_ < t_.size() && t_[p_] == close) {
                    ++p_;                       // empty container
                } else {
                    stack.push_back(Frame{idx, c == '{', {}, {}});
                    // An object frame must have its FIRST key parsed here;
                    // only subsequent keys follow a comma. Without this,
                    // attaching the first value dereferences an empty
                    // member vector.
                    if (stack.back().is_object && !parse_key(stack.back()))
                        return false;
                    continue;                   // descend without recursing
                }
            } else if (!parse_scalar(idx)) {
                return false;
            }

            // Attach `idx` to its parent, then close finished containers.
            for (;;) {
                if (stack.empty()) return true;

                Frame& f = stack.back();
                if (f.is_object) {
                    f.mems.back().value = idx;
                } else {
                    f.kids.push_back(idx);
                }

                skip_ws();
                if (p_ >= t_.size()) return fail("unterminated container");
                char d = t_[p_];
                if (d == ',') {
                    ++p_;
                    if (f.is_object && !parse_key(f)) return false;
                    break;                      // parse the next value
                }
                if ((f.is_object && d == '}') || (!f.is_object && d == ']')) {
                    ++p_;
                    idx = close_frame(f);
                    stack.pop_back();
                    continue;                   // bubble up
                }
                return fail("expected ',' or closing bracket");
            }
        }
    }

    std::uint32_t close_frame(Frame& f) {
        Value& v = d_.values[f.value_index];
        if (f.is_object) {
            v.first = static_cast<std::uint32_t>(d_.members.size());
            v.count = static_cast<std::uint32_t>(f.mems.size());
            d_.members.insert(d_.members.end(), f.mems.begin(), f.mems.end());
        } else {
            v.first = static_cast<std::uint32_t>(d_.children.size());
            v.count = static_cast<std::uint32_t>(f.kids.size());
            d_.children.insert(d_.children.end(), f.kids.begin(), f.kids.end());
        }
        return f.value_index;
    }

    bool parse_key(Frame& f) {
        skip_ws();
        std::string s;
        if (!parse_string_raw(s)) return false;
        skip_ws();
        if (p_ >= t_.size() || t_[p_] != ':') return fail("expected ':' after key");
        ++p_;
        d_.strings.push_back(std::move(s));
        f.mems.push_back(Member{static_cast<std::uint32_t>(d_.strings.size() - 1), 0});
        return true;
    }

    bool parse_scalar(std::uint32_t& idx) {
        char c = t_[p_];
        if (c == '"') {
            std::string s;
            if (!parse_string_raw(s)) return false;
            idx = emit(Kind::String);
            d_.strings.push_back(std::move(s));
            d_.values[idx].str = static_cast<std::uint32_t>(d_.strings.size() - 1);
            return true;
        }
        if (c == 't' && lit("true"))  { idx = emit(Kind::Bool); d_.values[idx].boolean = true;  return true; }
        if (c == 'f' && lit("false")) { idx = emit(Kind::Bool); d_.values[idx].boolean = false; return true; }
        if (c == 'n' && lit("null"))  { idx = emit(Kind::Null); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* begin = t_.data() + p_;
            char* end = nullptr;
            double d = std::strtod(begin, &end);
            if (end == begin) return fail("malformed number");
            p_ += static_cast<std::size_t>(end - begin);
            idx = emit(Kind::Number);
            d_.values[idx].number = d;
            return true;
        }
        return fail("unexpected character");
    }

    static void utf8_append(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(std::uint32_t& out) {
        if (p_ + 4 > t_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t_[p_ + i];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        p_ += 4;
        return true;
    }

    bool parse_string_raw(std::string& out) {
        if (p_ >= t_.size() || t_[p_] != '"') return fail("expected string");
        ++p_;
        for (;;) {
            if (p_ >= t_.size()) return fail("unterminated string");
            char c = t_[p_];
            if (c == '"') { ++p_; return true; }
            if (c != '\\') { out.push_back(c); ++p_; continue; }
            ++p_;
            if (p_ >= t_.size()) return fail("unterminated escape");
            char e = t_[p_++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    // Recombining a surrogate PAIR is correct here: Python's
                    // json.dumps escapes astral characters that way. Strings
                    // that genuinely contain LONE surrogates never reach this
                    // path -- encode.py sends those as base64 (§2.3), which is
                    // exactly why that escape hatch exists.
                    if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 1 < t_.size()
                        && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                        std::size_t save = p_;
                        p_ += 2;
                        std::uint32_t lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            p_ = save;          // not a pair; keep the high half
                        }
                    }
                    utf8_append(out, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
    }
};

}  // namespace

bool parse(std::string_view text, Doc& out, ParseError& err) {
    out.values.clear(); out.strings.clear();
    out.children.clear(); out.members.clear();
    out.values.reserve(text.size() / 16 + 8);
    Parser p(text, out, err);
    return p.run();
}

}  // namespace pyc::json
