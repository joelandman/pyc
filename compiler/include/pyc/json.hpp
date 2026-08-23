#pragma once
// Minimal JSON reader for the parse boundary (INTERFACES.md §2.1).
//
// FLAT by design. A tree of nested std::variant nodes would recurse in its
// DESTRUCTOR as well as its parser, so a 568-level document (INTERFACES §1.2)
// would overflow the stack on cleanup even after a careful iterative parse.
// Storing every value in one vector and referring to children by index makes
// both parsing and teardown iterative for free.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pyc::json {

enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

struct Value {
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::uint32_t str = 0;    // index into Doc::strings  (String)
    std::uint32_t first = 0;  // index into children/members
    std::uint32_t count = 0;
};

struct Member { std::uint32_t key; std::uint32_t value; };

class Doc {
public:
    std::vector<Value> values;
    std::vector<std::string> strings;
    std::vector<std::uint32_t> children;  // array elements -> value indices
    std::vector<Member> members;          // object members

    const Value& root() const { return values.at(0); }

    std::string_view str_of(const Value& v) const { return strings.at(v.str); }

    // Object lookup. Linear, but AST nodes have <10 fields, so a map would
    // cost more than it saves.
    const Value* find(const Value& obj, std::string_view key) const {
        for (std::uint32_t i = 0; i < obj.count; ++i) {
            const Member& m = members[obj.first + i];
            if (strings[m.key] == key) return &values[m.value];
        }
        return nullptr;
    }
    const Value& elem(const Value& arr, std::uint32_t i) const {
        return values[children[arr.first + i]];
    }
};

struct ParseError {
    std::string message;
    std::size_t offset = 0;
    int line = 0;
};

// Iterative. Returns false and fills `err` on malformed input; never throws,
// never recurses, and imposes no nesting limit of its own.
bool parse(std::string_view text, Doc& out, ParseError& err);

}  // namespace pyc::json
