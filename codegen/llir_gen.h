#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace pyc::codegen {

struct FuncDecl {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
};

// Forward declaration - actual impl in llir_gen.cpp
class LlirGen {
public:
    std::vector<FuncDecl> functions;
    std::string generate_code(const std::string& module_name);
};

} // namespace pyc::codegen
