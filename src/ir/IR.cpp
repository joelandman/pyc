#include "pyc/IR.h"
#include <algorithm>

namespace pyc {

void ModuleIR::addFunction(const std::string& name, const std::vector<std::string>& args) {
    IRFunction f;
    f.name = name;
    f.args = args;
    functions.push_back(f);
}

void ModuleIR::addInstruction(const std::string& funcName, const std::string& op, const std::vector<std::string>& operands, const std::string& result) {
    auto it = std::find_if(functions.begin(), functions.end(), [&](const IRFunction& f){ return f.name == funcName; });
    if (it != functions.end()) {
        IRInstruction inst;
        inst.op = op;
        for (const auto& o : operands) {
            inst.operands.push_back({o, "i32"});
        }
        inst.result = result.empty() ? "r" + std::to_string(it->body.size()) : result;
        it->body.push_back(inst);
    }
}

void ModuleIR::addInstructionRaw(const std::string& funcName, const std::string& op, const std::vector<IRValue>& operands, const std::string& result) {
    auto it = std::find_if(functions.begin(), functions.end(), [&](const IRFunction& f){ return f.name == funcName; });
    if (it != functions.end()) {
        IRInstruction inst;
        inst.op = op;
        inst.operands = operands;
        inst.result = result.empty() ? "r" + std::to_string(it->body.size()) : result;
        it->body.push_back(inst);
    }
}

void ModuleIR::setFunctionGlobals(const std::string& funcName,
                                   const std::vector<std::string>& globals) {
    auto it = std::find_if(functions.begin(), functions.end(),
                           [&](const IRFunction& f){ return f.name == funcName; });
    if (it != functions.end()) it->globalVars = globals;
}

void ModuleIR::addModuleGlobal(const std::string& name) {
    if (std::find(moduleGlobals.begin(), moduleGlobals.end(), name) == moduleGlobals.end())
        moduleGlobals.push_back(name);
}

} // namespace pyc
