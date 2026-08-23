#pragma once
// INTERFACES.md §4 — the C-API surface lowering may use.
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace pyc::rt {

// Mirrors pyc::ir::Ownership; kept here so A2 has no dependency on A3.
enum class Ownership { Owned, Borrowed, AlwaysNull, NotAnObject };

struct CApiSymbol {
    std::string_view name;
    Ownership        returns;
    bool             may_raise;      // caller MUST provide an error edge
    int              arity;
    // Parameter indices whose reference this call STEALS. Emitting an extra
    // DECREF for one of these is a double free; omitting our INCREF where a
    // call does NOT steal is a leak. Not derivable from CPython's data.
    std::initializer_list<int> steals;
    bool             banned;
    std::string_view ban_reason;

    bool steals_param(int i) const {
        for (int s : steals) if (s == i) return true;
        return false;
    }
};

// Nullptr if unknown. INTERFACES §4: lowering may not emit a call to a symbol
// that is absent here, so refcount discipline stays checkable rather than
// hopeful.
const CApiSymbol* lookup(std::string_view name);

}  // namespace pyc::rt
