#pragma once
// Hand-written support for the GENERATED AST (INTERFACES.md §2.2/§2.3).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pyc/diagnostics.hpp"   // SourceLoc

namespace pyc {

// Value-semantic indirection. AST sum types are recursive, so a node cannot
// hold another by value; Box gives pointer indirection with value semantics,
// so the tree stays copyable and destructible without hand-written rule-of-5.
template <class T>
class Box {
    std::unique_ptr<T> p_;
public:
    Box() : p_(nullptr) {}
    explicit Box(T v) : p_(std::make_unique<T>(std::move(v))) {}
    Box(Box&&) noexcept = default;
    Box& operator=(Box&&) noexcept = default;
    Box(const Box& o) : p_(o.p_ ? std::make_unique<T>(*o.p_) : nullptr) {}
    Box& operator=(const Box& o) {
        p_ = o.p_ ? std::make_unique<T>(*o.p_) : nullptr;
        return *this;
    }
    T&       operator*()        { return *p_; }
    const T& operator*()  const { return *p_; }
    T*       operator->()       { return p_.get(); }
    const T* operator->() const { return p_.get(); }
    explicit operator bool() const { return static_cast<bool>(p_); }
    T*       get()        const { return p_.get(); }
};

template <class T> Box<T> box(T v) { return Box<T>(std::move(v)); }

// A Python literal. `int` is arbitrary precision, so it is carried as its
// decimal text and only materialised by the runtime -- storing it in an
// int64_t here is exactly how the old tree wrapped math.factorial(25).
struct ConstNone {};
struct ConstEllipsis {};
struct ConstBigInt   { std::string digits; };
struct ConstFloat    { double value; };
struct ConstComplex  { double real, imag; };
struct ConstStr      { std::string value; };
struct ConstBytes    { std::string value; };  // raw bytes, may contain NUL
struct ConstBool     { bool value; };
struct ConstantValue;
struct ConstTuple    { std::vector<ConstantValue> items; };
struct ConstFrozenSet{ std::vector<ConstantValue> items; };

struct ConstantValue {
    std::variant<ConstNone, ConstEllipsis, ConstBool, ConstBigInt, ConstFloat,
                 ConstComplex, ConstStr, ConstBytes, ConstTuple,
                 ConstFrozenSet> v;
};

// Overload-set helper for std::visit.
//
// A missing alternative here is a HARD COMPILE ERROR, which is what gives
// CHARTER I4 its teeth in C++20. Never add a generic `auto` arm: that
// compiles and silently swallows every node kind CPython adds in future.
// The build lint enforces this (INTERFACES.md §2.3).
template <class... Ts> struct ov : Ts... { using Ts::operator()...; };
template <class... Ts> ov(Ts...) -> ov<Ts...>;

}  // namespace pyc
