#include "pyc/Compiler.h"
#include "pyc/PythonParser.h"
#include "pyc/IR.h"
#include "pyc/Codegen.h"
#include <llvm/IR/LLVMContext.h>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <string>

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

        if (node->type == "Constant") {
            std::string res = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {node->value}, res);
            return res;
        } else if (node->type == "Name") {
            return node->id;
        } else if (node->type == "BinOp") {
            return lowerBinOp(node);
        } else if (node->type == "Call") {
            return lowerCall(node);
        } else if (node->type == "Compare") {
            return lowerCompare(node);
        } else if (node->type == "Return") {
            return lowerReturnExpr(node);
        }
        return "";
    }

private:
    ModuleIR& ir;
    std::string currentFunc;
    int tempCounter = 0;

    std::string lowerBinOp(const ASTNode* node) {
        std::string left = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string right = lowerExpr(node->children.size() > 1 ? node->children[1].get() : nullptr);
        std::string res = "t" + std::to_string(tempCounter++);
        std::string op = node->op.empty() ? "add" : node->op;
        if (op == "Add") op = "add";
        else if (op == "Sub") op = "sub";
        else if (op == "Mult") op = "mul";
        else if (op == "Div" || op == "FloorDiv") op = "div";
        else if (op == "Mod") op = "mod";
        ir.addInstruction(currentFunc, op, {left, right}, res);
        return res;
    }

    std::string lowerCall(const ASTNode* node) {
        std::string funcName = (node->children.empty() || !node->children[0]) ? "" : node->children[0]->id;
        std::vector<std::string> argRes;
        for (size_t i = 1; i < node->children.size(); ++i) {
            if (node->children[i]) {
                argRes.push_back(lowerExpr(node->children[i].get()));
            }
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

    void lowerListComp(const ASTNode* node) {
        // List comprehension structure: [elt for target in iter if ifs]
        if (node->children.size() < 2) return;
        
        // Create a temporary list variable for the result
        std::string listVar = "list_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"list_create"}, listVar);
        
        // Get the element expression (first child)
        const ASTNode* eltNode = node->children[0].get();
        std::string eltVal = lowerExpr(eltNode);
        
        // Get the generator (second child)
        const ASTNode* genNode = node->children[1].get();
        if (genNode->type != "comprehension") return;
        
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
        ir.addInstruction(currentFunc, "assign", {listVar}, "temp_list");
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
