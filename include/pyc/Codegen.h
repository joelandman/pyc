#pragma once
#include "IR.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <string>
#include <vector>

namespace pyc {

class Codegen {
public:
    std::unique_ptr<llvm::Module> generate(ModuleIR& ir, llvm::LLVMContext& context, const std::string& moduleName, bool debugInfo = false);
    bool emitObject(llvm::Module* module, const std::string& outputPath);
    bool emitObject(llvm::Module* module, const std::string& outputPath,
                    const std::string& mcpu, const std::string& march);
    bool emitLLVM(llvm::Module* module, const std::string& outputPath);
    bool emitAssembly(llvm::Module* module, const std::string& outputPath);
    bool emitAssembly(llvm::Module* module, const std::string& outputPath,
                      const std::string& mcpu, const std::string& march);
    void optimize(llvm::Module* module, int optLevel,
                  const std::string& mcpu = "",
                  const std::string& march = "",
                  const std::string& pgoInstrument = "",
                  const std::string& pgoProfile = "");

    // O5: Emit user code as LLVM bitcode (for full LTO at -O5)
    bool emitBitcode(llvm::Module* module, const std::string& outputPath);

    // B7: Merge multiple LLVM modules into one (for import support)
    static std::unique_ptr<llvm::Module> mergeModules(
        std::vector<std::unique_ptr<llvm::Module>>& modules,
        llvm::LLVMContext& context,
        const std::string& outputModuleName);

    // LTO: Load precompiled runtime bitcode and link into module
    static bool linkRuntimeBitcode(
        llvm::Module* module,
        const std::string& bitcodePath);
};

} // namespace pyc
