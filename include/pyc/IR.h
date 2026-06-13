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
    // Conservative compiler-known type for result values. Current codegen
    // still treats values as boxed unless it explicitly opts into a native path.
    std::string resultType;
};

struct IRFunction {
    std::string name;
    std::vector<std::string> args;
    std::vector<IRInstruction> body;
    // Variables that should use module-level (global) storage in this function.
    std::vector<std::string> globalVars;
};

class ModuleIR {
public:
    std::vector<IRFunction> functions;
    // All variable names that are stored at module scope (declared global anywhere).
    std::vector<std::string> moduleGlobals;

    void addFunction(const std::string& name, const std::vector<std::string>& args = {});
    void addInstruction(const std::string& funcName, const std::string& op, const std::vector<std::string>& operands, const std::string& result = "", const std::string& resultType = "boxed");
    void addInstructionRaw(const std::string& funcName, const std::string& op, const std::vector<IRValue>& operands, const std::string& result = "", const std::string& resultType = "boxed");
    void setFunctionGlobals(const std::string& funcName, const std::vector<std::string>& globals);
    void addModuleGlobal(const std::string& name);
};

} // namespace pyc
