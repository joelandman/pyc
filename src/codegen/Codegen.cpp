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
#include <unordered_set>
#include <fstream>
#include <iostream>

namespace pyc {

std::unique_ptr<llvm::Module> Codegen::generate(ModuleIR& ir, llvm::LLVMContext& context, const std::string& moduleName) {
    auto module = std::make_unique<llvm::Module>(moduleName, context);
    llvm::IRBuilder<> builder(context);

    // PyObject struct layout (must match Runtime.cpp flat struct)
    // Fields: refcount(i32), type(i32), value(i64), dvalue(double)
    llvm::StructType* pyObjectTy = llvm::StructType::create(context, {
        llvm::Type::getInt32Ty(context),    // [0] refcount
        llvm::Type::getInt32Ty(context),    // [1] type
        llvm::Type::getInt64Ty(context),    // [2] value  (int)
        llvm::Type::getDoubleTy(context),   // [3] dvalue (float)
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

    // Boxed wrappers: return/accept PyObject* so for-loops stay in the PyObject* world
    llvm::FunctionType* listSizeBoxedTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(listSizeBoxedTy, llvm::Function::ExternalLinkage, "PyList_SizeBoxed", module.get());

    llvm::FunctionType* listGetItemObjTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(listGetItemObjTy, llvm::Function::ExternalLinkage, "PyList_GetItemObj", module.get());

    llvm::FunctionType* listAppendTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(listAppendTy, llvm::Function::ExternalLinkage, "PyList_Append", module.get());

    llvm::FunctionType* listNewBoxedTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(listNewBoxedTy, llvm::Function::ExternalLinkage, "PyList_NewBoxed", module.get());

    llvm::FunctionType* listSetItemBoxedTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(listSetItemBoxedTy, llvm::Function::ExternalLinkage, "PyList_SetItemBoxed", module.get());

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
    llvm::FunctionType* unicodeFromStrTy = llvm::FunctionType::get(pyObjectPtrTy, {int8PtrTy}, false);
    llvm::Function::Create(unicodeFromStrTy, llvm::Function::ExternalLinkage, "PyUnicode_FromString", module.get());

    llvm::FunctionType* getAttrTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, int8PtrTy}, false);
    llvm::Function::Create(getAttrTy, llvm::Function::ExternalLinkage, "PyObject_GetAttr", module.get());

    llvm::FunctionType* objectPrintTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {pyObjectPtrTy, int8PtrTy}, false);
    llvm::Function::Create(objectPrintTy, llvm::Function::ExternalLinkage, "PyObject_Print", module.get());

    llvm::FunctionType* fromDoubleTy = llvm::FunctionType::get(pyObjectPtrTy, {llvm::Type::getDoubleTy(context)}, false);
    llvm::Function::Create(fromDoubleTy, llvm::Function::ExternalLinkage, "PyFloat_FromDouble", module.get());

    llvm::FunctionType* trueDivTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(trueDivTy, llvm::Function::ExternalLinkage, "PyNumber_TrueDivide", module.get());

    llvm::FunctionType* rangeTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(rangeTy, llvm::Function::ExternalLinkage, "PyBuiltin_Range", module.get());

    // int PyObject_CompareBool(PyObject* a, PyObject* b, int op)
    llvm::FunctionType* cmpBoolTy = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context),
        {pyObjectPtrTy, pyObjectPtrTy, llvm::Type::getInt32Ty(context)}, false);
    llvm::Function::Create(cmpBoolTy, llvm::Function::ExternalLinkage, "PyObject_CompareBool", module.get());

    // String operations
    llvm::FunctionType* strFromAnyTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(strFromAnyTy, llvm::Function::ExternalLinkage, "PyStr_FromAny", module.get());

    llvm::FunctionType* strConcatTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(strConcatTy, llvm::Function::ExternalLinkage, "PyString_Concat", module.get());

    llvm::FunctionType* strRepeatTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(strRepeatTy, llvm::Function::ExternalLinkage, "PyString_Repeat", module.get());

    llvm::FunctionType* builtinLenTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(builtinLenTy, llvm::Function::ExternalLinkage, "PyBuiltin_Len", module.get());

    llvm::FunctionType* printNewlineTy = llvm::FunctionType::get(pyObjectPtrTy, {}, false);
    llvm::Function::Create(printNewlineTy, llvm::Function::ExternalLinkage, "PyBuiltin_PrintNewline", module.get());

    // Builtins: min/max, list, enumerate, zip
    for (const char* name : {"PyBuiltin_MinList","PyBuiltin_MaxList",
                              "PyBuiltin_List","PyBuiltin_Enumerate"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    for (const char* name : {"PyBuiltin_Min2","PyBuiltin_Max2","PyBuiltin_Zip2"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }

    // Builtins: int, float, abs; string methods; dict/list methods
    for (const char* name : {"PyBuiltin_Int","PyBuiltin_Float","PyBuiltin_Abs",
                              "PyString_Upper","PyString_Lower","PyString_Strip",
                              "PyString_SplitWhitespace","PyDict_Keys","PyDict_Values",
                              "PyDict_Items","PyList_Sort","PyList_Pop"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    for (const char* name : {"PyString_Split","PyString_Join"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }

    // Builtins: sum, sorted, any, all; isinstance (2-arg)
    for (const char* name : {"PyBuiltin_Sum","PyBuiltin_Sorted","PyBuiltin_Any","PyBuiltin_All"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_IsInstance", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_Apply", module.get());
    }

    // String methods: find, count, replace (find/count return int boxed; replace returns str)
    for (const char* name : {"PyString_Find","PyString_Count"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_Replace", module.get());
    }

    // PyBool_New(int) — boxes a 0/1 as a bool PyObject
    llvm::FunctionType* boolNewTy = llvm::FunctionType::get(pyObjectPtrTy, {llvm::Type::getInt32Ty(context)}, false);
    llvm::Function::Create(boolNewTy, llvm::Function::ExternalLinkage, "PyBool_New", module.get());

    // Dict operations (previously missing — caused PyDict_New/SetItem calls to be silently skipped)
    llvm::FunctionType* dictNewTy = llvm::FunctionType::get(pyObjectPtrTy, {}, false);
    llvm::Function::Create(dictNewTy, llvm::Function::ExternalLinkage, "PyDict_New", module.get());

    llvm::FunctionType* dictSetItemTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(dictSetItemTy, llvm::Function::ExternalLinkage, "PyDict_SetItem", module.get());

    llvm::FunctionType* dictGetItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(dictGetItemTy, llvm::Function::ExternalLinkage, "PyDict_GetItem", module.get());

    // Subscript / membership / power
    llvm::FunctionType* getItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(getItemTy, llvm::Function::ExternalLinkage, "Pyc_GetItem", module.get());

    llvm::FunctionType* setItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(setItemTy, llvm::Function::ExternalLinkage, "Pyc_SetItem", module.get());

    llvm::FunctionType* containsTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(containsTy, llvm::Function::ExternalLinkage, "Pyc_Contains", module.get());

    llvm::FunctionType* getSliceTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(getSliceTy, llvm::Function::ExternalLinkage, "Pyc_GetSlice", module.get());

    llvm::FunctionType* setSliceTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(setSliceTy, llvm::Function::ExternalLinkage, "Pyc_SetSlice", module.get());

    llvm::FunctionType* powerTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(powerTy, llvm::Function::ExternalLinkage, "Pyc_Pow", module.get());

    // Boolean / unary ops
    llvm::FunctionType* truthBoxedTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(truthBoxedTy, llvm::Function::ExternalLinkage, "PyObject_TruthBoxed", module.get());

    llvm::FunctionType* objNotTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(objNotTy, llvm::Function::ExternalLinkage, "PyObject_Not", module.get());

    llvm::FunctionType* negateTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(negateTy, llvm::Function::ExternalLinkage, "PyNumber_Negate", module.get());

    // B5 (cells for nonlocal): declare the minimal cell primitives so lowering can emit calls.
    llvm::FunctionType* cellNewTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(cellNewTy, llvm::Function::ExternalLinkage, "PyCell_New", module.get());

    llvm::FunctionType* cellGetTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(cellGetTy, llvm::Function::ExternalLinkage, "PyCell_Get", module.get());

    llvm::FunctionType* cellSetTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(cellSetTy, llvm::Function::ExternalLinkage, "PyCell_Set", module.get());

    llvm::FunctionType* cellCheckTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(cellCheckTy, llvm::Function::ExternalLinkage, "PyCell_Check", module.get());

    // printf no longer used in normal code paths (we use PyObject_Print)

    // Create one LLVM global variable per module-level global name.
    // Each holds a PyObject* (initialised to null).
    for (const auto& gname : ir.moduleGlobals) {
        new llvm::GlobalVariable(
            *module,
            pyObjectPtrTy,
            /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(llvm::PointerType::get(context, 0)),
            "pyc_global_" + gname);
    }

    auto llvmFunctionName = [](const std::string& name) {
        if (name == "__module__") return std::string("pyc_user_main");
        if (name == "main") return std::string("pyc_py_main");
        return name;
    };

    for (const auto& f : ir.functions) {
        if (f.name.empty()) continue;  // Skip functions without names
        // The C runtime's `main` is provided by src/runtime/MainWrapper.cpp.
        // The module entry and a Python `def main` are distinct symbols.
        std::string irName = llvmFunctionName(f.name);
        std::vector<llvm::Type*> argTypes(f.args.size(), pyObjectPtrTy);
        llvm::FunctionType* funcType = llvm::FunctionType::get(pyObjectPtrTy, argTypes, false);
        llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, irName, module.get());
    }

    // B4/B8: create __apply__<name> adapters for indirect callable dispatch.
    // Each adapter takes a single PyObject* list of positional arguments (flat user list,
    // with * contents already spliced at the call site) and calls the real target after
    // shape-aware unpacking based on the target's original paramNames (which retain * markers).
    // This supports both simple indirect calls and indirect calls that use dynamic *args
    // against targets that declare *vararg in their signature.
    // Also register each adapter with the runtime so Pyc_Apply(token, list) works.
    for (const auto& f : ir.functions) {
        if (f.name.empty() || f.name == "__module__") continue;
        std::string adapterName = "__apply__" + f.name;
        llvm::FunctionType* aty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function* adapter = llvm::Function::Create(aty, llvm::Function::ExternalLinkage, adapterName, module.get());

        llvm::BasicBlock* aentry = llvm::BasicBlock::Create(context, "entry", adapter);
        llvm::IRBuilder<> abuilder(aentry);
        llvm::Value* argListVal = adapter->getArg(0);
        argListVal->setName("args");

        std::string realName = llvmFunctionName(f.name);
        llvm::Function* real = module->getFunction(realName);
        if (!real) real = module->getFunction(f.name);
        if (!real) {
            abuilder.CreateRet(llvm::ConstantPointerNull::get(pyObjectPtrTy));
            continue;
        }

        // Use original param names (with * markers) to find a *vararg slot, if any.
        const auto& pnames = f.paramNames.empty() ? f.args : f.paramNames;
        size_t vidx = (size_t)-1;
        for (size_t j = 0; j < pnames.size(); ++j) {
            if (!pnames[j].empty() && pnames[j][0] == '*') { vidx = j; break; }
        }
        bool hasVar = (vidx != (size_t)-1);
        size_t fixed = hasVar ? vidx : real->arg_size();

        std::vector<llvm::Value*> cargs;
        llvm::Function* listGet = module->getFunction("PyList_GetItem");
        llvm::Function* listSize = module->getFunction("PyList_Size");
        llvm::Function* listNew  = module->getFunction("PyList_New");
        llvm::Function* listAppend = module->getFunction("PyList_Append");

        // Leading fixed prefix (before any *vararg in the target).
        for (size_t i = 0; i < fixed; ++i) {
            llvm::Value* idx = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), i);
            llvm::Value* el = nullptr;
            if (listGet) {
                el = abuilder.CreateCall(listGet, {argListVal, idx}, "a" + std::to_string(i));
            } else {
                el = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            }
            cargs.push_back(el);
        }

        if (hasVar) {
            // Collect [fixed .. len) from the incoming flat list into a fresh list for the * slot.
            llvm::Value* ln = nullptr;
            if (listSize) {
                ln = abuilder.CreateCall(listSize, {argListVal}, "ln");
            } else {
                ln = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
            }
            llvm::Value* startC = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), fixed);
            llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
            llvm::Value* rest = nullptr;
            if (listNew) {
                rest = abuilder.CreateCall(listNew, {zero}, "rest");
            } else {
                rest = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            }

            // Inline counted loop: j = start; while j < ln { append GetItem(j); j++ }
            llvm::AllocaInst* jAlloca = abuilder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "j");
            abuilder.CreateStore(startC, jAlloca);

            llvm::BasicBlock* lp = llvm::BasicBlock::Create(context, "tail_lp", adapter);
            llvm::BasicBlock* bd = llvm::BasicBlock::Create(context, "tail_bd", adapter);
            llvm::BasicBlock* ex = llvm::BasicBlock::Create(context, "tail_ex", adapter);
            abuilder.CreateBr(lp);
            abuilder.SetInsertPoint(lp);
            llvm::Value* jcur = abuilder.CreateLoad(llvm::Type::getInt64Ty(context), jAlloca, "jcur");
            llvm::Value* cmp = abuilder.CreateICmpSLT(jcur, ln, "cm");
            abuilder.CreateCondBr(cmp, bd, ex);
            abuilder.SetInsertPoint(bd);
            llvm::Value* el = nullptr;
            if (listGet) {
                el = abuilder.CreateCall(listGet, {argListVal, jcur}, "el");
            } else {
                el = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            }
            if (listAppend && rest) {
                abuilder.CreateCall(listAppend, {rest, el});
            }
            llvm::Value* one = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1);
            llvm::Value* jn = abuilder.CreateAdd(jcur, one, "jn");
            abuilder.CreateStore(jn, jAlloca);
            abuilder.CreateBr(lp);
            abuilder.SetInsertPoint(ex);

            cargs.push_back(rest ? rest : llvm::ConstantPointerNull::get(pyObjectPtrTy));
        }

        llvm::Value* r = abuilder.CreateCall(real, cargs, "r");
        abuilder.CreateRet(r);
    }

    // Declare the registration function so we can call it from the module ctor.
    llvm::FunctionType* regTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {int8PtrTy, pyObjectPtrTy}, false);
    llvm::Function* regFn = module->getFunction("pyc_register_callable");
    if (!regFn) regFn = llvm::Function::Create(regTy, llvm::Function::ExternalLinkage, "pyc_register_callable", module.get());
    llvm::Function* regCallable = regFn; // local alias used in the per-function registration below

    for (const auto& f : ir.functions) {
        std::string irName = llvmFunctionName(f.name);
        llvm::Function* func = module->getFunction(irName);
        if (!func) continue;
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", func);
        builder.SetInsertPoint(entry);

        // B4/B8: register all __apply__ adapters at module startup so Pyc_Apply can
        // dispatch to user functions and lambdas by their synthetic/user name token.
        if (f.name == "__module__") {
            for (const auto& rf : ir.functions) {
                if (rf.name.empty() || rf.name == "__module__") continue;
                std::string adapterName = "__apply__" + rf.name;
                llvm::Function* adp = module->getFunction(adapterName);
                if (!adp) continue;
                llvm::Value* nameStr = builder.CreateGlobalStringPtr(rf.name, "reg." + rf.name);
                // regFn is the registration function declared above.
                if (regFn) {
                    builder.CreateCall(regFn, {nameStr, adp});
                }
            }
        }

        std::unordered_map<std::string, llvm::Value*> valueMap;
        std::unordered_set<std::string> ownedSlots;
        for (size_t i = 0; i < f.args.size(); ++i) {
            llvm::Value* arg = func->getArg(i);
            if (!f.args[i].empty()) {
                arg->setName(f.args[i]);
                // For parameters, create an entry-block alloca that shadows
                // the parameter, and add the *alloca* to valueMap. This way
                // subsequent assigns to the parameter name write to the
                // alloca (and can be observed by future loads), and
                // initial reads return the parameter value. The alloca is
                // initialised in the entry block so it dominates all uses.
                llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                              func->getEntryBlock().begin());
                llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, f.args[i] + ".slot");
                entryBuilder.CreateStore(arg, alloca);
                valueMap[f.args[i]] = alloca;
            } else {
                // Use a synthetic name if args are empty
                std::string synthName = "arg" + std::to_string(i);
                arg->setName(synthName);
                valueMap[synthName] = arg;
            }
        }
        // Pre-populate global variables (after params so params shadow globals).
        for (const auto& gname : f.globalVars) {
            if (valueMap.count(gname)) continue;   // param with same name — skip
            llvm::GlobalVariable* gv = module->getNamedGlobal("pyc_global_" + gname);
            if (gv) valueMap[gname] = gv;
        }

        // B5 (cells): pre-populate slots for cell-backed parameters (hidden leading args
        // named "<pythonname>_cell"). Treat them as normal PyObject* cell objects.
        // Also create local cell slots for any owned cell names declared in f.cellVars
        // so that loads/stores via PyCell_Get/PyCell_Set can find them in valueMap.
        for (const auto& cname : f.freeCellVars) {
            // freeCellVars holds Python names; the actual parameter names are "<python>_cell"
            std::string slot = cname + "_cell";
            if (valueMap.count(slot)) continue;
            // The parameter itself is a PyObject* (the cell). Create an entry alloca for it.
            llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                           func->getEntryBlock().begin());
            llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, slot + ".slot");
            // The real argument will be wired by the caller; here we just reserve the slot name.
            // The actual incoming cell arg will be matched by LLVM arg position; for safety,
            // if a parameter with this exact name exists, store it into the alloca.
            // (Codegen arg loop above already handled named args by the same name.)
            valueMap[slot] = alloca;
        }
        for (const auto& cname : f.cellVars) {
            std::string slot = cname + "_cell";
            if (valueMap.count(slot)) continue;
            llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                           func->getEntryBlock().begin());
            llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, slot + ".slot");
            valueMap[slot] = alloca;
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
            // Special-case `sys` so user code can do `sys.argv` etc. The
            // runtime provides a `pyc_get_sys_module()` accessor that
            // returns the same global `sys` object every call.
            if (name == "__name__") {
                llvm::Function* fromStr = module->getFunction("PyUnicode_FromString");
                llvm::Value* strConst = builder.CreateGlobalStringPtr("__main__", "str");
                return builder.CreateCall(fromStr, {strConst}, name + ".name");
            }
            if (name == "sys") {
                llvm::Function* getSys = module->getFunction("pyc_get_sys_module");
                if (!getSys) {
                    llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {}, false);
                    getSys = llvm::Function::Create(ty, llvm::Function::ExternalLinkage,
                                                   "pyc_get_sys_module", module.get());
                }
                return builder.CreateCall(getSys, {}, name + ".sys");
            }
            auto it = valueMap.find(name);
            if (it == valueMap.end()) {
                llvm::GlobalVariable* gv = module->getNamedGlobal("pyc_global_" + name);
                if (gv) {
                    valueMap[name] = gv;
                    return builder.CreateLoad(pyObjectPtrTy, gv, name + ".load");
                }
                return llvm::ConstantPointerNull::get(llvm::PointerType::get(context, 0));
            }
            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second))
                return builder.CreateLoad(alloca->getAllocatedType(), alloca, name + ".load");
            if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(it->second))
                return builder.CreateLoad(pyObjectPtrTy, gv, name + ".load");
            return it->second;
        };

        // Helper: unbox a PyObject* (assumed to be int) to i64
        auto unboxToI64 = [&](llvm::Value* boxed) -> llvm::Value* {
            if (!boxed || boxed->getType() == llvm::Type::getInt64Ty(context)) return boxed;
            return builder.CreateLoad(llvm::Type::getInt64Ty(context),
                builder.CreateStructGEP(pyObjectTy, boxed, 2), "unboxed");
        };

        auto boxI64 = [&](llvm::Value* val, const std::string& name = "") -> llvm::Value* {
            llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
            if (!fromLong) return llvm::ConstantPointerNull::get(pyObjectPtrTy);
            return builder.CreateCall(fromLong, {val}, name);
        };

        // Return the value for 'name' as a PyObject* suitable for Python-visible
        // contexts (calls, returns, containers, etc.). If the name is backed by
        // native i64 or double (from range loop vars or numeric ops), box on demand.
        auto getAsPyObject = [&](const std::string& name) -> llvm::Value* {
            llvm::Value* v = getOrLoad(name);
            if (!v) return llvm::ConstantPointerNull::get(pyObjectPtrTy);
            if (v->getType() == llvm::Type::getInt64Ty(context)) {
                return boxI64(v, name + ".boxed");
            }
            if (v->getType()->isDoubleTy()) {
                llvm::Function* fromDouble = module->getFunction("PyFloat_FromDouble");
                if (!fromDouble) return llvm::ConstantPointerNull::get(pyObjectPtrTy);
                return builder.CreateCall(fromDouble, {v}, name + ".boxed");
            }
            return v;
        };

        auto unboxToDouble = [&](llvm::Value* boxed) -> llvm::Value* {
            llvm::Type* doubleTy = llvm::Type::getDoubleTy(context);
            if (!boxed || boxed->getType() == doubleTy) return boxed;
            if (boxed->getType() == llvm::Type::getInt64Ty(context)) {
                return builder.CreateSIToFP(boxed, doubleTy, "i64.to.double");
            }

            llvm::Value* typeVal = builder.CreateLoad(llvm::Type::getInt32Ty(context),
                builder.CreateStructGEP(pyObjectTy, boxed, 1), "boxed.type");
            llvm::Value* intVal = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                builder.CreateStructGEP(pyObjectTy, boxed, 2), "boxed.int");
            llvm::Value* intAsDouble = builder.CreateSIToFP(intVal, doubleTy, "boxed.int.double");
            llvm::Value* doubleVal = builder.CreateLoad(doubleTy,
                builder.CreateStructGEP(pyObjectTy, boxed, 3), "boxed.double");
            llvm::Value* isFloat = builder.CreateICmpEQ(typeVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 4), "boxed.isfloat");
            return builder.CreateSelect(isFloat, doubleVal, intAsDouble, "unboxed.double");
        };

        auto boxDouble = [&](llvm::Value* val, const std::string& name = "") -> llvm::Value* {
            llvm::Function* fromDouble = module->getFunction("PyFloat_FromDouble");
            if (!fromDouble) return llvm::ConstantPointerNull::get(pyObjectPtrTy);
            return builder.CreateCall(fromDouble, {val}, name);
        };

        auto emitNativeNumericBinary = [&](const IRInstruction& inst,
                                           const std::string& op) -> bool {
            if (inst.operands.size() < 2) return false;
            if (inst.resultType == "int") {
                llvm::Value* lhs = unboxToI64(getOrLoad(inst.operands[0].name));
                llvm::Value* rhs = unboxToI64(getOrLoad(inst.operands[1].name));
                llvm::Value* native = nullptr;
                if (op == "add") {
                    native = builder.CreateAdd(lhs, rhs, inst.result + ".i64");
                } else if (op == "sub") {
                    native = builder.CreateSub(lhs, rhs, inst.result + ".i64");
                } else if (op == "mul") {
                    native = builder.CreateMul(lhs, rhs, inst.result + ".i64");
                } else {
                    return false;
                }
                // Store the native i64 in the result temp to allow longer-lived
                // unboxed numeric values inside numeric regions (A2). Consumers
                // that need a PyObject* (calls, returns, containers, stores to
                // boxed locals, etc.) go through getAsPyObject or explicit box.
                valueMap[inst.result] = native;
                return true;
            }
            if (inst.resultType == "float") {
                llvm::Value* lhs = unboxToDouble(getOrLoad(inst.operands[0].name));
                llvm::Value* rhs = unboxToDouble(getOrLoad(inst.operands[1].name));
                llvm::Value* native = nullptr;
                if (op == "add") {
                    native = builder.CreateFAdd(lhs, rhs, inst.result + ".double");
                } else if (op == "sub") {
                    native = builder.CreateFSub(lhs, rhs, inst.result + ".double");
                } else if (op == "mul") {
                    native = builder.CreateFMul(lhs, rhs, inst.result + ".double");
                } else {
                    return false;
                }
                valueMap[inst.result] = native;
                return true;
            }
            return false;
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
                // Dispatch to PyObject_CompareBool so both int and float work.
                // op codes: 0=Eq, 1=NotEq, 2=Lt, 3=Gt, 4=LtE, 5=GtE
                std::string opstr = inst.operands.empty() ? "" : inst.operands[0].name;
                int opcode = 0;
                if      (opstr == "Eq"    || opstr == "eq") opcode = 0;
                else if (opstr == "NotEq" || opstr == "ne") opcode = 1;
                else if (opstr == "Lt"    || opstr == "lt") opcode = 2;
                else if (opstr == "Gt"    || opstr == "gt") opcode = 3;
                else if (opstr == "LtE")                    opcode = 4;
                else if (opstr == "GtE")                    opcode = 5;

                llvm::Value* lhsBox = getAsPyObject(inst.operands.size() > 1 ? inst.operands[1].name : "");
                llvm::Value* rhsBox = getAsPyObject(inst.operands.size() > 2 ? inst.operands[2].name : "");

                llvm::Function* cmpFn  = module->getFunction("PyObject_CompareBool");
                llvm::Function* boolNew = module->getFunction("PyBool_New");
                llvm::Value* boxedCmp = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                if (cmpFn && boolNew) {
                    // PyObject_CompareBool returns i32 (0 or 1); PyBool_New takes i32
                    llvm::Value* cmpResult = builder.CreateCall(cmpFn, {
                        lhsBox, rhsBox,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), opcode)
                    });
                    boxedCmp = builder.CreateCall(boolNew, {cmpResult}, inst.result);
                }
                valueMap[inst.result] = boxedCmp;
                continue;
            }
            if (inst.op == "i64const") {
                long v = std::stol(inst.operands.empty() ? "0" : inst.operands[0].name);
                valueMap[inst.result] = llvm::ConstantInt::get(context, llvm::APInt(64, v));
            } else if (inst.op == "i64_from_box") {
                llvm::Value* boxed = getOrLoad(inst.operands.empty() ? "" : inst.operands[0].name);
                valueMap[inst.result] = unboxToI64(boxed);
            } else if (inst.op == "box_i64") {
                llvm::Value* val = getOrLoad(inst.operands.empty() ? "" : inst.operands[0].name);
                valueMap[inst.result] = boxI64(val, inst.result);
            } else if (inst.op == "i64add") {
                llvm::Value* lhs = getOrLoad(inst.operands[0].name);
                llvm::Value* rhs = getOrLoad(inst.operands[1].name);
                valueMap[inst.result] = builder.CreateAdd(lhs, rhs, inst.result);
            } else if (inst.op == "i64icmp") {
                std::string opstr = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* lhs = getOrLoad(inst.operands.size() > 1 ? inst.operands[1].name : "");
                llvm::Value* rhs = getOrLoad(inst.operands.size() > 2 ? inst.operands[2].name : "");
                llvm::Value* cmp = nullptr;
                if      (opstr == "Eq"    || opstr == "eq") cmp = builder.CreateICmpEQ(lhs, rhs, inst.result);
                else if (opstr == "NotEq" || opstr == "ne") cmp = builder.CreateICmpNE(lhs, rhs, inst.result);
                else if (opstr == "Lt"    || opstr == "lt") cmp = builder.CreateICmpSLT(lhs, rhs, inst.result);
                else if (opstr == "Gt"    || opstr == "gt") cmp = builder.CreateICmpSGT(lhs, rhs, inst.result);
                else if (opstr == "LtE")                    cmp = builder.CreateICmpSLE(lhs, rhs, inst.result);
                else if (opstr == "GtE")                    cmp = builder.CreateICmpSGE(lhs, rhs, inst.result);
                else                                         cmp = builder.CreateICmpNE(lhs, rhs, inst.result);
                valueMap[inst.result] = cmp;
            } else if (inst.op == "i64assign") {
                llvm::Value* newVal = getOrLoad(inst.operands.empty() ? "" : inst.operands[0].name);
                auto it = valueMap.find(inst.result);
                if (it == valueMap.end()) {
                    llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                                  func->getEntryBlock().begin());
                    llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, inst.result);
                    valueMap[inst.result] = alloca;
                    builder.CreateStore(newVal, alloca);
                } else {
                    builder.CreateStore(newVal, it->second);
                }
            } else if (inst.op == "const") {
                std::string val = inst.operands.empty() ? "0" : inst.operands[0].name;
                if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
                    llvm::Function* fromStr = module->getFunction("PyUnicode_FromString");
                    if (fromStr) {
                        std::string s = val.substr(1, val.size() - 2);
                        llvm::Value* strConst = builder.CreateGlobalStringPtr(s, "str");
                        llvm::Value* boxed = builder.CreateCall(fromStr, {strConst}, inst.result);
                        valueMap[inst.result] = boxed;
                    }
                } else {
                    long v = std::stol(val);
                    llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
                    if (fromLong) {
                        llvm::Value* boxed = builder.CreateCall(fromLong,
                            {llvm::ConstantInt::get(context, llvm::APInt(64, v))}, inst.result);
                        valueMap[inst.result] = boxed;
                    }
                }
            } else if (inst.op == "bconst") {
                std::string val = inst.operands.empty() ? "False" : inst.operands[0].name;
                int bval = (val == "True") ? 1 : 0;
                llvm::Function* boolNew = module->getFunction("PyBool_New");
                if (boolNew) {
                    llvm::Value* boxed = builder.CreateCall(boolNew,
                        {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), bval)}, inst.result);
                    valueMap[inst.result] = boxed;
                }
            } else if (inst.op == "fconst") {
                double v = std::stod(inst.operands.empty() ? "0" : inst.operands[0].name);
                llvm::Function* fromDouble = module->getFunction("PyFloat_FromDouble");
                if (fromDouble) {
                    llvm::Value* boxed = builder.CreateCall(fromDouble,
                        {llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), v)}, inst.result);
                    valueMap[inst.result] = boxed;
                }
            } else if (inst.op == "add") {
                if (emitNativeNumericBinary(inst, "add")) continue;
                llvm::Function* numberAdd = module->getFunction("PyNumber_Add");
                llvm::Value* lhs = getAsPyObject(inst.operands[0].name);
                llvm::Value* rhs = getAsPyObject(inst.operands[1].name);
                if (numberAdd) {
                    llvm::Value* sum = builder.CreateCall(numberAdd, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = sum;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "sub") {
                if (emitNativeNumericBinary(inst, "sub")) continue;
                llvm::Function* numberSub = module->getFunction("PyNumber_Subtract");
                llvm::Value* lhs = getAsPyObject(inst.operands[0].name);
                llvm::Value* rhs = getAsPyObject(inst.operands[1].name);
                if (numberSub) {
                    llvm::Value* diff = builder.CreateCall(numberSub, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = diff;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "div") {
                if (inst.resultType == "int") {
                    llvm::Value* lhs = unboxToI64(getOrLoad(inst.operands[0].name));
                    llvm::Value* rhs = unboxToI64(getOrLoad(inst.operands[1].name));
                    // Guard div-by-zero: fall back to boxed (runtime returns NULL)
                    llvm::Value* isZero = builder.CreateICmpEQ(rhs, llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Function* numberDiv = module->getFunction("PyNumber_Divide");
                    llvm::Value* boxedL = getAsPyObject(inst.operands[0].name);
                    llvm::Value* boxedR = getAsPyObject(inst.operands[1].name);
                    llvm::Value* quot = nullptr;
                    if (numberDiv) {
                        quot = builder.CreateCall(numberDiv, {boxedL, boxedR}, inst.result + ".boxed");
                    } else {
                        quot = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                    }
                    // Native floor path (only if rhs != 0)
                    llvm::Value* q = builder.CreateSDiv(lhs, rhs);
                    llvm::Value* r = builder.CreateSRem(lhs, rhs);
                    llvm::Value* signsDiffer = builder.CreateICmpSLT(builder.CreateXor(lhs, rhs), llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Value* hasRem = builder.CreateICmpNE(r, llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Value* needAdjust = builder.CreateAnd(signsDiffer, hasRem);
                    llvm::Value* one = llvm::ConstantInt::get(context, llvm::APInt(64, 1));
                    llvm::Value* qAdj = builder.CreateSub(q, one);
                    q = builder.CreateSelect(needAdjust, qAdj, q);
                    llvm::Value* nativeRes = builder.CreateSelect(isZero, quot, boxI64(q, inst.result + ".i64"), inst.result);
                    valueMap[inst.result] = nativeRes;
                    continue;
                }
                // float or unknown -> boxed
                llvm::Function* numberDiv = module->getFunction("PyNumber_Divide");
                llvm::Value* lhs = getAsPyObject(inst.operands[0].name);
                llvm::Value* rhs = getAsPyObject(inst.operands[1].name);
                if (numberDiv) {
                    llvm::Value* quot = builder.CreateCall(numberDiv, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = quot;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "pow") {
                llvm::Function* fn = module->getFunction("Pyc_Pow");
                llvm::Value* lhs = getAsPyObject(inst.operands[0].name);
                llvm::Value* rhs = getAsPyObject(inst.operands[1].name);
                if (fn) valueMap[inst.result] = builder.CreateCall(fn, {lhs, rhs}, inst.result);
                else    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            } else if (inst.op == "truediv") {
                llvm::Function* numberTrueDiv = module->getFunction("PyNumber_TrueDivide");
                llvm::Value* lhs = getAsPyObject(inst.operands[0].name);
                llvm::Value* rhs = getAsPyObject(inst.operands[1].name);
                if (numberTrueDiv) {
                    llvm::Value* quot = builder.CreateCall(numberTrueDiv, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = quot;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "mod") {
                llvm::Function* numberRem = module->getFunction("PyNumber_Remainder");
                llvm::Value* lhs = getAsPyObject(inst.operands[0].name);
                llvm::Value* rhs = getAsPyObject(inst.operands[1].name);
                if (numberRem) {
                    llvm::Value* rem = builder.CreateCall(numberRem, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = rem;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "mul") {
                if (emitNativeNumericBinary(inst, "mul")) continue;
                llvm::Function* numberMult = module->getFunction("PyNumber_Multiply");
                llvm::Value* lhs = getAsPyObject(inst.operands[0].name);
                llvm::Value* rhs = getAsPyObject(inst.operands[1].name);
                if (numberMult) {
                    llvm::Value* prod = builder.CreateCall(numberMult, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = prod;
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "neg") {
                // Unary minus. For proven numeric resultType, keep native result (i64/double)
                // so it can participate in further unboxed arithmetic (A3 widening).
                // Box only on escape via getAsPyObject.
                if (inst.resultType == "int") {
                    llvm::Value* v = unboxToI64(getOrLoad(inst.operands.empty() ? "" : inst.operands[0].name));
                    llvm::Value* n = builder.CreateNeg(v, inst.result + ".i64");
                    valueMap[inst.result] = n;  // native i64 for longer unboxed life
                    continue;
                }
                if (inst.resultType == "float") {
                    llvm::Value* v = unboxToDouble(getOrLoad(inst.operands.empty() ? "" : inst.operands[0].name));
                    llvm::Value* n = builder.CreateFNeg(v, inst.result + ".double");
                    valueMap[inst.result] = n;
                    continue;
                }
                // Fallback: boxed runtime path
                llvm::Function* fn = module->getFunction("PyNumber_Negate");
                llvm::Value* arg = getAsPyObject(inst.operands.empty() ? "" : inst.operands[0].name);
                if (fn) {
                    valueMap[inst.result] = builder.CreateCall(fn, {arg}, inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "getattr") {
                llvm::Function* getAttr = module->getFunction("PyObject_GetAttr");
                llvm::Value* obj = getOrLoad(inst.operands[0].name);
                if (getAttr) {
                    llvm::Value* attrName = builder.CreateGlobalStringPtr(inst.operands[1].name, "attr");
                    llvm::Value* result = builder.CreateCall(getAttr, {obj, attrName}, inst.result);
                    valueMap[inst.result] = result;
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
                llvm::Value* src = getOrLoad(inst.operands[0].name);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(context);

                // If target currently has an i64 slot (e.g. range loop visible var),
                // decide whether to keep native storage or switch to boxed for this assign.
                auto tit = valueMap.find(inst.result);
                if (tit != valueMap.end()) {
                    if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(tit->second)) {
                        if (alloca->getAllocatedType() == i64Ty) {
                            if (src->getType() == i64Ty) {
                                builder.CreateStore(src, alloca);
                            } else {
                                // src is boxed (general expr). Switch this name's storage
                                // to a fresh PyObject* slot so arbitrary values (incl. str)
                                // are supported for the rest of the scope. The range
                                // machinery (if active) will publish by boxing into the
                                // current slot on next update.
                                llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                                              func->getEntryBlock().begin());
                                llvm::AllocaInst* newAlloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, inst.result + ".slot");
                                llvm::Value* toStore = src;
                                if (toStore->getType() == i64Ty) toStore = boxI64(toStore);
                                builder.CreateStore(toStore, newAlloca);
                                valueMap[inst.result] = newAlloca;
                            }
                            continue;
                        }
                    }
                }

                // Normal path: target expects PyObject*. Ensure src is boxed.
                llvm::Value* newVal = src;
                if (newVal->getType() == i64Ty) {
                    newVal = boxI64(newVal, inst.operands[0].name + ".boxed");
                } else if (newVal->getType()->isDoubleTy()) {
                    newVal = boxDouble(newVal, inst.operands[0].name + ".boxed");
                }

               // Basic refcounting for variables: variables own a reference.
                // For module globals we never DECREF the old value (the module
                // owns them). For alloca-backed slots (params and user-assigned
                // locals), DECREF the old value on reassignment.
                if (valueMap.find(inst.result) != valueMap.end()) {
                    llvm::Value* oldVal = builder.CreateLoad(pyObjectPtrTy, valueMap[inst.result], inst.result + ".old");
                    if (llvm::GlobalVariable* gv = llvm::dyn_cast<llvm::GlobalVariable>(valueMap[inst.result])) {
                        // Module global: never DECREF (the module owns this ref)
                        builder.CreateStore(newVal, gv);
                    } else if (ownedSlots.find(inst.result) != ownedSlots.end()) {
                        // Owned alloca slot: DECREF the old value before overwrite
                        llvm::Function* decref = module->getFunction("Py_DECREF");
                        if (decref) builder.CreateCall(decref, {oldVal});
                        builder.CreateStore(newVal, valueMap[inst.result]);
                    } else {
                        // Borrowed alloca slot (function param, cell, etc.):
                        // do not DECREF the initial borrowed value.
                        builder.CreateStore(newVal, valueMap[inst.result]);
                    }
                } else {
                    if (llvm::GlobalVariable* gv = module->getNamedGlobal("pyc_global_" + inst.result)) {
                        valueMap[inst.result] = gv;
                        builder.CreateStore(newVal, gv);
                        continue;
                    }
                    // First time we see this name: create the alloca in the
                    // ENTRY block so it dominates all uses (including uses
                    // that survive past loops or branches). Skip the INCREF
                    // on this first definition — the alloca starts
                    // uninitialised and the upcoming store fills it.
                    llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                                   func->getEntryBlock().begin());
                    llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, inst.result);
                    valueMap[inst.result] = alloca;
                    ownedSlots.insert(inst.result);
                    builder.CreateStore(newVal, alloca);
                    continue;
                }

                // INCREF the new value (variable takes ownership of the new
                // value's reference; for params/globals this gives us a
                // borrowed-style reference that we'll either reassign or
                // not DECREF on exit).
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
                        llvm::Value* arg = (inst.operands.size() > 1 ? getAsPyObject(inst.operands[1].name) : llvm::ConstantPointerNull::get(pyObjectPtrTy));
                        // Pass null — our runtime tolerates it and uses real stdout
                        builder.CreateCall(pyPrint, {arg, llvm::ConstantPointerNull::get(int8PtrTy)});
                    }
                } else {
                    llvm::Function* callee = module->getFunction(funcName);
                    if (!callee) callee = module->getFunction(llvmFunctionName(funcName));
                    if (callee) {
                        std::vector<llvm::Value*> callArgs;
                        for (size_t i = 1; i < inst.operands.size(); ++i) {
                            callArgs.push_back(getAsPyObject(inst.operands[i].name));
                        }
                        if (callee->getReturnType()->isVoidTy()) {
                            builder.CreateCall(callee, callArgs);
                        } else {
                            llvm::Value* callRes = builder.CreateCall(callee, callArgs, inst.result);
                            valueMap[inst.result] = callRes;
                        }
                    }
                }
            } else if (inst.op == "ret") {
                std::string retName = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* retVal = getAsPyObject(retName);
                // If we couldn't find a value, return a boxed 0 instead of null
                if (retVal == llvm::ConstantPointerNull::get(pyObjectPtrTy)) {
                    llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
                    if (fromLong) {
                        retVal = builder.CreateCall(fromLong, {llvm::ConstantInt::get(context, llvm::APInt(64, 0))});
                    }
                }
                if (!curBlock->getTerminator())
                    builder.CreateRet(retVal);
                // No break — continue processing labels/branches for other paths
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

        // Small helper to box an i64 value on demand (used by some numeric
        // binary paths and for range loop visible var boxing on escape).
        (void)boxI64; // already defined above; keep reference for future native paths.
    }
    if (llvm::verifyModule(*module, &llvm::errs())) {
        std::cerr << "Module verification failed\n";
        return nullptr;
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
