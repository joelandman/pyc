// Reads a pyc_parse envelope, lowers it, prints IR or diagnostics.
#include "pyc/ast/generated_from_json.hpp"
#include "pyc/genexp.hpp"
#include "pyc/ir/ir.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>

namespace pyc {
bool lower_to_ir(const ast::mod&, const std::string&, ir::Module&, DiagnosticSink&,
                 const std::vector<GenexpEntry>&);
std::string codegen_llvm(const ir::Module&);
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
    // A parse failure arrives as a structured `error` envelope on stdout, not
    // as text on stderr. Rendering it here is what makes a SyntaxError reach
    // the user: pycc redirects our stdin's producer into a file, so an
    // unrendered envelope was reported as a silent exit-1 with no diagnostic
    // at all -- a compiler that fails without saying why (INTERFACES §1.1).
    if (const json::Value* ev = doc.find(doc.root(), "error")) {
        auto str = [&](const char* k, const char* dflt) {
            const json::Value* v = doc.find(*ev, k);
            return v ? std::string(doc.str_of(*v)) : std::string(dflt);
        };
        auto num = [&](const char* k) {
            const json::Value* v = doc.find(*ev, k);
            return v ? (int)v->number : 0;
        };
        std::fprintf(stderr, "%s:%d:%d: error: %s [%s]\n",
                     str("file", "<input>").c_str(), num("line"), num("col"),
                     str("message", "invalid syntax").c_str(),
                     str("kind", "SyntaxError").c_str());
        return 1;
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

    // Generator expressions: marshalled code objects from the parse stage,
    // keyed by source position (rebuild/GENERATORS.md).
    std::vector<GenexpEntry> genexps;
    if (const json::Value* gv = doc.find(doc.root(), "genexps")) {
        for (std::uint32_t i = 0; i < gv->count; ++i) {
            const json::Value& e = doc.elem(*gv, i);
            GenexpEntry g;
            if (const json::Value* x = doc.find(e, "line")) g.line = (int)x->number;
            if (const json::Value* x = doc.find(e, "col"))  g.col  = (int)x->number;
            if (const json::Value* x = doc.find(e, "code")) g.code = b64_decode(doc.str_of(*x));
            if (const json::Value* x = doc.find(e, "freevars"))
                for (std::uint32_t k = 0; k < x->count; ++k)
                    g.freevars.push_back(std::string(doc.str_of(doc.elem(*x, k))));
            genexps.push_back(std::move(g));
        }
    }

    ir::Module m;
    if (!lower_to_ir(tree, file, m, sink, genexps)) {
        for (const auto& d : sink.items) {
            if (d.severity != Diagnostic::Severity::Error) continue;
            std::fprintf(stderr, "%s:%d:%d: error: %s [%s]\n",
                         d.loc.file.c_str(), d.loc.line, d.loc.col,
                         d.message.c_str(), d.construct.c_str());
        }
        return 1;
    }
    // --emit-llvm selects the backend; the default stays the IR dump, which
    // is what every existing check reads.
    bool llvm = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--emit-llvm") llvm = true;
    std::fputs(llvm ? codegen_llvm(m).c_str() : ir::to_string(m).c_str(), stdout);
    return 0;
}
