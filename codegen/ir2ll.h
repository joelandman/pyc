#pragma once
#include <string>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include "ir/ir.h"

namespace pyc::codegen {

// Translate IR module to string representation of LLVM IR
std::string translate_module(pyc::ir::IRModule& ir_mod);

// Translate IR module and return raw LLVM Module pointer
llvm::Module* translate_module_to_llvm(pyc::ir::IRModule& ir_mod, llvm::LLVMContext& ctx);

} // namespace pyc::codegen
