#pragma once
#include <string>
#include <vector>
#include <memory>

namespace pyc {

struct IRValue {
    std::string name;
    std::string type;
};

struct IRInstruction {
    std::string op;
    std::vector<IRValue> operands;
    std::string result;
};

struct IRFunction {
    std::string name;
    std::vector<std::string> args;
    std::vector<IRInstruction> body;
};

class ModuleIR {
public:
    std::vector<IRFunction> functions;
    void addFunction(const std::string& name, const std::vector<std::string>& args = {});
    void addInstruction(const std::string& funcName, const std::string& op, const std::vector<std::string>& operands, const std::string& result = "");
};

} // namespace pyc
