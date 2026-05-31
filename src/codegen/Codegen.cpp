#include "pyc/Codegen.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <unordered_map>
#include <fstream>
#include <iostream>

namespace pyc {

std::unique_ptr<llvm::Module> Codegen::generate(ModuleIR& ir, llvm::LLVMContext& context, const std::string& moduleName) {
    auto module = std::make_unique<llvm::Module>(moduleName, context);
    llvm::IRBuilder<> builder(context);

    // PyObject struct layout (must match Runtime.cpp)
    llvm::StructType* pyObjectTy = llvm::StructType::create(context, {
        llvm::Type::getInt32Ty(context),   // refcount
        llvm::Type::getInt32Ty(context),   // type
        llvm::Type::getInt64Ty(context)    // value
    }, "PyObject");

    llvm::PointerType* pyObjectPtrTy = llvm::PointerType::get(context, 0);

    // Declare runtime functions (from include/pyc/runtime.h)
    llvm::FunctionType* fromLongTy = llvm::FunctionType::get(pyObjectPtrTy, {llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(fromLongTy, llvm::Function::ExternalLinkage, "PyInt_FromLong", module.get());

    llvm::FunctionType* listNewTy = llvm::FunctionType::get(pyObjectPtrTy, {llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(listNewTy, llvm::Function::ExternalLinkage, "PyList_New", module.get());

    llvm::FunctionType* listGetItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(listGetItemTy, llvm::Function::ExternalLinkage, "PyList_GetItem", module.get());

    llvm::FunctionType* listSizeTy = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(listSizeTy, llvm::Function::ExternalLinkage, "PyList_Size", module.get());

    llvm::FunctionType* listSetItemTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, llvm::Type::getInt64Ty(context), pyObjectPtrTy}, false);
    llvm::Function::Create(listSetItemTy, llvm::Function::ExternalLinkage, "PyList_SetItem", module.get());

    llvm::FunctionType* numberAddTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(numberAddTy, llvm::Function::ExternalLinkage, "PyNumber_Add", module.get());

    llvm::FunctionType* numberMultTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(numberMultTy, llvm::Function::ExternalLinkage, "PyNumber_Multiply", module.get());

    llvm::FunctionType* numberSubTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(numberSubTy, llvm::Function::ExternalLinkage, "PyNumber_Subtract", module.get());

    llvm::FunctionType* numberDivTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(numberDivTy, llvm::Function::ExternalLinkage, "PyNumber_Divide", module.get());

    llvm::FunctionType* numberRemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(numberRemTy, llvm::Function::ExternalLinkage, "PyNumber_Remainder", module.get());

    llvm::FunctionType* decrefTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(decrefTy, llvm::Function::ExternalLinkage, "Py_DECREF", module.get());

    llvm::FunctionType* increfTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(increfTy, llvm::Function::ExternalLinkage, "Py_INCREF", module.get());

    llvm::PointerType* int8PtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
    llvm::FunctionType* objectPrintTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {pyObjectPtrTy, int8PtrTy}, false);
    llvm::Function::Create(objectPrintTy, llvm::Function::ExternalLinkage, "PyObject_Print", module.get());

    // printf no longer used in normal code paths (we use PyObject_Print)

    for (const auto& f : ir.functions) {
        std::vector<llvm::Type*> argTypes(f.args.size(), pyObjectPtrTy);
        llvm::FunctionType* funcType = llvm::FunctionType::get(pyObjectPtrTy, argTypes, false);
        llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, f.name, module.get());
    }

    for (const auto& f : ir.functions) {
        llvm::Function* func = module->getFunction(f.name);
        if (!func) continue;
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", func);
        builder.SetInsertPoint(entry);

        std::unordered_map<std::string, llvm::Value*> valueMap;
        for (size_t i = 0; i < f.args.size(); ++i) {
            llvm::Value* arg = func->getArg(i);
            arg->setName(f.args[i]);
            valueMap[f.args[i]] = arg;
        }

        std::unordered_map<std::string, llvm::BasicBlock*> blockMap;
        blockMap["entry"] = entry;
        for (const auto& inst : f.body) {
            if (inst.op == "label") {
                const std::string& ln = inst.result;
                if (blockMap.find(ln) == blockMap.end()) {
                    blockMap[ln] = llvm::BasicBlock::Create(context, ln, func);
                }
            }
        }

        auto getOrLoad = [&](const std::string& name) -> llvm::Value* {
            auto it = valueMap.find(name);
            if (it == valueMap.end()) return llvm::ConstantPointerNull::get(llvm::PointerType::get(context, 0));
            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second)) {
                return builder.CreateLoad(pyObjectPtrTy, alloca, name + ".load");
            }
            return it->second;
        };

        // Helper: unbox a PyObject* (assumed to be int) to i64
        auto unboxToI64 = [&](llvm::Value* boxed) -> llvm::Value* {
            if (!boxed || boxed->getType() == llvm::Type::getInt64Ty(context)) return boxed;
            return builder.CreateLoad(llvm::Type::getInt64Ty(context),
                builder.CreateStructGEP(pyObjectTy, boxed, 2), "unboxed");
        };

        llvm::BasicBlock* curBlock = entry;
        for (const auto& inst : f.body) {
            if (inst.op == "label") {
                auto it = blockMap.find(inst.result);
                if (it != blockMap.end()) {
                    llvm::BasicBlock* target = it->second;
                    if (!curBlock->getTerminator()) {
                        builder.CreateBr(target);
                    }
                    builder.SetInsertPoint(target);
                    curBlock = target;
                }
                continue;
            } else if (inst.op == "br") {
                if (inst.operands.size() >= 3) {
                    std::string cname = inst.operands[0].name;
                    std::string tname = inst.operands[1].name;
                    std::string fname = inst.operands[2].name;
                    llvm::Value* cval = getOrLoad(cname);

                    // Unbox if this is a boxed comparison result (PyObject*)
                    if (cval->getType() != llvm::Type::getInt1Ty(context)) {
                        llvm::Value* unboxed = unboxToI64(cval);
                        // Treat non-zero as true
                        cval = builder.CreateICmpNE(unboxed, llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    }

                    auto tit = blockMap.find(tname);
                    auto fit = blockMap.find(fname);
                    if (tit != blockMap.end() && fit != blockMap.end()) {
                        builder.CreateCondBr(cval, tit->second, fit->second);
                    }
                } else if (!inst.result.empty()) {
                    auto it = blockMap.find(inst.result);
                    if (it != blockMap.end() && !curBlock->getTerminator()) {
                        builder.CreateBr(it->second);
                    }
                }
                continue;
            } else if (inst.op == "icmp") {
                std::string opstr = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::CmpInst::Predicate pred = llvm::CmpInst::ICMP_EQ;
                if (opstr == "Gt" || opstr == "gt") pred = llvm::CmpInst::ICMP_SGT;
                else if (opstr == "Lt" || opstr == "lt") pred = llvm::CmpInst::ICMP_SLT;
                else if (opstr == "Eq" || opstr == "eq") pred = llvm::CmpInst::ICMP_EQ;
                else if (opstr == "GtE") pred = llvm::CmpInst::ICMP_SGE;
                else if (opstr == "LtE") pred = llvm::CmpInst::ICMP_SLE;
                else if (opstr == "NotEq") pred = llvm::CmpInst::ICMP_NE;

                llvm::Value* lhsBox = getOrLoad(inst.operands.size() > 1 ? inst.operands[1].name : "");
                llvm::Value* rhsBox = getOrLoad(inst.operands.size() > 2 ? inst.operands[2].name : "");

                // Unbox: GEP to the 'value' field (index 2) and load it
                llvm::Value* lhsVal = unboxToI64(lhsBox);
                llvm::Value* rhsVal = unboxToI64(rhsBox);

                llvm::Value* rawCmp = builder.CreateICmp(pred, lhsVal, rhsVal);

                // Box the comparison result as PyInt (0 or 1)
                llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
                llvm::Value* boxedCmp;
                if (fromLong) {
                    llvm::Value* oneOrZero = builder.CreateSelect(
                        rawCmp,
                        llvm::ConstantInt::get(context, llvm::APInt(64, 1)),
                        llvm::ConstantInt::get(context, llvm::APInt(64, 0))
                    );
                    boxedCmp = builder.CreateCall(fromLong, {oneOrZero}, inst.result);
                } else {
                    boxedCmp = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }

                valueMap[inst.result] = boxedCmp;
                continue;
            }
            if (inst.op == "const") {
                long val = inst.operands.empty() ? 0L : std::stol(inst.operands[0].name);
                llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
                if (fromLong) {
                    llvm::Value* boxed = builder.CreateCall(fromLong, {llvm::ConstantInt::get(context, llvm::APInt(64, val))}, inst.result);
                    valueMap[inst.result] = boxed;
                }
            } else if (inst.op == "add") {
                llvm::Function* numberAdd = module->getFunction("PyNumber_Add");
                llvm::Value* lhs = getOrLoad(inst.operands[0].name);
                llvm::Value* rhs = getOrLoad(inst.operands[1].name);
                if (numberAdd) {
                    llvm::Value* sum = builder.CreateCall(numberAdd, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = sum;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "sub") {
                llvm::Function* numberSub = module->getFunction("PyNumber_Subtract");
                llvm::Value* lhs = getOrLoad(inst.operands[0].name);
                llvm::Value* rhs = getOrLoad(inst.operands[1].name);
                if (numberSub) {
                    llvm::Value* diff = builder.CreateCall(numberSub, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = diff;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "div") {
                llvm::Function* numberDiv = module->getFunction("PyNumber_Divide");
                llvm::Value* lhs = getOrLoad(inst.operands[0].name);
                llvm::Value* rhs = getOrLoad(inst.operands[1].name);
                if (numberDiv) {
                    llvm::Value* quot = builder.CreateCall(numberDiv, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = quot;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "mod") {
                llvm::Function* numberRem = module->getFunction("PyNumber_Remainder");
                llvm::Value* lhs = getOrLoad(inst.operands[0].name);
                llvm::Value* rhs = getOrLoad(inst.operands[1].name);
                if (numberRem) {
                    llvm::Value* rem = builder.CreateCall(numberRem, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = rem;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "mul") {
                llvm::Function* numberMult = module->getFunction("PyNumber_Multiply");
                llvm::Value* lhs = getOrLoad(inst.operands[0].name);
                llvm::Value* rhs = getOrLoad(inst.operands[1].name);
                if (numberMult) {
                    llvm::Value* prod = builder.CreateCall(numberMult, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = prod;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "subscript") {
                // Handle list indexing: list[index]
                llvm::Function* listGetItem = module->getFunction("PyList_GetItem");
                if (listGetItem) {
                    llvm::Value* list = getOrLoad(inst.operands[0].name);
                    llvm::Value* index = getOrLoad(inst.operands[1].name);
                    // Convert index to i64 if needed
                    if (index->getType() != llvm::Type::getInt64Ty(context)) {
                        index = builder.CreateZExtOrTrunc(index, llvm::Type::getInt64Ty(context));
                    }
                    llvm::Value* item = builder.CreateCall(listGetItem, {list, index}, inst.result);
                    valueMap[inst.result] = item;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "list") {
                // Create a new list with the given elements
                llvm::Function* listNew = module->getFunction("PyList_New");
                if (listNew) {
                    // For now, we'll create an empty list and assume elements will be set later
                    // This is a simplification - in a more complete implementation we'd pass the element count
                    llvm::Value* listSize = llvm::ConstantInt::get(context, llvm::APInt(64, inst.operands.size()));
                    llvm::Value* newList = builder.CreateCall(listNew, {listSize}, inst.result);
                    valueMap[inst.result] = newList;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "assign") {
                llvm::Value* newVal = getOrLoad(inst.operands[0].name);

                // Basic refcounting for variables: variables own a reference
                if (valueMap.find(inst.result) != valueMap.end()) {
                    // DECREF the old value
                    llvm::Value* oldVal = builder.CreateLoad(pyObjectPtrTy, valueMap[inst.result], inst.result + ".old");
                    llvm::Function* decref = module->getFunction("Py_DECREF");
                    if (decref) {
                        builder.CreateCall(decref, {oldVal});
                    }
                } else {
                    llvm::AllocaInst* alloca = builder.CreateAlloca(pyObjectPtrTy, nullptr, inst.result);
                    valueMap[inst.result] = alloca;
                }

                // INCREF the new value (variable takes ownership)
                llvm::Function* incref = module->getFunction("Py_INCREF");
                if (incref) {
                    builder.CreateCall(incref, {newVal});
                }

                builder.CreateStore(newVal, valueMap[inst.result]);
            } else if (inst.op == "call") {
                std::string funcName = inst.operands.empty() ? "" : inst.operands[0].name;
                if (funcName == "print" || funcName == "pyc_print") {
                    llvm::Function* pyPrint = module->getFunction("PyObject_Print");
                    if (pyPrint) {
                        llvm::Value* arg = (inst.operands.size() > 1 ? getOrLoad(inst.operands[1].name) : llvm::ConstantPointerNull::get(pyObjectPtrTy));
                        // Pass null — our runtime tolerates it and uses real stdout
                        builder.CreateCall(pyPrint, {arg, llvm::ConstantPointerNull::get(int8PtrTy)});

                        // Basic refcounting: consume the printed value if it was a temp
                        llvm::Function* decref = module->getFunction("Py_DECREF");
                        if (decref) {
                            builder.CreateCall(decref, {arg});
                        }
                    }
                } else {
                    llvm::Function* callee = module->getFunction(funcName);
                    if (callee) {
                        std::vector<llvm::Value*> callArgs;
                        for (size_t i = 1; i < inst.operands.size(); ++i) {
                            callArgs.push_back(getOrLoad(inst.operands[i].name));
                        }
                        llvm::Value* callRes = builder.CreateCall(callee, callArgs, inst.result);
                        valueMap[inst.result] = callRes;
                    }
                }
            } else if (inst.op == "ret") {
                std::string retName = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* retVal = getOrLoad(retName);
                // If we couldn't find a value, return a boxed 0 instead of null
                if (retVal == llvm::ConstantPointerNull::get(pyObjectPtrTy)) {
                    llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
                    if (fromLong) {
                        retVal = builder.CreateCall(fromLong, {llvm::ConstantInt::get(context, llvm::APInt(64, 0))});
                    }
                }
                builder.CreateRet(retVal);
                break;
            }
        }
        if (!curBlock->getTerminator()) {
            // Return a boxed 0 as a sensible default instead of null
            llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
            if (fromLong) {
                llvm::Value* zero = builder.CreateCall(fromLong, {llvm::ConstantInt::get(context, llvm::APInt(64, 0))});
                builder.CreateRet(zero);
            } else {
                builder.CreateRet(llvm::ConstantPointerNull::get(pyObjectPtrTy));
            }
        }
    }
    if (llvm::verifyModule(*module, &llvm::errs())) {
        std::cerr << "Module verification failed\n";
    }
    return module;
}

bool Codegen::emitObject(llvm::Module* module, const std::string& outputPath) {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();

    std::string targetTriple = "x86_64-unknown-linux-gnu";
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    if (!target) {
        std::cerr << error << std::endl;
        return false;
    }

    llvm::TargetOptions opt;
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(targetTriple, "generic", "", opt, llvm::Reloc::PIC_, std::nullopt));
    if (!targetMachine) return false;

    module->setDataLayout(targetMachine->createDataLayout());
    module->setTargetTriple(targetTriple);

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "Could not open file: " << ec.message() << std::endl;
        return false;
    }

    llvm::legacy::PassManager pass;
    llvm::CodeGenFileType fileType = llvm::CodeGenFileType::ObjectFile;
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        std::cerr << "Target machine can't emit object file" << std::endl;
        return false;
    }
    pass.run(*module);
    dest.flush();
    return true;
}

void Codegen::optimize(llvm::Module* module, int optLevel) {
  if (!module || optLevel <= 0) return;
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(
      optLevel == 1 ? llvm::OptimizationLevel::O1 :
      optLevel == 2 ? llvm::OptimizationLevel::O2 :
      llvm::OptimizationLevel::O3);
  MPM.run(*module, MAM);
}

bool Codegen::emitLLVM(llvm::Module* module, const std::string& outputPath) {
    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "Could not open " << outputPath << ": " << ec.message() << std::endl;
        return false;
    }
    module->print(dest, nullptr);
    dest.flush();
    return true;
}

bool Codegen::emitAssembly(llvm::Module* module, const std::string& outputPath) {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();

    std::string targetTriple = "x86_64-unknown-linux-gnu";
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    if (!target) {
        std::cerr << error << std::endl;
        return false;
    }

    llvm::TargetOptions opt;
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(targetTriple, "generic", "", opt, llvm::Reloc::PIC_, std::nullopt));
    if (!targetMachine) return false;

    module->setDataLayout(targetMachine->createDataLayout());
    module->setTargetTriple(targetTriple);

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "Could not open file: " << ec.message() << std::endl;
        return false;
    }

    llvm::legacy::PassManager pass;
    llvm::CodeGenFileType fileType = llvm::CodeGenFileType::AssemblyFile;
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        std::cerr << "Target machine can't emit assembly file" << std::endl;
        return false;
    }
    pass.run(*module);
    dest.flush();
    return true;
}

} // namespace pyc
