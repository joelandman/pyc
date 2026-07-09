#include "pyc/IR.h"
#include <algorithm>

namespace pyc {

void ModuleIR::addFunction(const std::string& name, const std::vector<std::string>& args) {
    // If a function with this name already exists, don't add a duplicate.
    // (E.g. `main` is added once for module-level code, and again if a user
    //  shadows it with a `def main():` inside an `if __name__ == '__main__'`
    //  block.) Re-adding would corrupt the IR's arg list relative to the
    //  already-declared LLVM FunctionType.
    auto it = std::find_if(functions.begin(), functions.end(),
                          [&](const IRFunction& f){ return f.name == name; });
    if (it != functions.end()) return;
    IRFunction f;
    f.name = name;
    f.args = args;
    functions.push_back(f);
}

void ModuleIR::addInstruction(const std::string& funcName, const std::string& op, const std::vector<std::string>& operands, const std::string& result, const std::string& resultType) {
    auto it = std::find_if(functions.begin(), functions.end(), [&](const IRFunction& f){ return f.name == funcName; });
    if (it != functions.end()) {
        if (it->body.size() > 1000000) {
            std::fprintf(stderr, "ABORT: IR body for %s exceeded 1M instructions (likely infinite loop) op=%s\n", funcName.c_str(), op.c_str());
            std::abort();
        }
        IRInstruction inst;
        inst.op = op;
        for (const auto& o : operands) {
            inst.operands.push_back({o, "i32"});
        }
        inst.result = result.empty() ? "r" + std::to_string(it->body.size()) : result;
        inst.resultType = resultType;
        it->body.push_back(inst);
    }
}

void ModuleIR::addInstructionRaw(const std::string& funcName, const std::string& op, const std::vector<IRValue>& operands, const std::string& result, const std::string& resultType) {
    auto it = std::find_if(functions.begin(), functions.end(), [&](const IRFunction& f){ return f.name == funcName; });
    if (it != functions.end()) {
        IRInstruction inst;
        inst.op = op;
        inst.operands = operands;
        inst.result = result.empty() ? "r" + std::to_string(it->body.size()) : result;
        inst.resultType = resultType;
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
