// codegen/ir_gen.cpp - LLVM IR code generator for pyc compiler
// Converts AST/IR intermediate representation to LLVM IR

#include "codegen/llir_gen.h"
#include "frontend/ast.h"
#include "ir/ir.h"
#include "ir/builder.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>
#include <unordered_map>
#include <memory>

namespace pyc::codegen {

// ===== Type mapping from C++ types to LLVM types =====

static llvm::Type* get_llvm_type(llvm::IRBuilder<>& builder, pyc::ir::TypeKind kind) {
    switch (kind) {
        case pyc::ir::TypeKind::I32:    return builder.getInt32Ty();
        case pyc::ir::TypeKind::I64:    return builder.getInt64Ty();
        case pyc::ir::TypeKind::F64:    return builder.getDoubleTy();
        case pyc::ir::TypeKind::PBYTE:  return builder.getInt8Ty();
        case pyc::ir::TypeKind::PYOBJ:  return builder.getPtrTy();
        case pyc::ir::TypeKind::VOID:   return builder.getVoidTy();
        default:                        return builder.getInt64Ty();
    }
}

// ===== LlirGen Implementation =====

class LlirGenImpl {
    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    std::unordered_map<std::string, llvm::Value*> locals_;
    std::unordered_map<std::string, llvm::Value*> globals_;
    std::vector<struct FuncDecl> functions_;
    std::string current_function_;
    
    // PyObject helper function declarations
    llvm::Function* fn_create_int_ = nullptr;
    llvm::Function* fn_create_str_ = nullptr;
    llvm::Function* fn_create_float_ = nullptr;
    llvm::Function* fn_print_ = nullptr;
    llvm::Function* fn_call_ = nullptr;
    
public:
    LlirGenImpl() : builder_(context_) {
        // Create module
        module_ = std::make_unique<llvm::Module>("pyc_module", context_);
    }
    
    void set_python_object_helpers(
        llvm::Function* create_int,
        llvm::Function* create_str,
        llvm::Function* create_float,
        llvm::Function* print,
        llvm::Function* call
    ) {
        fn_create_int_ = create_int;
        fn_create_str_ = create_str;
        fn_create_float_ = create_float;
        fn_print_ = print;
        fn_call_ = call;
    }
    
    std::string generate_code(const std::string& module_name) {
        // Generate main function
        generate_main();
        
        // Generate user-defined functions
        for (auto& func_decl : functions_) {
            generate_function(func_decl);
        }
        
        // Print LLVM IR
        std::string result;
        llvm::raw_string_ostream os(result);
        module_->print(os, nullptr);
        return result;
    }
    
    void generate_function(FuncDecl& func_decl) {
        std::string func_name = func_decl.name;
        
        // Create function type
        std::vector<llvm::Type*> arg_types;
        for (auto& type_name : func_decl.param_types) {
            arg_types.push_back(get_llvm_type(builder_, get_type_kind(type_name)));
        }
        auto func_type = llvm::FunctionType::get(
            get_llvm_type(builder_, get_type_kind(func_decl.return_type)),
            arg_types,
            false
        );
        
        // Create function
        auto llvm_func = llvm::Function::Create(
            func_type,
            llvm::Function::ExternalLinkage,
            func_decl.name,
            module_.get()
        );
        
        // Create entry block
        auto entry_block = llvm::BasicBlock::Create(context_, "entry", llvm_func);
        builder_.SetInsertPoint(entry_block);
        
        // Store function name
        current_function_ = func_decl.name;
        
        // Generate function body from AST
        generate_function_body(llvm_func);
        
        // Add return if needed
        if (func_decl.return_type != "void") {
            auto zero = builder_.getInt64(0);
            builder_.CreateRet(zero);
        }
        
        local_functions_[func_name] = llvm_func;
    }
    
    void generate_function_body(llvm::Function* /*llvm_func*/) {
        // This would be implemented to walk AST nodes and generate IR
        // For now, stub implementation
        // llvm::dbgs() << "Generating function body...\n";
    }
    
    void generate_main() {
        // Create _pyc_main function
        auto main_type = llvm::FunctionType::get(
            builder_.getInt32Ty(),
            false
        );
        auto main_func = llvm::Function::Create(
            main_type,
            llvm::Function::ExternalLinkage,
            "_pyc_main",
            module_.get()
        );
        
        auto main_block = llvm::BasicBlock::Create(context_, "entry", main_func);
        builder_.SetInsertPoint(main_block);
        
        // Call main function if it exists
        if (auto it = local_functions_.find("main"); it != local_functions_.end()) {
            builder_.CreateCall(it->second, {});
        }
        
        builder_.CreateRet(builder_.getInt32(0));
    }
    
    // Type management
    pyc::ir::TypeKind get_type_kind(const std::string& type_name) {
        if (type_name == "int" || type_name == "Integer") {
            return pyc::ir::TypeKind::I64;
        }
        if (type_name == "float" || type_name == "Float") {
            return pyc::ir::TypeKind::F64;
        }
        if (type_name == "str" || type_name == "String") {
            return pyc::ir::TypeKind::PYOBJ;
        }
        if (type_name == "bool") {
            return pyc::ir::TypeKind::I32;
        }
        return pyc::ir::TypeKind::PYOBJ;
    }
    
private:
    std::unordered_map<std::string, llvm::Function*> local_functions_;
};

// ===== Public API Implementation =====

std::string LlirGen::generate_code(const std::string& /*module_name*/) {
    return "// Legacy LLVM IR generator - stub";
}

} // namespace pyc::codegen
