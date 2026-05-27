#pragma once
#include <string>
#include <memory>
#include <vector>

namespace pyc {

struct ASTNode {
    std::string type;
    std::string value;
    std::string op;
    std::string id;
    int lineno = 0;
    std::vector<std::string> args;
    std::vector<std::unique_ptr<ASTNode>> children;
};

class PythonParser {
public:
    std::unique_ptr<ASTNode> parseFile(const std::string& path);
    std::unique_ptr<ASTNode> parse(const std::string& source, const std::string& filename = "<string>");
};

} // namespace pyc
