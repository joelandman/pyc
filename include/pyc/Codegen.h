#pragma once
#include "IR.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <string>

namespace pyc {

class Codegen {
public:
    std::unique_ptr<llvm::Module> generate(ModuleIR& ir, llvm::LLVMContext& context, const std::string& moduleName);
    bool emitObject(llvm::Module* module, const std::string& outputPath);
    bool emitLLVM(llvm::Module* module, const std::string& outputPath);
    bool emitAssembly(llvm::Module* module, const std::string& outputPath);
    void optimize(llvm::Module* module, int optLevel = 2);
};

} // namespace pyc
