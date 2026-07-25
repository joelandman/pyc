#pragma once
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/IR/PassManager.h>

namespace pyc {

// Dead Code Elimination at LLVM IR level
// Uses LLVM's built-in DCE pass via the new pass manager
class LLVMDCE {
public:
    // Apply dead code elimination to an LLVM module
    static void eliminate(llvm::Module& module);
    
private:
    // Run DCE using LLVM's PassBuilder
    static void runDCE(llvm::Module& module);
};

} // namespace pyc
