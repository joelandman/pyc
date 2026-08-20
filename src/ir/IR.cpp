#include "pyc/IR.h"
#include <algorithm>
#include <cstdio>
#include <utility>

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

void ModuleIR::addInstruction(const std::string& funcName, const std::string& op, const std::vector<std::string>& operands, const std::string& result, const std::string& resultType, int lineno) {
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
        inst.lineno = (lineno != 0) ? lineno : currentLineno;
        it->body.push_back(inst);
    }
}

void ModuleIR::addInstructionRaw(const std::string& funcName, const std::string& op, const std::vector<IRValue>& operands, const std::string& result, const std::string& resultType, int lineno) {
    auto it = std::find_if(functions.begin(), functions.end(), [&](const IRFunction& f){ return f.name == funcName; });
    if (it != functions.end()) {
        IRInstruction inst;
        inst.op = op;
        inst.operands = operands;
        inst.result = result.empty() ? "r" + std::to_string(it->body.size()) : result;
        inst.resultType = resultType;
        inst.lineno = (lineno != 0) ? lineno : currentLineno;
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

static bool isCompilerTemp(const std::string& n) {
    if (n.empty()) return false;
    if (n[0] == '$') return true;
    if (n[0] == 'r' || n[0] == 't' || n[0] == 'c') {
        if (n.size() < 2) return false;
        for (size_t i = 1; i < n.size(); ++i)
            if (n[i] < '0' || n[i] > '9') return false;
        return true;
    }
    return false;
}

static bool isRuntimeName(const std::string& c) {
    return c.rfind("Py", 0) == 0 || c.rfind("Pyc_", 0) == 0 || c.rfind("pyc_", 0) == 0;
}

static bool isExtractCallee(const std::string& c) {
    return c.find("GetItem") != std::string::npos ||
           c.find("Unpack") != std::string::npos ||
           c == "Pyc_GetItem" || c == "PyObject_GetAttr";
}

static void markRetainedCallArgs(const IRInstruction& inst,
                                 std::unordered_set<std::string>& escaping,
                                 std::vector<std::pair<std::string, std::string>>& storeEdges,
                                 std::vector<std::pair<std::string, std::string>>& extractEdges) {
    if (inst.operands.empty()) return;
    const std::string& callee = inst.operands[0].name;
    auto markFrom = [&](size_t i0) {
        for (size_t i = i0; i < inst.operands.size(); ++i)
            if (!inst.operands[i].name.empty()) escaping.insert(inst.operands[i].name);
    };
    auto store = [&](const std::string& val, const std::string& container) {
        if (!val.empty() && !container.empty()) storeEdges.push_back({val, container});
    };
    if (callee == "Pyc_Apply" || callee == "pyc_raise" || callee == "pyc_reraise") {
        markFrom(1);
        return;
    }
    if (callee == "PyCell_New" || callee == "PyCell_Set") {
        if (inst.operands.size() >= 2) escaping.insert(inst.operands.back().name);
        return;
    }
    if (callee == "PyList_Append" && inst.operands.size() >= 3) {
        store(inst.operands.back().name, inst.operands[1].name);
        return;
    }
    if (callee.find("SetItem") != std::string::npos && inst.operands.size() >= 3) {
        store(inst.operands.back().name, inst.operands[1].name);
        if (callee.find("Dict") != std::string::npos && inst.operands.size() >= 4)
            store(inst.operands[inst.operands.size() - 2].name, inst.operands[1].name);
        return;
    }
    if (callee.find("SetAttr") != std::string::npos && inst.operands.size() >= 2) {
        escaping.insert(inst.operands.back().name);
        return;
    }
    if (isExtractCallee(callee) && inst.operands.size() >= 2 && !inst.result.empty()) {
        extractEdges.push_back({inst.operands[1].name, inst.result});
        return;
    }
    if (!isRuntimeName(callee)) markFrom(1);
}

static bool isGlobalName(const IRFunction& f, const ModuleIR& ir, const std::string& name) {
    if (std::find(ir.moduleGlobals.begin(), ir.moduleGlobals.end(), name) != ir.moduleGlobals.end())
        return true;
    return std::find(f.globalVars.begin(), f.globalVars.end(), name) != f.globalVars.end();
}

void analyzeEscapes(ModuleIR& ir, bool dump) {
    for (auto& f : ir.functions) {
        f.escapingValues.clear();
        f.nonEscapingTemps.clear();
        std::unordered_set<std::string> definedTemps;
        std::unordered_set<std::string> escaping;
        std::vector<std::pair<std::string, std::string>> assignEdges;
        std::vector<std::pair<std::string, std::string>> storeEdges;
        std::vector<std::pair<std::string, std::string>> extractEdges;

        for (const auto& a : f.args) escaping.insert(a);

        for (const auto& inst : f.body) {
            if (!inst.result.empty() && inst.op != "label" && inst.op != "br" &&
                inst.op != "try_begin" && inst.op != "try_end") {
                if (isCompilerTemp(inst.result)) definedTemps.insert(inst.result);
            }
            if (inst.op == "ret") {
                for (const auto& o : inst.operands)
                    if (!o.name.empty()) escaping.insert(o.name);
            } else if (inst.op == "call") {
                markRetainedCallArgs(inst, escaping, storeEdges, extractEdges);
            } else if (inst.op == "list") {
                for (const auto& o : inst.operands)
                    if (!o.name.empty() && !inst.result.empty())
                        storeEdges.push_back({o.name, inst.result});
            } else if (inst.op == "subscript" && inst.operands.size() >= 1 && !inst.result.empty()) {
                extractEdges.push_back({inst.operands[0].name, inst.result});
            } else if (inst.op == "assign") {
                if (!inst.operands.empty() && !inst.result.empty()) {
                    assignEdges.push_back({inst.operands[0].name, inst.result});
                    if (isGlobalName(f, ir, inst.result))
                        escaping.insert(inst.operands[0].name);
                }
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& e : assignEdges) {
                if (escaping.count(e.second) && escaping.insert(e.first).second)
                    changed = true;
            }
            for (const auto& e : extractEdges) {
                if (escaping.count(e.second) && escaping.insert(e.first).second)
                    changed = true;
            }
            for (const auto& e : storeEdges) {
                if (escaping.count(e.second) && escaping.insert(e.first).second)
                    changed = true;
            }
        }

        f.escapingValues = std::move(escaping);
        for (const auto& t : definedTemps)
            if (!f.escapingValues.count(t)) f.nonEscapingTemps.insert(t);

        if (dump) {
            std::fprintf(stderr, "[escape] func=%s temps=%zu escape=%zu keep=%zu\n",
                         f.name.c_str(), definedTemps.size(),
                         definedTemps.size() - f.nonEscapingTemps.size(),
                         f.nonEscapingTemps.size());
            size_t shown = 0;
            for (const auto& t : f.nonEscapingTemps) {
                if (shown == 0) std::fprintf(stderr, "[escape]   keep:");
                std::fprintf(stderr, " %s", t.c_str());
                if (++shown >= 24) break;
            }
            if (shown) std::fprintf(stderr, "%s\n",
                f.nonEscapingTemps.size() > shown ? " ..." : "");
        }
    }
}

} // namespace pyc
