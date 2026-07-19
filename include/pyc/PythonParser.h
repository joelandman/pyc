#pragma once
#include <string>
#include <memory>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
#include <Python.h>
#ifdef __cplusplus
}
#endif

namespace pyc {

struct ASTNode {
    std::string type;
    std::string value;
    std::string op;
    std::string id;
    int lineno = 0;
    bool is_float = false;
    bool is_str   = false;
    bool is_bool  = false;
    bool is_none  = false;
    bool is_complex = false;
    double complex_real = 0.0;
    double complex_imag = 0.0;
    std::vector<std::string> args;
    std::vector<std::unique_ptr<ASTNode>> children;
};

class PythonParser {
public:
    std::unique_ptr<ASTNode> parseFile(const std::string& path);
    std::unique_ptr<ASTNode> parse(const std::string& source, const std::string& filename = "<string>");
    
private:
    PyObject* getAstModule();
};

} // namespace pyc
