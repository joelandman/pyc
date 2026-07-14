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
    std::vector<std::string> args;        // cleaned (no * markers)
    std::vector<IRInstruction> body;
    // Variables that should use module-level (global) storage in this function.
    std::vector<std::string> globalVars;
    // Original parameter names with * markers (for *vararg detection in adapters/forwarders).
    std::vector<std::string> paramNames;

    // B5 (cells/nonlocal):
    // Python-level names in this function whose storage is a cell (PyCell_New/Get/Set).
    // This includes names declared 'nonlocal' here, and names assigned here that
    // descendant nested functions access via 'nonlocal'.
    std::vector<std::string> cellVars;
    // For a nested function that uses cells from an enclosing scope, the Python names
    // of those cells. Lowering will synthesize hidden parameters for them; codegen
    // will receive them as leading PyObject* cell parameters.
    std::vector<std::string> freeCellVars;
    // Trailing default values for this function/lambda (module global slot names).
    // Used by __apply__ adapters for indirect calls when fewer args are supplied.
    std::vector<std::string> defaultGlobals;
    
    // A5: names that should use native i64 storage (proven numeric locals).
    // Codegen will allocate i64 alloca for these instead of PyObject* slots.
    std::vector<std::string> numericLocals;
};

 class ModuleIR {
 public:
     std::vector<IRFunction> functions;
     // All variable names that are stored at module scope (declared global anywhere).
     std::vector<std::string> moduleGlobals;
     // Module name for B7 import system (e.g., "utils", "main")
     std::string moduleName;

     void addFunction(const std::string& name, const std::vector<std::string>& args = {});
     void addInstruction(const std::string& funcName, const std::string& op, const std::vector<std::string>& operands, const std::string& result = "", const std::string& resultType = "boxed");
     void addInstructionRaw(const std::string& funcName, const std::string& op, const std::vector<IRValue>& operands, const std::string& result = "", const std::string& resultType = "boxed");
     void setFunctionGlobals(const std::string& funcName, const std::vector<std::string>& globals);
     void addModuleGlobal(const std::string& name);
};

} // namespace pyc
