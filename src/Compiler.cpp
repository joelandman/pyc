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

std::string lowerExpression(const ASTNode* node, ModuleIR& ir, const std::string& currentFunc, int& tempCounter) {
    if (!node || currentFunc.empty()) return "";
    if (node->type == "Constant") {
        std::string res = "c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {node->value}, res);
        return res;
    } else if (node->type == "Name") {
        return node->id;  // arg or var (load in codegen)
    } else if (node->type == "BinOp") {
        std::string left = lowerExpression(node->children.empty() ? nullptr : node->children[0].get(), ir, currentFunc, tempCounter);
        std::string right = lowerExpression(node->children.size() > 1 ? node->children[1].get() : nullptr, ir, currentFunc, tempCounter);
        std::string res = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "add", {left, right}, res);
        return res;
    } else if (node->type == "Call") {
        std::string funcName = (node->children.empty() || !node->children[0]) ? "" : node->children[0]->id;
        std::vector<std::string> argRes;
        for (size_t i = 1; i < node->children.size(); ++i) {
            if (node->children[i]) {
                argRes.push_back(lowerExpression(node->children[i].get(), ir, currentFunc, tempCounter));
            }
        }
        std::string res = "t" + std::to_string(tempCounter++);
        std::vector<std::string> ops = {funcName};
        ops.insert(ops.end(), argRes.begin(), argRes.end());
        ir.addInstruction(currentFunc, "call", ops, res);
        return res;
    } else if (node->type == "Return") {
        std::string val = lowerExpression(node->children.empty() ? nullptr : node->children[0].get(), ir, currentFunc, tempCounter);
        ir.addInstruction(currentFunc, "ret", {val}, val);
        return val;
    } else if (node->type == "Compare") {
        std::string left = lowerExpression(node->children.empty() ? nullptr : node->children[0].get(), ir, currentFunc, tempCounter);
        std::string right = lowerExpression(node->children.size() > 1 ? node->children[1].get() : nullptr, ir, currentFunc, tempCounter);
        std::string res = "cmp" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "icmp", {node->op, left, right}, res);
        return res;
    } else if (node->type == "If") {
        std::string cond = lowerExpression(node->children.empty() ? nullptr : node->children[0].get(), ir, currentFunc, tempCounter);
        ir.addInstruction(currentFunc, "br", {cond, "then", "else"});
        ir.addInstruction(currentFunc, "label", {}, "then");
        for (size_t i = 1; i < node->children.size(); ++i) {  // body
            if (i < node->children.size() - 1) lowerExpression(node->children[i].get(), ir, currentFunc, tempCounter);
        }
        ir.addInstruction(currentFunc, "br", {}, "endif");
        ir.addInstruction(currentFunc, "label", {}, "else");
        // orelse body (simplified)
        ir.addInstruction(currentFunc, "label", {}, "endif");
        return "";
    } else if (node->type == "While") {
        ir.addInstruction(currentFunc, "label", {}, "loop");
        std::string cond = lowerExpression(node->children.empty() ? nullptr : node->children[0].get(), ir, currentFunc, tempCounter);
        ir.addInstruction(currentFunc, "br", {cond, "body", "exit"});
        ir.addInstruction(currentFunc, "label", {}, "body");
        for (size_t i = 1; i < node->children.size(); ++i) {
            lowerExpression(node->children[i].get(), ir, currentFunc, tempCounter);
        }
        ir.addInstruction(currentFunc, "br", {}, "loop");
        ir.addInstruction(currentFunc, "label", {}, "exit");
        return "";
    } else if (node->type == "Assign") {
        if (!node->children.empty() && node->children[0]) {
            std::string val = lowerExpression(node->children[0].get(), ir, currentFunc, tempCounter);
            ir.addInstruction(currentFunc, "assign", {val}, node->id);
        }
        return node->id;
    }
    for (const auto& c : node->children) {
        if (c) lowerExpression(c.get(), ir, currentFunc, tempCounter);
    }
    return "";
}

void lowerAST(const ASTNode* node, ModuleIR& ir) {
    if (!node) return;
    if (node->type == "FunctionDef") {
        ir.addFunction(node->id, node->args);
        int temp = 0;
        for (const auto& c : node->children) {
            lowerExpression(c.get(), ir, node->id, temp);
        }
    } else if (node->type == "Expr" || node->type == "Module") {
        // Top-level in main
        ir.addFunction("main", {});
        int temp = 0;
        lowerExpression(node, ir, "main", temp);
    }
    for (const auto& c : node->children) {
        lowerAST(c.get(), ir);
    }
}

bool Compiler::compile(const std::string& inputPath, const std::string& outputPath, bool useStatic, int optLevel) {
    PythonParser parser;
    auto ast = parser.parseFile(inputPath);
    if (!ast) {
        std::cerr << "Parse error for " << inputPath << std::endl;
        return false;
    }
    std::cout << "Parsed AST root: " << ast->type << " (depth " << ast->children.size() << ")\n";
    ModuleIR ir;
    lowerAST(ast.get(), ir);
    llvm::LLVMContext context;
    Codegen codegen;
    auto module = codegen.generate(ir, context, "pyc_module");
    if (!module) return false;
    codegen.optimize(module.get(), optLevel);
    if (codegen.emitObject(module.get(), outputPath + ".o")) {
        std::cout << "Generated object " << outputPath << ".o (opt=" << optLevel << ")\n";
        std::string linkCmd = "clang++ ";
        if (useStatic) linkCmd += "-static -s -Wl,--gc-sections ";
        linkCmd += outputPath + ".o src/runtime/Runtime.cpp -o " + outputPath + " -O" + std::to_string(optLevel);
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
