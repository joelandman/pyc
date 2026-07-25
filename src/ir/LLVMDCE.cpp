#include "pyc/LLVMDCE.h"
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Transforms/Scalar/DCE.h>

namespace pyc {

void LLVMDCE::eliminate(llvm::Module& module) {
    runDCE(module);
}

void LLVMDCE::runDCE(llvm::Module& module) {
    // Use LLVM's new pass manager
    llvm::FunctionPassManager fpm;
    
    // Add DCE pass to remove dead code from functions
    fpm.addPass(llvm::DCEPass());
    
    // Run the function pass manager on each function in the module
    for (auto& func : module.functions()) {
        llvm::AnalysisManager<llvm::Function> fam;
        llvm::PassBuilder pb;
        pb.registerFunctionAnalyses(fam);
        fpm.run(func, fam);
    }
}

} // namespace pyc
