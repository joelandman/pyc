#pragma once
// INTERFACES.md §1.1. Every layer reports through this.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pyc {

struct SourceLoc {
    std::string file;
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

struct Diagnostic {
    enum class Severity { Error, Warning, Note };
    Severity severity = Severity::Error;
    std::string message;
    SourceLoc loc;
    std::string construct;
    std::optional<std::string> introduced_in;
};

class DiagnosticSink {
public:
    virtual ~DiagnosticSink() = default;
    virtual void report(Diagnostic d) = 0;
    virtual bool had_error() const = 0;
};

class CollectingSink : public DiagnosticSink {
public:
    std::vector<Diagnostic> items;
    void report(Diagnostic d) override {
        if (d.severity == Diagnostic::Severity::Error) error_ = true;
        items.push_back(std::move(d));
    }
    bool had_error() const override { return error_; }
private:
    bool error_ = false;
};

std::string b64_decode(std::string_view in);

}  // namespace pyc
