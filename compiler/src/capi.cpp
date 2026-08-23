#include "pyc/rt/capi.hpp"
#include "pyc/rt/capi_table.hpp"

#include <algorithm>
#include <unordered_map>

namespace pyc::rt {

const CApiSymbol* lookup(std::string_view name) {
    static const std::unordered_map<std::string_view, const CApiSymbol*> index = [] {
        std::unordered_map<std::string_view, const CApiSymbol*> m;
        m.reserve(kCApiSymbolCount * 2);
        for (const CApiSymbol& s : kCApiSymbols) m.emplace(s.name, &s);
        return m;
    }();
    auto it = index.find(name);
    return it == index.end() ? nullptr : it->second;
}

}  // namespace pyc::rt
