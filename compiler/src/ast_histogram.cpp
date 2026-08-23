// Reads a pyc_parse envelope, deserializes it into the typed AST, and prints a
// node-kind histogram. Compared against CPython's own count for the same file,
// this proves the C++ reader drops nothing -- a lost subtree changes counts.
#include "pyc/ast/generated_from_json.hpp"
#include "pyc/ast/generated_walk.hpp"

#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>

using namespace pyc;

namespace {

std::map<std::string, int> g_hist;

struct Histogram : ast::WalkSink {
    void on(std::string_view kind) override { ++g_hist[std::string(kind)]; }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: ast_histogram <envelope.json>\n"); return 2; }
    std::ostringstream buf;
    if (std::string(argv[1]) == "-") buf << std::cin.rdbuf();
    else { FILE* f = std::fopen(argv[1], "rb"); if (!f) { perror("open"); return 2; }
           char tmp[65536]; size_t n; while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) buf.write(tmp, (std::streamsize)n);
           std::fclose(f); }
    const std::string text = buf.str();

    json::Doc doc; json::ParseError perr;
    if (!json::parse(text, doc, perr)) {
        std::fprintf(stderr, "json: %s at line %d\n", perr.message.c_str(), perr.line);
        return 1;
    }
    const json::Value* astv = doc.find(doc.root(), "ast");
    if (!astv) { std::fprintf(stderr, "envelope has no 'ast'\n"); return 1; }

    const json::Value* fv = doc.find(doc.root(), "file");
    CollectingSink sink;
    ast::JsonCtx ctx{doc, sink, fv ? std::string(doc.str_of(*fv)) : std::string("<input>")};

    ast::mod module;
    if (!ast::from_json(module, ctx, *astv, ast::Depth{})) {
        for (const auto& d : sink.items)
            std::fprintf(stderr, "error: %s [%s]\n", d.message.c_str(), d.construct.c_str());
        return 1;
    }
    Histogram hist;
    ast::walk(module, hist);
    long total = 0;
    for (const auto& [k, v] : g_hist) { std::printf("%s %d\n", k.c_str(), v); total += v; }
    std::printf("__total__ %ld\n", total);
    return 0;
}
