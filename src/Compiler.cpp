#include "pyc/Compiler.h"
#include "pyc/PythonParser.h"
#include "pyc/IR.h"
#include "pyc/Codegen.h"
#include <llvm/IR/LLVMContext.h>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace pyc {

class LoweringVisitor {
public:
    LoweringVisitor(ModuleIR& moduleIR) : ir(moduleIR) {}

    void lower(const ASTNode* node, const std::string& funcName = "") {
        if (!node) return;
        if (!funcName.empty()) currentFunc = funcName;

        if (node->type == "Module") {
            ir.addFunction("main", {});
            currentFunc = "main";
            tempCounter = 0;
            // Pre-scan: collect all global-declared variable names so main and
            // functions can share module-level storage for those variables.
            collectGlobalDecls(node);
            // main inherits all module globals (top-level code IS the module scope).
            ir.setFunctionGlobals("main", ir.moduleGlobals);
            for (const auto& c : node->children) {
                lower(c.get());
            }
        } else if (node->type == "FunctionDef") {
            std::string saved = currentFunc;
            ir.addFunction(node->id, node->args);
            funcParamNames[node->id] = node->args;

            // Collect this function's global declarations (scan body before lowering).
            std::vector<std::string> funcGlobals = scanFuncGlobals(node);
            // Remove names that are also parameters (params shadow globals).
            for (const auto& p : node->args) {
                funcGlobals.erase(std::remove(funcGlobals.begin(), funcGlobals.end(), p),
                                  funcGlobals.end());
            }
            ir.setFunctionGlobals(node->id, funcGlobals);

            // Count defaults and collect their values
            std::vector<std::string> defaults;
            for (const auto& c : node->children) {
                if (c && c->type == "Default") {
                    std::string defVal = lowerExpr(c.get());
                    defaults.push_back(defVal);
                }
            }
            if (!defaults.empty()) {
                funcDefaultCount[node->id] = defaults.size();
                funcDefaultValues[node->id] = defaults;
            }

            currentFunc = node->id;
            tempCounter = 0;
            for (const auto& c : node->children) {
                lower(c.get());
            }
            currentFunc = saved;   // restore context for siblings (important for top-level code after defs)
        } else if (node->type == "If") {
            lowerIf(node);
        } else if (node->type == "While") {
            lowerWhile(node);
        } else if (node->type == "For") {
            lowerFor(node);
        } else if (node->type == "Break") {
            ir.addInstruction(currentFunc, "br", {}, loopBreakLabel);
        } else if (node->type == "Continue") {
            ir.addInstruction(currentFunc, "br", {}, loopContinueLabel);
        } else if (node->type == "Global") {
            // Declaration only — already collected in pre-scan, no IR emitted.
            return;
        } else if (node->type == "AugAssign") {
            lowerAugAssign(node);
        } else if (node->type == "Assign") {
            lowerAssign(node);
        } else if (node->type == "Return") {
            lowerReturn(node);
        } else if (node->type == "Expr") {
            if (!node->children.empty() && node->children[0]) {
                lowerExpr(node->children[0].get());
            }
        } else if (node->type == "ListComp") {
            lowerListComp(node);
        } else if (node->type == "DictComp") {
            lowerDictComp(node);
        } else {
            // expressions or fallthrough
            lowerExpr(node);
            for (const auto& c : node->children) {
                if (c) lower(c.get());
            }
        }
    }

    std::string lowerExpr(const ASTNode* node) {
        if (!node || currentFunc.empty()) return "";
        if (node->type == "FunctionDef") return "";
        if (!node || currentFunc.empty()) return "";
        if (node->type == "FunctionDef") return "";

        if (node->type == "Constant") {
            std::string res = "c" + std::to_string(tempCounter++);
            std::string val = node->value;
            if (node->is_bool) {
                ir.addInstruction(currentFunc, "bconst", {val}, res);
            } else if (node->is_float) {
                ir.addInstruction(currentFunc, "fconst", {val}, res);
            } else if (node->is_str) {
                // Wrap in quotes so codegen detects it as a string.
                // Embedded quotes are not escaped in this MVP.
                ir.addInstruction(currentFunc, "const", {"\"" + val + "\""}, res);
            } else {
                ir.addInstruction(currentFunc, "const", {val}, res);
            }
            return res;
        } else if (node->type == "Name") {
            return node->id;
        } else if (node->type == "Attribute") {
            return lowerAttribute(node);
        } else if (node->type == "BinOp") {
            return lowerBinOp(node);
        } else if (node->type == "Call") {
            return lowerCall(node);
        } else if (node->type == "Compare") {
            return lowerCompare(node);
        } else if (node->type == "List" || node->type == "Tuple") {
            return lowerList(node);
        } else if (node->type == "Dict") {
            return lowerDict(node);
        } else if (node->type == "Default") {
            // Defaults are handled specially in FunctionDef lowering
            return lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        } else if (node->type == "Return") {
            return lowerReturnExpr(node);
        } else if (node->type == "JoinedStr") {
            return lowerJoinedStr(node);
        } else if (node->type == "FormattedValue") {
            return lowerFormattedValue(node);
        } else if (node->type == "BoolOp") {
            return lowerBoolOp(node);
        } else if (node->type == "UnaryOp") {
            return lowerUnaryOp(node);
        } else if (node->type == "ListComp") {
            return lowerListComp(node);
        } else if (node->type == "Subscript") {
            return lowerSubscriptGet(node);
        } else if (node->type == "IfExp") {
            return lowerIfExpr(node);
        }
        return "";
    }

private:
    ModuleIR& ir;
    std::string currentFunc;
    int tempCounter = 0;
    // Current innermost loop labels — updated by lowerFor/lowerWhile so
    // break/continue target the right blocks even with nested loops.
    std::string loopContinueLabel;
    std::string loopBreakLabel;
    std::unordered_map<std::string, int> funcDefaultCount;
    std::unordered_map<std::string, std::vector<std::string>> funcDefaultValues;
    std::unordered_map<std::string, std::vector<std::string>> funcParamNames;

    // Recursively collect all names from `global` statements in the subtree.
    void collectGlobalDecls(const ASTNode* node) {
        if (!node) return;
        if (node->type == "Global") {
            for (const auto& name : node->args) ir.addModuleGlobal(name);
        }
        for (const auto& c : node->children) collectGlobalDecls(c.get());
    }

    // Collect names from `global` statements that are direct descendants of a FunctionDef.
    std::vector<std::string> scanFuncGlobals(const ASTNode* funcNode) {
        std::vector<std::string> result;
        for (const auto& c : funcNode->children) {
            if (c && c->type == "Global") {
                for (const auto& name : c->args) result.push_back(name);
            }
        }
        return result;
    }

    std::string lowerBinOp(const ASTNode* node) {
        std::string left = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string right = lowerExpr(node->children.size() > 1 ? node->children[1].get() : nullptr);
        std::string res = "t" + std::to_string(tempCounter++);
        std::string op = node->op.empty() ? "add" : node->op;
        if (op == "Add") op = "add";
        else if (op == "Sub") op = "sub";
        else if (op == "Mult") op = "mul";
        else if (op == "FloorDiv") op = "div";
        else if (op == "Div") op = "truediv";
        else if (op == "Mod") op = "mod";
        else if (op == "Pow") op = "pow";
        ir.addInstruction(currentFunc, op, {left, right}, res);
        return res;
    }

    std::string lowerCall(const ASTNode* node) {
        // Method call: obj.method(args) — func is an Attribute node
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Attribute") {
            return lowerMethodCall(node);
        }
        std::string funcName = (node->children.empty() || !node->children[0]) ? "" : node->children[0]->id;
        std::vector<std::string> argRes;
        std::vector<std::pair<std::string, std::string>> kwArgs; // (name, value)

        for (size_t i = 1; i < node->children.size(); ++i) {
            if (!node->children[i]) continue;
            if (node->children[i]->type == "Keyword") {
                if (!node->children[i]->children.empty()) {
                    std::string val = lowerExpr(node->children[i]->children[0].get());
                    kwArgs.emplace_back(node->children[i]->id, val);
                }
            } else {
                argRes.push_back(lowerExpr(node->children[i].get()));
            }
        }

        // Handle keyword arguments by mapping to parameter positions
        if (!kwArgs.empty()) {
            auto pit = funcParamNames.find(funcName);
            if (pit != funcParamNames.end()) {
                const auto& params = pit->second;
                for (auto& kw : kwArgs) {
                    for (size_t j = 0; j < params.size(); ++j) {
                        if (params[j] == kw.first) {
                            if (argRes.size() <= j) argRes.resize(j + 1);
                            argRes[j] = kw.second;
                            break;
                        }
                    }
                }
            } else {
                // Fallback: append keyword values
                for (auto& kw : kwArgs) argRes.push_back(kw.second);
            }
        }
        // Simple default injection: if no args provided and function has defaults, use them
        // Only use defaults when we have no positional args AND no keyword args
        if (argRes.empty() && kwArgs.empty()) {
            auto it = funcDefaultValues.find(funcName);
            if (it != funcDefaultValues.end()) {
                argRes = it->second;
            }
        }

        // print() with no args → bare newline
        if (funcName == "print" && argRes.empty() && kwArgs.empty()) {
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_PrintNewline"}, res);
            return res;
        }

        // print(a, b, c, ...) → build space-separated string, then single-arg print
        if (funcName == "print" && argRes.size() > 1) {
            // Convert first arg to its string representation
            std::string acc = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyStr_FromAny", argRes[0]}, acc);
            for (size_t i = 1; i < argRes.size(); ++i) {
                std::string sp = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\" \""}, sp);
                std::string withSep = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyString_Concat", acc, sp}, withSep);
                std::string argStr = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyStr_FromAny", argRes[i]}, argStr);
                acc = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyString_Concat", withSep, argStr}, acc);
            }
            // Route through the normal single-arg print path in codegen
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"print", acc}, res);
            return res;
        }

        // len(obj) → PyBuiltin_Len(obj)
        if (funcName == "len") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Len", arg}, res);
            return res;
        }

        // min/max — fold pairwise; single list arg uses list variant
        if (funcName == "min" || funcName == "max") {
            std::string fn2  = (funcName == "min") ? "PyBuiltin_Min2"    : "PyBuiltin_Max2";
            std::string fnLst = (funcName == "min") ? "PyBuiltin_MinList" : "PyBuiltin_MaxList";
            if (argRes.size() == 1) {
                std::string res = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {fnLst, argRes[0]}, res);
                return res;
            }
            std::string acc = argRes[0];
            for (size_t i = 1; i < argRes.size(); ++i) {
                std::string res2 = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {fn2, acc, argRes[i]}, res2);
                acc = res2;
            }
            return acc;
        }
        // list(x) → PyBuiltin_List(x)
        if (funcName == "list") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            if (argRes.empty()) {
                std::string sc = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, sc);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sc}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", arg}, res);
            }
            return res;
        }
        // enumerate(iterable) → PyBuiltin_Enumerate(iterable)
        if (funcName == "enumerate") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Enumerate", arg}, res);
            return res;
        }
        // zip(a, b) → PyBuiltin_Zip2(a, b)
        if (funcName == "zip" && argRes.size() >= 2) {
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Zip2", argRes[0], argRes[1]}, res);
            return res;
        }

        // int(x) → PyBuiltin_Int(x)
        if (funcName == "int") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Int", arg}, res);
            return res;
        }
        // float(x) → PyBuiltin_Float(x)
        if (funcName == "float") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Float", arg}, res);
            return res;
        }
        // abs(x) → PyBuiltin_Abs(x)
        if (funcName == "abs") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Abs", arg}, res);
            return res;
        }

        // str(obj) → PyStr_FromAny(obj)
        if (funcName == "str") {
            if (argRes.empty()) {
                std::string res = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"\""}, res);
                return res;
            }
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyStr_FromAny", argRes[0]}, res);
            return res;
        }

        // Normalize range(stop), range(start,stop), range(start,stop,step)
        // → always call PyBuiltin_Range(start, stop, step) with 3 PyObject* args
        if (funcName == "range") {
            std::string startRes, stopRes, stepRes;
            if (argRes.size() == 1) {
                startRes = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, startRes);
                stopRes  = argRes[0];
                stepRes  = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"1"}, stepRes);
            } else if (argRes.size() == 2) {
                startRes = argRes[0];
                stopRes  = argRes[1];
                stepRes  = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"1"}, stepRes);
            } else if (argRes.size() >= 3) {
                startRes = argRes[0];
                stopRes  = argRes[1];
                stepRes  = argRes[2];
            } else {
                // range() with no args → empty list
                startRes = stopRes = stepRes = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, startRes);
            }
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call",
                              {"PyBuiltin_Range", startRes, stopRes, stepRes}, res);
            return res;
        }

        std::string res = "t" + std::to_string(tempCounter++);
        std::vector<std::string> ops = {funcName};
        ops.insert(ops.end(), argRes.begin(), argRes.end());
        ir.addInstruction(currentFunc, "call", ops, res);
        return res;
    }

    std::string lowerCompare(const ASTNode* node) {
        if (node->children.empty()) return "";
        // Evaluate all operands exactly once: children[0]=left, children[1..n]=comparators
        std::vector<std::string> operands;
        for (const auto& c : node->children)
            operands.push_back(lowerExpr(c.get()));

        const auto& ops = node->args;   // all op names, populated by parser
        if (ops.empty()) return "";

        // Helper: emit one pairwise comparison, return result name
        auto emitPair = [&](const std::string& opName,
                             const std::string& lhs, const std::string& rhs) {
            std::string r = "t" + std::to_string(tempCounter++);
            if (opName == "In") {
                ir.addInstruction(currentFunc, "call", {"Pyc_Contains", rhs, lhs}, r);
            } else if (opName == "NotIn") {
                std::string c2 = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_Contains", rhs, lhs}, c2);
                ir.addInstruction(currentFunc, "call", {"PyObject_Not", c2}, r);
            } else if (opName == "Is") {
                // identity: PyObject_CompareBool with Eq (pointer eq handled there)
                ir.addInstruction(currentFunc, "icmp", {"Eq", lhs, rhs}, r);
            } else if (opName == "IsNot") {
                ir.addInstruction(currentFunc, "icmp", {"NotEq", lhs, rhs}, r);
            } else {
                ir.addInstruction(currentFunc, "icmp", {opName, lhs, rhs}, r);
            }
            return r;
        };

        // Single comparison — common fast path
        if (ops.size() == 1 && operands.size() >= 2)
            return emitPair(ops[0], operands[0], operands[1]);

        // Chained: (a op0 b) and (b op1 c) and ... — short-circuit like BoolOp
        int bc = tempCounter++;
        std::string resultVar = "chain_r_"   + std::to_string(bc);
        std::string endLabel  = "chain_end_" + std::to_string(bc);

        std::string first = emitPair(ops[0], operands[0], operands[1]);
        ir.addInstruction(currentFunc, "assign", {first}, resultVar);

        for (size_t i = 1; i < ops.size() && i + 1 < operands.size(); ++i) {
            std::string rhsL = "chain_rhs_" + std::to_string(bc) + "_" + std::to_string(i);
            std::string truth = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyObject_TruthBoxed", resultVar}, truth);
            ir.addInstruction(currentFunc, "br", {truth, rhsL, endLabel});
            ir.addInstruction(currentFunc, "label", {}, rhsL);
            std::string pairRes = emitPair(ops[i], operands[i], operands[i + 1]);
            ir.addInstruction(currentFunc, "assign", {pairRes}, resultVar);
        }
        ir.addInstruction(currentFunc, "label", {}, endLabel);
        return resultVar;
    }

    void lowerIf(const ASTNode* node) {
        int c = tempCounter++;
        std::string thenL = "if_then_" + std::to_string(c);
        std::string elseL = "if_else_" + std::to_string(c);
        std::string endL  = "if_end_"  + std::to_string(c);

        std::string cond = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        ir.addInstruction(currentFunc, "br", {cond, thenL, elseL});
        ir.addInstruction(currentFunc, "label", {}, thenL);

        // node->value = number of then-body statements (set in parser)
        size_t bodyCount = node->value.empty() ? 0 : (size_t)std::stoi(node->value);
        size_t n = node->children.size();
        for (size_t i = 1; i <= bodyCount && i < n; ++i)
            lower(node->children[i].get());

        ir.addInstruction(currentFunc, "br", {}, endL);
        ir.addInstruction(currentFunc, "label", {}, elseL);

        for (size_t i = 1 + bodyCount; i < n; ++i)
            lower(node->children[i].get());

        ir.addInstruction(currentFunc, "label", {}, endL);
    }

    void lowerWhile(const ASTNode* node) {
        int c = tempCounter++;
        std::string loopL = "while_loop_" + std::to_string(c);
        std::string bodyL = "while_body_" + std::to_string(c);
        std::string exitL = "while_exit_" + std::to_string(c);

        std::string savedCont = loopContinueLabel, savedBreak = loopBreakLabel;
        loopContinueLabel = loopL;
        loopBreakLabel    = exitL;

        ir.addInstruction(currentFunc, "label", {}, loopL);
        std::string cond = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        ir.addInstruction(currentFunc, "br", {cond, bodyL, exitL});
        ir.addInstruction(currentFunc, "label", {}, bodyL);
        for (size_t i = 1; i < node->children.size(); ++i)
            lower(node->children[i].get());
        ir.addInstruction(currentFunc, "br", {}, loopL);
        ir.addInstruction(currentFunc, "label", {}, exitL);

        loopContinueLabel = savedCont;
        loopBreakLabel    = savedBreak;
    }

    void lowerFor(const ASTNode* node) {
        // For AST layout (from buildAST):
        //   node->id        = target variable name  (e.g. "i")
        //   children[0]     = iter expression       (e.g. Call(range,5))
        //   children[1..n]  = body statements
        if (node->children.empty()) return;
        std::string listVal = lowerExpr(node->children[0].get());  // iter

        // Boxed length: PyList_SizeBoxed returns PyObject*(int)
        std::string lenRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", listVal}, lenRes);

        // Use a fresh temp for the initial 0 to avoid name collision with the alloca.
        std::string idxVar  = node->id + "__idx";     // alloca variable name
        std::string idxInit = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, idxInit);
        ir.addInstruction(currentFunc, "assign", {idxInit}, idxVar);

        std::string loopLabel = "for_loop_" + std::to_string(tempCounter);
        std::string bodyLabel = "for_body_" + std::to_string(tempCounter);
        std::string exitLabel = "for_exit_" + std::to_string(tempCounter);
        tempCounter++;

        std::string savedCont = loopContinueLabel, savedBreak = loopBreakLabel;
        loopContinueLabel = loopLabel;
        loopBreakLabel    = exitLabel;

        ir.addInstruction(currentFunc, "label", {}, loopLabel);
        std::string cmpRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "icmp", {"Lt", idxVar, lenRes}, cmpRes);
        ir.addInstruction(currentFunc, "br", {cmpRes, bodyLabel, exitLabel});

        ir.addInstruction(currentFunc, "label", {}, bodyLabel);
        std::string itemRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", listVal, idxVar}, itemRes);
        if (node->id == "__unpack__") {
            // for a, b in iterable: unpack each item into multiple variables
            for (size_t j = 0; j < node->args.size(); ++j) {
                std::string ic = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(j)}, ic);
                std::string elem = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", itemRes, ic}, elem);
                ir.addInstruction(currentFunc, "assign", {elem}, node->args[j]);
            }
        } else {
            ir.addInstruction(currentFunc, "assign", {itemRes}, node->id);
        }

        for (size_t i = 1; i < node->children.size(); ++i)  // body starts at children[1]
            lower(node->children[i].get());

        // idxVar = idxVar + 1
        std::string oneRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"1"}, oneRes);
        std::string nextIdx = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "add", {idxVar, oneRes}, nextIdx);
        ir.addInstruction(currentFunc, "assign", {nextIdx}, idxVar);

        ir.addInstruction(currentFunc, "br", {}, loopLabel);
        ir.addInstruction(currentFunc, "label", {}, exitLabel);

        loopContinueLabel = savedCont;
        loopBreakLabel    = savedBreak;
    }

    std::string lowerList(const ASTNode* node) {
        size_t n = node->children.size();
        // Box size as PyObject* so PyList_NewBoxed receives a proper int.
        std::string sizeConst = "c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
        std::string listRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sizeConst}, listRes);

        for (size_t i = 0; i < n; ++i) {
            std::string elem = lowerExpr(node->children[i].get());
            std::string idxConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idxConst);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", listRes, idxConst, elem}, "");
        }
        return listRes;
    }

    std::string lowerDict(const ASTNode* node) {
        std::string dictRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyDict_New"}, dictRes);

        for (size_t i = 0; i + 1 < node->children.size(); i += 2) {
            std::string key = lowerExpr(node->children[i].get());
            std::string val = lowerExpr(node->children[i+1].get());
            ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", dictRes, key, val}, "");
        }
        return dictRes;
    }

    std::string lowerAttribute(const ASTNode* node) {
        std::string obj = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string res = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "getattr", {obj, node->id}, res);
        return res;
    }

    void lowerAssign(const ASTNode* node) {
        // Multi-target: a = b = val — args holds all target names
        if (!node->args.empty()) {
            std::string val = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
            for (const auto& name : node->args)
                ir.addInstruction(currentFunc, "assign", {val}, name);
            return;
        }
        if (node->id == "__subscript__") {
            if (node->children.size() < 2) return;
            const ASTNode* sub = node->children[0].get();   // Subscript node
            std::string obj = lowerExpr(sub->children.size() > 0 ? sub->children[0].get() : nullptr);
            std::string idx = lowerExpr(sub->children.size() > 1 ? sub->children[1].get() : nullptr);
            std::string val = lowerExpr(node->children[1].get());
            std::string dummy = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", obj, idx, val}, dummy);
            return;
        }
        if (node->id == "__unpack__") {
            if (node->children.size() < 2) return;
            const ASTNode* tupleTgt = node->children[0].get();  // Tuple/List of Name nodes
            std::string rhs = lowerExpr(node->children[1].get());
            for (size_t i = 0; i < tupleTgt->children.size(); ++i) {
                const ASTNode* nm = tupleTgt->children[i].get();
                if (!nm || nm->id.empty()) continue;
                std::string ic = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i)}, ic);
                std::string elem = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", rhs, ic}, elem);
                ir.addInstruction(currentFunc, "assign", {elem}, nm->id);
            }
            return;
        }
        if (!node->children.empty() && node->children[0]) {
            std::string val = lowerExpr(node->children[0].get());
            ir.addInstruction(currentFunc, "assign", {val}, node->id);
        }
    }

    void lowerAugAssign(const ASTNode* node) {
        if (node->children.empty()) return;
        std::string op = node->op;
        if      (op == "Add")      op = "add";
        else if (op == "Sub")      op = "sub";
        else if (op == "Mult")     op = "mul";
        else if (op == "FloorDiv") op = "div";
        else if (op == "Div")      op = "truediv";
        else if (op == "Mod")      op = "mod";
        else if (op == "Pow")      op = "pow";
        else                       op = "add";

        if (node->id == "__subscript__") {
            // a[i] op= val — children[0]=Subscript, children[1]=rhs
            if (node->children.size() < 2) return;
            const ASTNode* sub = node->children[0].get();
            std::string obj = lowerExpr(sub->children.size() > 0 ? sub->children[0].get() : nullptr);
            std::string idx = lowerExpr(sub->children.size() > 1 ? sub->children[1].get() : nullptr);
            std::string rhs = lowerExpr(node->children[1].get());
            std::string cur = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, idx}, cur);
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, op, {cur, rhs}, res);
            std::string dummy = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", obj, idx, res}, dummy);
        } else {
            // Normal name: children[0] = rhs
            std::string rhs = lowerExpr(node->children[0].get());
            std::string result = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, op, {node->id, rhs}, result);
            ir.addInstruction(currentFunc, "assign", {result}, node->id);
        }
    }

    std::string lowerSubscriptGet(const ASTNode* node) {
        // Subscript node: children[0]=object, children[1]=slice/index
        std::string obj = lowerExpr(node->children.size() > 0 ? node->children[0].get() : nullptr);
        std::string idx = lowerExpr(node->children.size() > 1 ? node->children[1].get() : nullptr);
        std::string res = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, idx}, res);
        return res;
    }

    std::string lowerReturnExpr(const ASTNode* node) {
        std::string val = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        ir.addInstruction(currentFunc, "ret", {val}, val);
        return val;
    }

    void lowerReturn(const ASTNode* node) {
        lowerReturnExpr(node);
    }

    // obj.method(args) dispatch
    std::string lowerMethodCall(const ASTNode* node) {
        // node->children[0] = Attribute(obj, method_name)
        // node->children[1..] = positional args
        const ASTNode* attr = node->children[0].get();
        std::string methodName = attr->id;
        std::string obj = lowerExpr(attr->children.empty() ? nullptr : attr->children[0].get());

        std::vector<std::string> args;
        for (size_t i = 1; i < node->children.size(); ++i) {
            if (node->children[i] && node->children[i]->type != "Keyword")
                args.push_back(lowerExpr(node->children[i].get()));
        }

        std::string res = "t" + std::to_string(tempCounter++);

        // Known list methods
        if (methodName == "append") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyList_Append", obj, arg}, res);
        // Known string methods
        } else if (methodName == "upper") {
            ir.addInstruction(currentFunc, "call", {"PyString_Upper", obj}, res);
        } else if (methodName == "lower") {
            ir.addInstruction(currentFunc, "call", {"PyString_Lower", obj}, res);
        } else if (methodName == "strip") {
            ir.addInstruction(currentFunc, "call", {"PyString_Strip", obj}, res);
        } else if (methodName == "split") {
            if (args.empty()) {
                ir.addInstruction(currentFunc, "call", {"PyString_SplitWhitespace", obj}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyString_Split", obj, args[0]}, res);
            }
        } else if (methodName == "join") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyString_Join", obj, arg}, res);
        // Dict methods
        } else if (methodName == "keys") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Keys", obj}, res);
        } else if (methodName == "values") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Values", obj}, res);
        } else if (methodName == "items") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Items", obj}, res);
        // List methods
        } else if (methodName == "sort") {
            ir.addInstruction(currentFunc, "call", {"PyList_Sort", obj}, res);
        } else if (methodName == "pop") {
            ir.addInstruction(currentFunc, "call", {"PyList_Pop", obj}, res);
        } else {
            // Unknown method — return None
            ir.addInstruction(currentFunc, "const", {"0"}, res);
        }
        return res;
    }

    // x if cond else y  — IfExp (ternary)
    std::string lowerIfExpr(const ASTNode* node) {
        if (node->children.size() < 3) return "";
        int c = tempCounter++;
        std::string resultVar = "ifexp_r_"    + std::to_string(c);
        std::string thenL     = "ifexp_then_" + std::to_string(c);
        std::string elseL     = "ifexp_else_" + std::to_string(c);
        std::string endL      = "ifexp_end_"  + std::to_string(c);

        // Pre-create the result alloca before the branch so both branches can store to it.
        std::string initVal = "c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, initVal);
        ir.addInstruction(currentFunc, "assign", {initVal}, resultVar);

        std::string cond = lowerExpr(node->children[0].get());
        ir.addInstruction(currentFunc, "br", {cond, thenL, elseL});

        ir.addInstruction(currentFunc, "label", {}, thenL);
        std::string tv = lowerExpr(node->children[1].get());
        ir.addInstruction(currentFunc, "assign", {tv}, resultVar);
        ir.addInstruction(currentFunc, "br", {}, endL);

        ir.addInstruction(currentFunc, "label", {}, elseL);
        std::string ev = lowerExpr(node->children[2].get());
        ir.addInstruction(currentFunc, "assign", {ev}, resultVar);

        ir.addInstruction(currentFunc, "label", {}, endL);
        return resultVar;
    }

    // Short-circuit boolean operator (and / or) with N values.
    // Produces a result alloca variable; codegen loads it for the caller.
    std::string lowerBoolOp(const ASTNode* node) {
        if (node->children.empty()) return "";
        bool isAnd = (node->op == "And");

        // Reserve a single counter for all labels of this boolop instance.
        int bc = tempCounter++;
        std::string resultVar = "boolop_r_"   + std::to_string(bc);
        std::string endLabel  = "boolop_end_" + std::to_string(bc);

        // Evaluate first value; store as initial result.
        std::string firstVal = lowerExpr(node->children[0].get());
        ir.addInstruction(currentFunc, "assign", {firstVal}, resultVar);

        for (size_t i = 1; i < node->children.size(); ++i) {
            std::string rhsLabel = "boolop_rhs_" + std::to_string(bc)
                                   + "_" + std::to_string(i);

            // Box truthiness so the br handler can unbox it.
            std::string truthVal = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call",
                              {"PyObject_TruthBoxed", resultVar}, truthVal);

            // AND: truthy → keep evaluating; OR: truthy → done
            if (isAnd)
                ir.addInstruction(currentFunc, "br", {truthVal, rhsLabel, endLabel});
            else
                ir.addInstruction(currentFunc, "br", {truthVal, endLabel, rhsLabel});

            ir.addInstruction(currentFunc, "label", {}, rhsLabel);
            std::string nextVal = lowerExpr(node->children[i].get());
            ir.addInstruction(currentFunc, "assign", {nextVal}, resultVar);
            // Codegen inserts fallthrough br to endLabel at the next label instruction.
        }

        ir.addInstruction(currentFunc, "label", {}, endLabel);
        return resultVar;
    }

    std::string lowerUnaryOp(const ASTNode* node) {
        std::string val = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        if (node->op == "UAdd") return val;   // identity

        std::string res = "t" + std::to_string(tempCounter++);
        if (node->op == "Not")
            ir.addInstruction(currentFunc, "call", {"PyObject_Not",  val}, res);
        else if (node->op == "USub")
            ir.addInstruction(currentFunc, "call", {"PyNumber_Negate", val}, res);
        else
            ir.addInstruction(currentFunc, "const", {"0"}, res);   // unknown → 0
        return res;
    }

    // Lower a single part of a JoinedStr: Constant (string literal) or FormattedValue
    std::string lowerFStrPart(const ASTNode* node) {
        if (node->type == "FormattedValue") return lowerFormattedValue(node);
        return lowerExpr(node);  // Constant string part
    }

    std::string lowerFormattedValue(const ASTNode* node) {
        std::string exprVal = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string res = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyStr_FromAny", exprVal}, res);
        return res;
    }

    std::string lowerJoinedStr(const ASTNode* node) {
        if (node->children.empty()) {
            std::string res = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"\""}, res);
            return res;
        }
        std::string acc = lowerFStrPart(node->children[0].get());
        for (size_t i = 1; i < node->children.size(); ++i) {
            std::string part = lowerFStrPart(node->children[i].get());
            std::string newAcc = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyString_Concat", acc, part}, newAcc);
            acc = newAcc;
        }
        return acc;
    }

    std::string lowerListComp(const ASTNode* node) {
        // [elt for target in iter if cond ...]
        // children[0] = elt expression
        // children[1] = comprehension generator node
        if (node->children.size() < 2) return "";
        const ASTNode* eltNode = node->children[0].get();
        const ASTNode* genNode = node->children[1].get();
        if (!genNode || genNode->type != "comprehension" || genNode->children.size() < 2)
            return "";

        // Create result list
        std::string sc = "c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, sc);
        std::string listVar = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sc}, listVar);

        // Evaluate iterator once
        std::string iterVal = lowerExpr(genNode->children[1].get());
        std::string lenRes  = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", iterVal}, lenRes);

        // Index variable (unique name to avoid clashes)
        std::string idxVar  = "lc_i_" + std::to_string(tempCounter++);
        std::string idxInit = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, idxInit);
        ir.addInstruction(currentFunc, "assign", {idxInit}, idxVar);

        int lc = tempCounter++;
        std::string loopL = "lc_lp_" + std::to_string(lc);
        std::string bodyL = "lc_bd_" + std::to_string(lc);
        std::string contL = "lc_ct_" + std::to_string(lc);
        std::string exitL = "lc_ex_" + std::to_string(lc);

        ir.addInstruction(currentFunc, "label", {}, loopL);
        std::string cmpR = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "icmp", {"Lt", idxVar, lenRes}, cmpR);
        ir.addInstruction(currentFunc, "br", {cmpR, bodyL, exitL});

        ir.addInstruction(currentFunc, "label", {}, bodyL);
        std::string item = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", iterVal, idxVar}, item);
        ir.addInstruction(currentFunc, "assign", {item}, genNode->children[0]->id);

        // Conditions: if any is false, jump to contL (skip append)
        for (size_t ci = 2; ci < genNode->children.size(); ++ci) {
            std::string trueL = "lc_ci_" + std::to_string(tempCounter++);
            std::string condV = lowerExpr(genNode->children[ci].get());
            ir.addInstruction(currentFunc, "br", {condV, trueL, contL});
            ir.addInstruction(currentFunc, "label", {}, trueL);
        }

        // Evaluate element in loop context and append
        std::string eltVal = lowerExpr(eltNode);
        ir.addInstruction(currentFunc, "call", {"PyList_Append", listVar, eltVal}, "");

        // Fall through to contL
        ir.addInstruction(currentFunc, "label", {}, contL);
        std::string one = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"1"}, one);
        std::string nxt = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "add", {idxVar, one}, nxt);
        ir.addInstruction(currentFunc, "assign", {nxt}, idxVar);
        ir.addInstruction(currentFunc, "br", {}, loopL);
        ir.addInstruction(currentFunc, "label", {}, exitL);
        return listVar;
    }

    void lowerDictComp(const ASTNode* node) {
        // Dict comprehension structure: {key: value for target in iter if ifs}
        if (node->children.size() < 3) return;
        
        // Create a temporary dict variable for the result
        std::string dictVar = "dict_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"dict_create"}, dictVar);
        
        // Get the key and value expressions
        const ASTNode* keyNode = node->children[0].get();
        const ASTNode* valueNode = node->children[1].get();
        
        std::string keyVal = lowerExpr(keyNode);
        std::string valueVal = lowerExpr(valueNode);
        
        // Get the generator (third child)
        const ASTNode* genNode = node->children[2].get();
        if (genNode->type != "comprehension") return;
        
        // Process the generator: target, iter, ifs
        std::string target = genNode->children[0]->id;  // target variable name
        std::string iter = lowerExpr(genNode->children[1].get());  // iterator expression
        
        // Generate the loop structure for comprehension
        std::string loopLabel = "dict_comp_loop_" + std::to_string(tempCounter++);
        std::string loopBodyLabel = "dict_comp_body_" + std::to_string(tempCounter++);
        std::string loopEndLabel = "dict_comp_end_" + std::to_string(tempCounter++);
        
        // Add loop label
        ir.addInstruction(currentFunc, "label", {}, loopLabel);
        
        // Create iterator and check if it has elements
        std::string iterVar = "iter_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"iter_create", iter}, iterVar);
        
        std::string hasNextVar = "has_next_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"iter_has_next", iterVar}, hasNextVar);
        
        // Branch to check if we should continue
        ir.addInstruction(currentFunc, "br", {hasNextVar, loopBodyLabel, loopEndLabel});
        
        // Loop body
        ir.addInstruction(currentFunc, "label", {}, loopBodyLabel);
        
        // Get the next value from iterator
        std::string nextVal = "next_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"iter_next", iterVar}, nextVal);
        
        // Assign to target variable
        ir.addInstruction(currentFunc, "assign", {nextVal}, target);
        
        // Handle conditions (ifs)
        if (genNode->children.size() > 2) {
            // Process conditions (if clauses) - simple implementation
            for (size_t i = 2; i < genNode->children.size(); ++i) {
                std::string cond = lowerExpr(genNode->children[i].get());
                std::string condLabel = "cond_" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "br", {cond, condLabel, loopEndLabel});
                ir.addInstruction(currentFunc, "label", {}, condLabel);
            }
        }
        
        // Add key-value pair to dict
        ir.addInstruction(currentFunc, "call", {"dict_add", dictVar, keyVal, valueVal}, "");
        
        // Continue loop
        ir.addInstruction(currentFunc, "br", {}, loopLabel);
        
        // Loop end
        ir.addInstruction(currentFunc, "label", {}, loopEndLabel);
        
        // Return the dict
        ir.addInstruction(currentFunc, "assign", {dictVar}, "temp_dict");
    }
};

// Legacy thin wrapper kept temporarily for any external callers (to be removed)

void lowerAST(const ASTNode* node, ModuleIR& ir) {
    if (!node) return;
    LoweringVisitor visitor(ir);
    visitor.lower(node);
}

bool Compiler::compile(const std::string& inputPath, const std::string& outputPath, bool useStatic, int optLevel, bool emitLLVM, bool emitASM, bool verbose) {
    PythonParser parser;
    auto ast = parser.parseFile(inputPath);
    if (!ast) {
        std::cerr << "Parse error for " << inputPath << std::endl;
        return false;
    }
    if (verbose) std::cout << "Parsed AST root: " << ast->type << " (depth " << ast->children.size() << ")\n";
    ModuleIR ir;
    lowerAST(ast.get(), ir);
    llvm::LLVMContext context;
    Codegen codegen;
    auto module = codegen.generate(ir, context, "pyc_module");
    if (!module) return false;
    codegen.optimize(module.get(), optLevel);
    if (emitLLVM) {
        if (codegen.emitLLVM(module.get(), outputPath + ".ll")) {
            std::cout << "Emitted LLVM IR " << outputPath << ".ll (opt=" << optLevel << ")\n";
            return true;
        }
        return false;
    }
    if (emitASM) {
        if (codegen.emitAssembly(module.get(), outputPath + ".s")) {
            std::cout << "Emitted assembly " << outputPath << ".s (opt=" << optLevel << ")\n";
            return true;
        }
        return false;
    }
    if (codegen.emitObject(module.get(), outputPath + ".o")) {
        std::cout << "Generated object " << outputPath << ".o (opt=" << optLevel << ")\n";
        std::string linkCmd = "clang++ ";
        if (useStatic) linkCmd += "-static -s -Wl,--gc-sections ";

        // Prefer prebuilt static runtime lib if available (from build or install)
        // Otherwise fall back to compiling the small runtime source (always works during development)
        std::string runtimeLink = " src/runtime/Runtime.cpp";
        // Try common locations for libpycrt.a
        for (const auto& libdir : {"./build", "../build", ".", "/usr/local/lib", "/usr/lib"}) {
            std::string libpath = std::string(libdir) + "/libpycrt.a";
            if (std::ifstream(libpath).good()) {
                runtimeLink = " -L" + std::string(libdir) + " -lpycrt";
                break;
            }
        }
        linkCmd += outputPath + ".o" + runtimeLink + " -o " + outputPath + " -O" + std::to_string(optLevel);
        if (std::system(linkCmd.c_str()) == 0) {
            std::cout << "Linked with runtime to " << outputPath << " (static=" << useStatic << ")\n";
        } else {
            std::cerr << "Link failed. Run manually: " << linkCmd << std::endl;
            return false;
        }
        return true;
    }
    return false;
}

} // namespace pyc
