#pragma once
// INTERFACES.md §4 — the C-API surface lowering may use.
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace pyc::rt {

// Mirrors pyc::ir::Ownership; kept here so A2 has no dependency on A3.
// Unknown is not a variant of "borrowed" -- it means no source records this
// symbol's contract. Guessing would leak or double-free, so an Unknown symbol
// is not emittable (INTERFACES §4).
enum class Ownership { Owned, Borrowed, AlwaysNull, NotAnObject, Unknown };

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
    bool             stable_abi;     // present in Misc/stable_abi.toml
    // Version that introduced the symbol ("3.2", "3.13", or empty if not
    // recorded). Emitting a symbol newer than the target is a version error,
    // which is the C-API half of I8.
    std::string_view added;

    bool steals_param(int i) const {
        for (int s : steals) if (s == i) return true;
        return false;
    }

    // The single question lowering must ask before emitting a call.
    bool emittable() const {
        return !banned && returns != Ownership::Unknown;
    }

    // Available on the target? `added` is empty when unrecorded, which is
    // treated as available: refcounts.dat carries no version data and most of
    // its symbols long predate any target we support.
    bool available_in(int major, int minor) const {
        if (added.empty()) return true;
        auto dot = added.find('.');
        if (dot == std::string_view::npos) return true;
        int amaj = 0, amin = 0;
        for (char c : added.substr(0, dot)) amaj = amaj * 10 + (c - '0');
        for (char c : added.substr(dot + 1)) amin = amin * 10 + (c - '0');
        return (major > amaj) || (major == amaj && minor >= amin);
    }
};

// Nullptr if unknown. INTERFACES §4: lowering may not emit a call to a symbol
// that is absent here, so refcount discipline stays checkable rather than
// hopeful.
const CApiSymbol* lookup(std::string_view name);

}  // namespace pyc::rt
