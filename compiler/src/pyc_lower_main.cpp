// Single pyc binary: two front doors over one lowering core.
//
//   pyc FILE.py -o OUT [...]      the driver (INTERFACES §5): parse in the
//                                 target interpreter, lower in-process, link.
//   pyc --lower [ENVELOPE|-]      the former standalone `pyc_lower`: read a
//                                 pyc_parse envelope, print IR. No sysroot, no
//                                 link -- this is how check_ir_wellformed.py
//                                 drives the lowering directly.
//
// The lowering itself is `lower_envelope` below; both doors call it.
#include "pyc/ast/generated_from_json.hpp"
#include "pyc/genexp.hpp"
#include "pyc/ir/ir.hpp"
#include "pyc/lower_api.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

namespace pyc {
bool lower_to_ir(const ast::mod&, const std::string&, ir::Module&, DiagnosticSink&,
                 const std::vector<GenexpEntry>&, const std::vector<FuncCodeEntry>&);
}
using namespace pyc;

// Read a pyc_parse envelope, deserialize the AST, lower it into `m`.
// Diagnostics go to stderr (INTERFACES §1.1). Returns 0 on success.
//
// Shared by the driver (in-process) and the `--lower` door; it is the whole
// "backend" the user used to have to run as a separate `pyc_lower` binary.
namespace pyc {
int lower_envelope(const std::string& json_text, const std::string& default_file,
                   ir::Module& m) {
    json::Doc doc; json::ParseError perr;
    if (!json::parse(json_text, doc, perr)) {
        std::fprintf(stderr, "json: %s (line %d)\n", perr.message.c_str(), perr.line);
        return 2;
    }
    // A parse failure arrives as a structured `error` envelope on stdout, not
    // as text on stderr. Rendering it here is what makes a SyntaxError reach
    // the user: the driver redirects the parser's stdout into the envelope, so
    // an unrendered envelope would be reported as a silent exit-1 with no
    // diagnostic at all -- a compiler that fails without saying why
    // (INTERFACES §1.1).
    if (const json::Value* ev = doc.find(doc.root(), "error")) {
        auto str = [&](const char* k, const char* dflt) {
            const json::Value* x = doc.find(*ev, k);
            return x ? std::string(doc.str_of(*x)) : std::string(dflt);
        };
        auto num = [&](const char* k) {
            const json::Value* x = doc.find(*ev, k);
            return x ? (int)x->number : 0;
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
    std::string file = fv ? std::string(doc.str_of(*fv)) : default_file;

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

    std::vector<FuncCodeEntry> func_codes;
    if (const json::Value* fvcs = doc.find(doc.root(), "func_codes")) {
        for (std::uint32_t i = 0; i < fvcs->count; ++i) {
            const json::Value& e = doc.elem(*fvcs, i);
            FuncCodeEntry fc;
            if (const json::Value* x = doc.find(e, "qual")) fc.qual = std::string(doc.str_of(*x));
            if (const json::Value* x = doc.find(e, "line")) fc.line = (int)x->number;
            if (const json::Value* x = doc.find(e, "end_col")) fc.end_col = (int)x->number;
            if (const json::Value* x = doc.find(e, "code")) fc.code = b64_decode(doc.str_of(*x));
            auto names = [&](const char* key, std::vector<std::string>& dst) {
                const json::Value* x = doc.find(e, key);
                if (!x) return;
                for (std::uint32_t k = 0; k < x->count; ++k)
                    dst.push_back(std::string(doc.str_of(doc.elem(*x, k))));
            };
            names("locals", fc.locals);
            names("freevars", fc.freevars);
            func_codes.push_back(std::move(fc));
        }
    }

    if (!lower_to_ir(tree, file, m, sink, genexps, func_codes)) {
        for (const auto& d : sink.items) {
            if (d.severity != Diagnostic::Severity::Error) continue;
            std::fprintf(stderr, "%s:%d:%d: error: %s [%s]\n",
                         d.loc.file.c_str(), d.loc.line, d.loc.col,
                         d.message.c_str(), d.construct.c_str());
        }
        return 1;
    }
    return 0;
}
}  // namespace pyc

// The driver lives in compiler/src/driver.cpp; declared here only to dispatch.
int pyc_driver_main(int argc, char** argv);

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--lower") {
        // Former `pyc_lower`: read an envelope from argv[2] (or stdin for "-")
        // and print IR. `--emit-llvm` selects LLVM; the default stays the IR
        // dump, which is what every existing check reads.
        std::string inpath;
        bool emit = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--emit-llvm") emit = true;
            else if (a == "-") inpath.clear();
            else if (inpath.empty()) inpath = a;
        }
        std::ostringstream buf;
        if (!inpath.empty()) {
            FILE* f = std::fopen(inpath.c_str(), "rb");
            if (!f) { std::perror("open"); return 2; }
            char t[65536]; size_t n;
            while ((n = std::fread(t, 1, sizeof t, f)) > 0) buf.write(t, (std::streamsize)n);
            std::fclose(f);
        } else buf << std::cin.rdbuf();
        ir::Module m;
        int rc = lower_envelope(buf.str(), "<input>", m);
        if (rc) return rc;
        std::fputs(emit ? codegen_llvm(m).c_str() : ir::to_string(m).c_str(), stdout);
        return 0;
    }
    return pyc_driver_main(argc, argv);
}
