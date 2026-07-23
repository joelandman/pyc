#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

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
    // A6: names that should use native f64 storage (proven float locals).
    // Codegen will allocate f64 alloca for these to enable native float chains.
    std::vector<std::string> numericFloatLocals;
    // Param types from call-site analysis. Each entry is "int", "float", or "" (unknown).
    // Populated by generateParamTypeAnalysis; used to allocate native param slots.
    std::vector<std::string> paramTypes;
    // Container element types: maps variable name → (index → element type at that index)
    // Element types: "float", "int", "boxed", "float_list", "int_list", "boxed_tuple"
    // - "float_list" = element is a float list (subscript returns "float")
    // - "float" = element is a primitive float
    // - "boxed" = element is generic/unknown
    // Key 0 is a wildcard meaning "any index"
    std::unordered_map<std::string, std::unordered_map<size_t, std::string>> containerElementTypes;
    // Subscript element types: maps variable name → (index → element type)
    // Simpler version of containerElementTypes, only tracks primitive element types
    // Used as a fallback when containerElementTypes has no matching entry
    std::unordered_map<std::string, std::unordered_map<size_t, std::string>> subscriptElementTypes;
    // Per-index element types for container variables from multi-element list construction
    // Records concrete element types for each index position even when elements have different types
    // Keys are container variable names; values map to a vector of element types at each index
    // Used to track per-index types for mixed-type containers and propagate through unpack/subscript
    std::unordered_map<std::string, std::vector<std::string>> listElementTypes;
    // Return types for user-defined functions: tracks what types each function returns.
    // Used by callers to infer return value types. Contains: "float", "int", "boxed", "list", "dict", etc.
    // If multiple different types are returned, stored as "boxed".
    std::string returnType;
    // Return element types: maps return value index → element type when function returns a list/tuple
    // Tracks per-index types (e.g., index 0 = float_list, index 1 = float_list for a list of float lists)
    std::unordered_map<size_t, std::string> returnContainerElementTypes;
    // Return subscript element types: maps return value index → element type for lists of known-type elements
    std::unordered_map<size_t, std::string> returnSubscriptElementTypes;
    // Call-site type info: for each function, tracks what types its arguments have at call sites.
    // This is populated during call-site analysis and used for return type propagation.
    std::unordered_map<std::string, std::vector<std::vector<std::string>>> callSiteArgTypes;
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
