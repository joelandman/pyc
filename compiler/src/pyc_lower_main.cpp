// Reads a pyc_parse envelope, lowers it, prints IR or diagnostics.
#include "pyc/ast/generated_from_json.hpp"
#include "pyc/ir/ir.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>

namespace pyc {
bool lower_to_ir(const ast::mod&, const std::string&, ir::Module&, DiagnosticSink&);
}
using namespace pyc;

int main(int argc, char** argv) {
    std::ostringstream buf;
    if (argc > 1 && std::string(argv[1]) != "-") {
        FILE* f = std::fopen(argv[1], "rb");
        if (!f) { std::perror("open"); return 2; }
        char t[65536]; size_t n;
        while ((n = std::fread(t, 1, sizeof t, f)) > 0) buf.write(t, (std::streamsize)n);
        std::fclose(f);
    } else buf << std::cin.rdbuf();

    json::Doc doc; json::ParseError perr;
    if (!json::parse(buf.str(), doc, perr)) {
        std::fprintf(stderr, "json: %s (line %d)\n", perr.message.c_str(), perr.line);
        return 2;
    }
    const json::Value* astv = doc.find(doc.root(), "ast");
    if (!astv) { std::fprintf(stderr, "envelope has no 'ast'\n"); return 2; }
    const json::Value* fv = doc.find(doc.root(), "file");
    std::string file = fv ? std::string(doc.str_of(*fv)) : "<input>";

    CollectingSink sink;
    ast::JsonCtx ctx{doc, sink, file};
    ast::mod tree;
    if (!ast::from_json(tree, ctx, *astv, ast::Depth{})) {
        for (const auto& d : sink.items)
            std::fprintf(stderr, "error: %s\n", d.message.c_str());
        return 1;
    }

    ir::Module m;
    if (!lower_to_ir(tree, file, m, sink)) {
        for (const auto& d : sink.items) {
            if (d.severity != Diagnostic::Severity::Error) continue;
            std::fprintf(stderr, "%s:%d:%d: error: %s [%s]\n",
                         d.loc.file.c_str(), d.loc.line, d.loc.col,
                         d.message.c_str(), d.construct.c_str());
        }
        return 1;
    }
    std::fputs(ir::to_string(m).c_str(), stdout);
    return 0;
}
