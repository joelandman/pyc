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
            for (const auto& c : node->children) {
                lower(c.get());
            }
        } else if (node->type == "FunctionDef") {
            std::string saved = currentFunc;
            ir.addFunction(node->id, node->args);
            funcParamNames[node->id] = node->args;

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
            ir.addInstruction(currentFunc, "br", {}, "exit");
        } else if (node->type == "Continue") {
            ir.addInstruction(currentFunc, "br", {}, "loop");
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
            if (node->is_float) {
                ir.addInstruction(currentFunc, "fconst", {val}, res);
            } else {
                // Quote strings so codegen can distinguish them from numbers
                if (!val.empty() && val.find_first_of("\"'") == std::string::npos) {
                    bool isNum = val.find_first_not_of("0123456789+-") == std::string::npos;
                    if (!isNum) val = "\"" + val + "\"";
                }
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
        }
        return "";
    }

private:
    ModuleIR& ir;
    std::string currentFunc;
    int tempCounter = 0;
    std::unordered_map<std::string, int> funcDefaultCount;
    std::unordered_map<std::string, std::vector<std::string>> funcDefaultValues;
    std::unordered_map<std::string, std::vector<std::string>> funcParamNames;

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
        ir.addInstruction(currentFunc, op, {left, right}, res);
        return res;
    }

    std::string lowerCall(const ASTNode* node) {
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
        std::string left = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string right = lowerExpr(node->children.size() > 1 ? node->children[1].get() : nullptr);
        std::string res = "cmp" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "icmp", {node->op, left, right}, res);
        return res;
    }

    void lowerIf(const ASTNode* node) {
        std::string cond = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        ir.addInstruction(currentFunc, "br", {cond, "then", "else"});
        ir.addInstruction(currentFunc, "label", {}, "then");
        size_t n = node->children.size();
        for (size_t i = 1; i < n - (n > 2 ? 1 : 0); ++i) {
            lower(node->children[i].get());   // statements, not expr values
        }
        ir.addInstruction(currentFunc, "br", {}, "endif");
        ir.addInstruction(currentFunc, "label", {}, "else");
        if (n > 2) {
            lower(node->children[n-1].get());
        }
        ir.addInstruction(currentFunc, "label", {}, "endif");
    }

    void lowerWhile(const ASTNode* node) {
        ir.addInstruction(currentFunc, "label", {}, "loop");
        std::string cond = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        ir.addInstruction(currentFunc, "br", {cond, "body", "exit"});
        ir.addInstruction(currentFunc, "label", {}, "body");
        for (size_t i = 1; i < node->children.size(); ++i) {
            lower(node->children[i].get());   // statements
        }
        ir.addInstruction(currentFunc, "br", {}, "loop");
        ir.addInstruction(currentFunc, "label", {}, "exit");
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

        ir.addInstruction(currentFunc, "label", {}, loopLabel);
        std::string cmpRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "icmp", {"Lt", idxVar, lenRes}, cmpRes);
        ir.addInstruction(currentFunc, "br", {cmpRes, bodyLabel, exitLabel});

        ir.addInstruction(currentFunc, "label", {}, bodyLabel);
        std::string itemRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", listVal, idxVar}, itemRes);
        ir.addInstruction(currentFunc, "assign", {itemRes}, node->id);  // target = node->id

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
    }

    std::string lowerList(const ASTNode* node) {
        size_t n = node->children.size();
        std::string sizeStr = std::to_string(n);
        std::string listRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_New", sizeStr}, listRes);

        for (size_t i = 0; i < n; ++i) {
            std::string elem = lowerExpr(node->children[i].get());
            std::string idxStr = std::to_string(i);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItem", listRes, idxStr, elem}, "");
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
        if (!node->children.empty() && node->children[0]) {
            std::string val = lowerExpr(node->children[0].get());
            ir.addInstruction(currentFunc, "assign", {val}, node->id);
        }
    }

    std::string lowerReturnExpr(const ASTNode* node) {
        std::string val = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        ir.addInstruction(currentFunc, "ret", {val}, val);
        return val;
    }

    void lowerReturn(const ASTNode* node) {
        lowerReturnExpr(node);
    }

    std::string lowerListComp(const ASTNode* node) {
        // List comprehension structure: [elt for target in iter if ifs]
        if (node->children.size() < 2) return "";
        
        // Create a temporary list variable for the result
        std::string listVar = "list_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"list_create"}, listVar);
        
        // Get the element expression (first child)
        const ASTNode* eltNode = node->children[0].get();
        std::string eltVal = lowerExpr(eltNode);
        
        // Get the generator (second child)
        const ASTNode* genNode = node->children[1].get();
        if (genNode->type != "comprehension") return "";
        
        // Process the generator: target, iter, ifs
        std::string target = genNode->children[0]->id;  // target variable name
        std::string iter = lowerExpr(genNode->children[1].get());  // iterator expression
        
        // Generate the loop structure for comprehension
        std::string loopLabel = "list_comp_loop_" + std::to_string(tempCounter++);
        std::string loopBodyLabel = "list_comp_body_" + std::to_string(tempCounter++);
        std::string loopEndLabel = "list_comp_end_" + std::to_string(tempCounter++);
        
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
        
        // Add element to list
        ir.addInstruction(currentFunc, "call", {"list_append", listVar, eltVal}, "");
        
        // Continue loop
        ir.addInstruction(currentFunc, "br", {}, loopLabel);
        
        // Loop end
        ir.addInstruction(currentFunc, "label", {}, loopEndLabel);
        
        // Return the list
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
