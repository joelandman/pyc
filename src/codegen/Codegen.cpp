#include "pyc/Codegen.h"
#include <cctype>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/LoopDataPrefetch.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>
#include <llvm/Transforms/Scalar/LoopInterchange.h>
#include <llvm/Transforms/Scalar/LoopRotation.h>
#include <llvm/Transforms/Scalar/LoopUnrollAndJamPass.h>
#include <llvm/Transforms/Scalar/LoopVersioningLICM.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/Inliner.h>
#include <llvm/Transforms/IPO/SampleProfile.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>
#include <llvm/ProfileData/InstrProfWriter.h>
#include <llvm/Passes/PassBuilder.h>
// #include <llvm/Passes/PassPlugin.h>  // Removed: header not available in LLVM 22
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/PGOOptions.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Support/MemoryBuffer.h>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <iostream>
#include <filesystem>

namespace pyc {

std::unique_ptr<llvm::Module> Codegen::generate(ModuleIR& ir, llvm::LLVMContext& context, const std::string& moduleName, bool debugInfo) {
    auto module = std::make_unique<llvm::Module>(moduleName, context);
    llvm::IRBuilder<> builder(context);

    // Debug info setup
    std::unique_ptr<llvm::DIBuilder> diBuilder;
    llvm::DICompileUnit* diCU = nullptr;
    llvm::DIFile* diFile = nullptr;
    llvm::DISubroutineType* diSubroutineType = nullptr;
    if (debugInfo) {
        diBuilder = std::make_unique<llvm::DIBuilder>(*module);
        // Derive source file from the first function that has a sourceFile,
        // or from the module name.
        std::string srcFile = "pyc_input.py";
        std::string srcDir = ".";
        for (const auto& f : ir.functions) {
            if (!f.sourceFile.empty()) {
                std::filesystem::path p(f.sourceFile);
                srcFile = p.filename().string();
                srcDir = p.parent_path().empty() ? "." : p.parent_path().string();
                break;
            }
        }
        diFile = diBuilder->createFile(srcFile, srcDir);
        diCU = diBuilder->createCompileUnit(
            llvm::dwarf::DW_LANG_C_plus_plus,
            diFile,
            "pyc",
            false,       // isOptimized
            "",          // flags
            0);          // runtime version
        // Subroutine type: void() — used as a generic type for all functions.
        // The actual parameter types are not reflected in debug info (the
        // debugger sees PyObject* / i64 / double, not Python types).
        diSubroutineType = diBuilder->createSubroutineType(
            diBuilder->getOrCreateTypeArray({nullptr}));
        module->addModuleFlag(llvm::Module::Error, "Dwarf Version", 5);
        module->addModuleFlag(llvm::Module::Error, "Debug Info Version", 3);
    }

    // I-009: fallback source path for functions whose IR sourceFile is empty
    // (__module__ is never given one by lowering). Prefer the first real
    // sourceFile (same scan DI uses), else ir.moduleName + ".py", else
    // "<unknown>". The test harness accepts the compiled path or its basename.
    std::string tbFallbackFile = "<unknown>";
    for (const auto& f : ir.functions) {
        if (!f.sourceFile.empty()) {
            tbFallbackFile = f.sourceFile;
            break;
        }
    }
    if (tbFallbackFile == "<unknown>" && !ir.moduleName.empty()) {
        tbFallbackFile = ir.moduleName + ".py";
    }

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
    llvm::FunctionType* listGetItemI64Ty = llvm::FunctionType::get(
        pyObjectPtrTy, {pyObjectPtrTy, llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(listGetItemI64Ty, llvm::Function::ExternalLinkage, "PyList_GetItemI64", module.get());
    llvm::FunctionType* listSizeI64Ty = llvm::FunctionType::get(
        llvm::Type::getInt64Ty(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(listSizeI64Ty, llvm::Function::ExternalLinkage, "PyList_SizeI64", module.get());
    // Unpack2/3: (list, out0*, out1*[, out2*]) -> i32
    llvm::Type* ptrPtrTy = llvm::PointerType::get(context, 0);
    llvm::FunctionType* unpack2Ty = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context), {pyObjectPtrTy, ptrPtrTy, ptrPtrTy}, false);
    llvm::Function::Create(unpack2Ty, llvm::Function::ExternalLinkage, "PyList_Unpack2", module.get());
    llvm::FunctionType* unpack3Ty = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context), {pyObjectPtrTy, ptrPtrTy, ptrPtrTy, ptrPtrTy}, false);
    llvm::Function::Create(unpack3Ty, llvm::Function::ExternalLinkage, "PyList_Unpack3", module.get());

    llvm::FunctionType* listAppendTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(listAppendTy, llvm::Function::ExternalLinkage, "PyList_Append", module.get());

    llvm::FunctionType* listNewBoxedTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(listNewBoxedTy, llvm::Function::ExternalLinkage, "PyList_NewBoxed", module.get());
    llvm::Function::Create(listNewBoxedTy, llvm::Function::ExternalLinkage, "PyList_NewIntBoxed", module.get());
    llvm::Function::Create(listNewBoxedTy, llvm::Function::ExternalLinkage, "PyList_NewFloatBoxed", module.get());

    llvm::FunctionType* listSetItemBoxedTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(listSetItemBoxedTy, llvm::Function::ExternalLinkage, "PyList_SetItemBoxed", module.get());

    llvm::FunctionType* listGetItemInt64Ty = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {pyObjectPtrTy, llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(listGetItemInt64Ty, llvm::Function::ExternalLinkage, "PyList_GetItemInt64", module.get());

    llvm::FunctionType* listGetItemDoubleTy = llvm::FunctionType::get(llvm::Type::getDoubleTy(context), {pyObjectPtrTy, llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(listGetItemDoubleTy, llvm::Function::ExternalLinkage, "PyList_GetItemDouble", module.get());

    llvm::FunctionType* listSetItemInt64Ty = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, llvm::Type::getInt64Ty(context), llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(listSetItemInt64Ty, llvm::Function::ExternalLinkage, "PyList_SetItemInt64", module.get());

    llvm::FunctionType* listSetItemDoubleTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, llvm::Type::getInt64Ty(context), llvm::Type::getDoubleTy(context)}, false);
    llvm::Function::Create(listSetItemDoubleTy, llvm::Function::ExternalLinkage, "PyList_SetItemDouble", module.get());
    llvm::Function::Create(listSetItemDoubleTy, llvm::Function::ExternalLinkage, "PyList_SetItemDoubleAuto", module.get());

    llvm::FunctionType* listSetItemInt64AutoTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, llvm::Type::getInt64Ty(context), llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(listSetItemInt64AutoTy, llvm::Function::ExternalLinkage, "PyList_SetItemInt64Auto", module.get());

    llvm::FunctionType* listSetItemTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, llvm::Type::getInt64Ty(context), pyObjectPtrTy}, false);
    llvm::Function::Create(listSetItemTy, llvm::Function::ExternalLinkage, "PyList_SetItem", module.get());

    // --- tuple type (type 7) ---
    llvm::FunctionType* tupleNewTy = llvm::FunctionType::get(pyObjectPtrTy, {llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(tupleNewTy, llvm::Function::ExternalLinkage, "PyTuple_New", module.get());
    llvm::FunctionType* tupleNewBoxedTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(tupleNewBoxedTy, llvm::Function::ExternalLinkage, "PyTuple_NewBoxed", module.get());
    llvm::FunctionType* tupleSetItemTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, llvm::Type::getInt64Ty(context), pyObjectPtrTy}, false);
    llvm::Function::Create(tupleSetItemTy, llvm::Function::ExternalLinkage, "PyTuple_SetItem", module.get());
    llvm::FunctionType* tupleSetItemBoxedTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(tupleSetItemBoxedTy, llvm::Function::ExternalLinkage, "PyTuple_SetItemBoxed", module.get());
    llvm::FunctionType* tupleBuiltinTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(tupleBuiltinTy, llvm::Function::ExternalLinkage, "PyBuiltin_Tuple", module.get());

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

    // A8: String formatting
    llvm::FunctionType* stringFormatTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(stringFormatTy, llvm::Function::ExternalLinkage, "PyString_Format", module.get());

    llvm::FunctionType* decrefTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(decrefTy, llvm::Function::ExternalLinkage, "Py_DECREF", module.get());

    llvm::FunctionType* increfTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(increfTy, llvm::Function::ExternalLinkage, "Py_INCREF", module.get());

    llvm::PointerType* int8PtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
    llvm::FunctionType* unicodeFromStrTy = llvm::FunctionType::get(pyObjectPtrTy, {int8PtrTy}, false);
    llvm::Function::Create(unicodeFromStrTy, llvm::Function::ExternalLinkage, "PyUnicode_FromString", module.get());

    // Length-aware constructor: "const" str literals with interior NUL
    // (and chr(0)) must not go through strlen. Same (ptr, i64) shape as
    // PyBytes_FromStringAndSize used by "bytesconst".
    llvm::FunctionType* unicodeFromStrAndSizeTy = llvm::FunctionType::get(pyObjectPtrTy,
        {int8PtrTy, llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(unicodeFromStrAndSizeTy, llvm::Function::ExternalLinkage,
                           "PyUnicode_FromStringAndSize", module.get());

    llvm::FunctionType* getAttrTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, int8PtrTy}, false);
    llvm::Function::Create(getAttrTy, llvm::Function::ExternalLinkage, "PyObject_GetAttr", module.get());

    llvm::FunctionType* callTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(callTy, llvm::Function::ExternalLinkage, "PyObject_Call", module.get());

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

    // int PyObject_TruthValue(PyObject* obj) — used by the "br" handler
    // below for boxed non-numeric conditions (str/list/dict/Decimal/...).
    llvm::FunctionType* truthValueTy = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(truthValueTy, llvm::Function::ExternalLinkage, "PyObject_TruthValue", module.get());

    // A9: Runtime type guards for multi-versioning dispatch
    llvm::FunctionType* isIntTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(isIntTy, llvm::Function::ExternalLinkage, "pyc_is_int", module.get());

    llvm::FunctionType* isFloatTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(isFloatTy, llvm::Function::ExternalLinkage, "pyc_is_float", module.get());

    // String operations
    llvm::FunctionType* strFromAnyTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(strFromAnyTy, llvm::Function::ExternalLinkage, "PyStr_FromAny", module.get());

    llvm::FunctionType* strConcatTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(strConcatTy, llvm::Function::ExternalLinkage, "PyString_Concat", module.get());

    // Pyc_FormatValue(value, specStr) — Format Specification Mini-
    // Language, used by f-string format specs and str.format().
    llvm::Function::Create(strConcatTy, llvm::Function::ExternalLinkage, "Pyc_FormatValue", module.get());
    // PyBuiltin_StrFormat(templateStr, argsList, kwargsDict) — str.format().
    {
        llvm::FunctionType* strFormatTy = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(strFormatTy, llvm::Function::ExternalLinkage, "PyBuiltin_StrFormat", module.get());
    }

    llvm::FunctionType* strRepeatTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(strRepeatTy, llvm::Function::ExternalLinkage, "PyString_Repeat", module.get());

    llvm::FunctionType* builtinLenTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(builtinLenTy, llvm::Function::ExternalLinkage, "PyBuiltin_Len", module.get());

    // PyBuiltin_Open(path, mode) -> file dict (2 args).
    llvm::FunctionType* builtinOpenTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(builtinOpenTy, llvm::Function::ExternalLinkage, "PyBuiltin_Open", module.get());

    llvm::FunctionType* printNewlineTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
    llvm::Function::Create(printNewlineTy, llvm::Function::ExternalLinkage, "PyBuiltin_PrintNewline", module.get());

    llvm::FunctionType* assertFailTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(assertFailTy, llvm::Function::ExternalLinkage, "PyBuiltin_AssertFailure", module.get());

    llvm::FunctionType* pycPrintTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(pycPrintTy, llvm::Function::ExternalLinkage, "pyc_print", module.get());

    llvm::FunctionType* pycImportFailedTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(pycImportFailedTy, llvm::Function::ExternalLinkage, "pyc_import_failed", module.get());

    // B7: Runtime import support functions
    llvm::FunctionType* pycImportModuleTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(pycImportModuleTy, llvm::Function::ExternalLinkage, "pyc_import_module", module.get());
    llvm::FunctionType* pycImportFromTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(pycImportFromTy, llvm::Function::ExternalLinkage, "pyc_import_from_module", module.get());
    llvm::FunctionType* pycRunModuleTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(pycRunModuleTy, llvm::Function::ExternalLinkage, "pyc_run_module", module.get());
    
    // os.path stub functions
    llvm::FunctionType* osPathExistsTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(osPathExistsTy, llvm::Function::ExternalLinkage, "Pyc_OsPathExists", module.get());
    llvm::FunctionType* osPathIsFileTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(osPathIsFileTy, llvm::Function::ExternalLinkage, "Pyc_OsPathIsFile", module.get());
    llvm::FunctionType* osPathIsDirTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(osPathIsDirTy, llvm::Function::ExternalLinkage, "Pyc_OsPathIsDir", module.get());
    llvm::FunctionType* osUnlinkTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(osUnlinkTy, llvm::Function::ExternalLinkage, "Pyc_OsUnlink", module.get());
    
    // subprocess stub functions
    llvm::FunctionType* subCallTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(subCallTy, llvm::Function::ExternalLinkage, "Pyc_SubprocessCall", module.get());
    llvm::FunctionType* subCheckOutTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(subCheckOutTy, llvm::Function::ExternalLinkage, "Pyc_SubprocessCheckOutput", module.get());

    // Exception support: pyc_raise(exc), pyc_current_exception(), pyc_clear_exception(),
    // pyc_try_push(jmpBuf, filter), pyc_try_pop().
    llvm::FunctionType* pycRaiseTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(pycRaiseTy, llvm::Function::ExternalLinkage, "pyc_raise", module.get());
    llvm::FunctionType* pycCurExcTy = llvm::FunctionType::get(pyObjectPtrTy, {}, false);
    llvm::Function::Create(pycCurExcTy, llvm::Function::ExternalLinkage, "pyc_current_exception", module.get());
    llvm::FunctionType* pycClearExcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
    llvm::Function::Create(pycClearExcTy, llvm::Function::ExternalLinkage, "pyc_clear_exception", module.get());
    llvm::FunctionType* pycTryPushTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {int8PtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(pycTryPushTy, llvm::Function::ExternalLinkage, "pyc_try_push", module.get());
    llvm::FunctionType* pycTryPopTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
    llvm::Function::Create(pycTryPopTy, llvm::Function::ExternalLinkage, "pyc_try_pop", module.get());
    llvm::FunctionType* pycReraiseTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
    llvm::Function::Create(pycReraiseTy, llvm::Function::ExternalLinkage, "pyc_reraise", module.get());
    // I-009 traceback frames. Emitted even when -g is off.
    llvm::FunctionType* pycPushFrameTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {int8PtrTy, int8PtrTy}, false);
    llvm::Function::Create(pycPushFrameTy, llvm::Function::ExternalLinkage, "Pyc_PushFrame", module.get());
    llvm::FunctionType* pycPopFrameTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
    llvm::Function::Create(pycPopFrameTy, llvm::Function::ExternalLinkage, "Pyc_PopFrame", module.get());
    llvm::FunctionType* pycSetLinenoTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {llvm::Type::getInt32Ty(context)}, false);
    llvm::Function::Create(pycSetLinenoTy, llvm::Function::ExternalLinkage, "Pyc_SetLineno", module.get());
    llvm::FunctionType* pycMakeExcTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(pycMakeExcTy, llvm::Function::ExternalLinkage, "pyc_make_exc", module.get());
    llvm::FunctionType* pycExcMatchesTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(pycExcMatchesTy, llvm::Function::ExternalLinkage, "pyc_exc_matches", module.get());
    llvm::FunctionType* pycMakeFuncTy = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(pycMakeFuncTy, llvm::Function::ExternalLinkage, "pyc_make_func", module.get());
    // Complex numbers (type 13): PyComplex_New(real: double, imag: double) -> ptr
    llvm::FunctionType* pyComplexNewTy = llvm::FunctionType::get(pyObjectPtrTy, {llvm::Type::getDoubleTy(context), llvm::Type::getDoubleTy(context)}, false);
    llvm::Function::Create(pyComplexNewTy, llvm::Function::ExternalLinkage, "PyComplex_New", module.get());
    // Complex arithmetic: PyComplex_Add/Sub/Mul/Div(a: ptr, b: ptr) -> ptr
    llvm::FunctionType* pyComplexArithTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    for (const char* name : {"PyComplex_Add", "PyComplex_Sub", "PyComplex_Mul", "PyComplex_Div"}) {
        llvm::Function::Create(pyComplexArithTy, llvm::Function::ExternalLinkage, name, module.get());
    }
    // Complex pow: PyComplex_Pow(base: ptr, exp: ptr) -> ptr
    llvm::FunctionType* pyComplexPowTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(pyComplexPowTy, llvm::Function::ExternalLinkage, "PyComplex_Pow", module.get());
    // Complex abs: PyComplex_Abs(z: ptr) -> ptr (returns float)
    llvm::FunctionType* pyComplexAbsTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(pyComplexAbsTy, llvm::Function::ExternalLinkage, "PyComplex_Abs", module.get());
    // cmath module functions: sqrt, log, exp, sin, cos, tan (all take one complex ptr, return complex ptr)
    for (const char* name : {"PyCmath_Sqrt", "PyCmath_Log", "PyCmath_Exp", "PyCmath_Sin", "PyCmath_Cos", "PyCmath_Tan"}) {
        llvm::FunctionType* cmathTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(cmathTy, llvm::Function::ExternalLinkage, name, module.get());
    }
    // datetime module: AST-recognized construction and method calls
    // (Compiler.cpp lowerMethodCall), dispatched as direct calls like
    // cmath above rather than through the generic token+registry path.
    llvm::FunctionType* dateNewTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(dateNewTy, llvm::Function::ExternalLinkage, "PyDateTime_Date", module.get());
    llvm::FunctionType* datetimeNewTy = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(datetimeNewTy, llvm::Function::ExternalLinkage, "PyDateTime_Datetime", module.get());
    llvm::FunctionType* timedeltaNewTy = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(timedeltaNewTy, llvm::Function::ExternalLinkage, "PyTimedelta_New", module.get());
    for (const char* name : {"PyDateTime_Isoformat", "PyDateTime_Weekday", "PyDateTime_Isoweekday", "PyTimedelta_TotalSeconds"}) {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, name, module.get());
    }
    // pathlib.Path: same AST-recognized direct-call convention as datetime
    // above. PyPathlib_Path/Exists/IsFile/IsDir/Mkdir take one ptr arg;
    // PyPathlib_Joinpath takes the receiver plus a boxed list of parts.
    for (const char* name : {"PyPathlib_Path", "PyPathlib_Exists", "PyPathlib_IsFile",
                              "PyPathlib_IsDir", "PyPathlib_Mkdir"}) {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, name, module.get());
    }
    llvm::FunctionType* joinpathTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(joinpathTy, llvm::Function::ExternalLinkage, "PyPathlib_Joinpath", module.get());
    // hashlib: same direct-call convention. PyHashlib_Md5/Sha1/Sha256 take
    // the data string; PyHashlib_Hexdigest/Digest take the hash-object receiver.
    for (const char* name : {"PyHashlib_Md5", "PyHashlib_Sha1", "PyHashlib_Sha256",
                              "PyHashlib_Hexdigest", "PyHashlib_Digest"}) {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, name, module.get());
    }
    // copy.copy/deepcopy: same direct-call convention, one ptr arg (the
    // value to copy) -> ptr.
    for (const char* name : {"PyCopy_Copy", "PyCopy_Deepcopy"}) {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, name, module.get());
    }
    // file.readlines(): same one-ptr-arg direct-call convention.
    {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_FileReadlines", module.get());
    }
    // csv.writer(f) / .writerow(row): direct-call convention.
    {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, "PyCsv_Writer", module.get());
        llvm::FunctionType* twoArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyCsv_Writerow", module.get());
    }
    // itertools.chain.from_iterable(...): direct-call convention.
    {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, "PyItertools_ChainFromIterable", module.get());
    }
    // itertools.groupby(iterable, key=...): direct-call convention (2
    // raw args), not token+registry — see PyItertools_Groupby's comment.
    {
        llvm::FunctionType* twoArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyItertools_Groupby", module.get());
    }
    // collections.deque(...): direct-call convention (needs a compile-time
    // "deque" noteType tag on construction that generic dict-dispatch
    // can't attach, mirroring pathlib.Path). PyCollections_Deque/
    // PyDeque_Popleft take one ptr arg; PyDeque_Appendleft/Rotate take two.
    {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, "PyCollections_Deque", module.get());
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, "PyDeque_Popleft", module.get());
        llvm::FunctionType* twoArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyDeque_Appendleft", module.get());
        llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyDeque_Rotate", module.get());
    }
    // decimal.Decimal(...): direct-call convention (needs a compile-time
    // "decimal" noteType tag, mirroring collections.deque above).
    // PyDecimal_Construct takes one ptr arg; PyDecimal_Quantize takes two.
    {
        llvm::FunctionType* oneArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(oneArgTy, llvm::Function::ExternalLinkage, "PyDecimal_Construct", module.get());
        llvm::FunctionType* twoArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyDecimal_Quantize", module.get());
    }
    // bytes/bytearray: PyBytes_FromStringAndSize/PyByteArray_FromStringAndSize
    // take (ptr, i64 length) — the length-aware construction "bytesconst"'s
    // Codegen handler needs (see that handler's comment for why "const"'s
    // NUL-terminated PyUnicode_FromString path can't be reused for bytes).
    {
        llvm::FunctionType* bytesFromSizeTy = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, llvm::Type::getInt64Ty(context)}, false);
        llvm::Function::Create(bytesFromSizeTy, llvm::Function::ExternalLinkage, "PyBytes_FromStringAndSize", module.get());
        llvm::Function::Create(bytesFromSizeTy, llvm::Function::ExternalLinkage, "PyByteArray_FromStringAndSize", module.get());
    }
    // bytes()/bytearray() construction (2-arg: value, encoding — either
    // may be null) and bytes/str methods: .hex()/.fromhex()/.decode()/
    // .encode() (1 or 2-arg direct-call convention, matching hashobj's
    // .hexdigest() pattern). twoArg isn't declared yet at this point in
    // the function, so declare the FunctionType inline instead.
    {
        llvm::FunctionType* twoArgTy2 = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        for (const char* n : {"PyBuiltin_Bytes", "PyBuiltin_Bytearray", "PyBytes_Decode", "PyStr_Encode",
                              "PyByteArray_Append", "PyByteArray_ExtendOp"}) {
            llvm::Function::Create(twoArgTy2, llvm::Function::ExternalLinkage, n, module.get());
        }
        llvm::FunctionType* oneArgTy2 = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        for (const char* n : {"PyBytes_Hex", "PyBytes_Fromhex"}) {
            llvm::Function::Create(oneArgTy2, llvm::Function::ExternalLinkage, n, module.get());
        }
    }
    // setjmp is special: declaration with the ReturnsTwice attribute.
    {
        llvm::FunctionType* setjmpTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {int8PtrTy}, false);
        llvm::Function* sj = llvm::Function::Create(setjmpTy, llvm::Function::ExternalLinkage, "setjmp", module.get());
        sj->addFnAttr(llvm::Attribute::ReturnsTwice);
    }

    // Builtins: list, reversed, enumerate, zip
    for (const char* name : {"PyBuiltin_List","PyBuiltin_Reversed",
                              "PyBuiltin_Enumerate"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    // enumerate(iterable, start) — 2-arg variant
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyBuiltin_Enumerate2", module.get());
    }
    llvm::FunctionType* zip2Ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(zip2Ty, llvm::Function::ExternalLinkage, "PyBuiltin_Zip2", module.get());
    llvm::FunctionType* zipNTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(zipNTy, llvm::Function::ExternalLinkage, "PyBuiltin_ZipN", module.get());
    // min/max: list form takes (iterable, key); 2-value form takes
    // (a, b, key) — key= was found completely unsupported (silently
    // ignored, min([...], key=f) printed the key function itself,
    // because min/max have no funcParamNames entry so the generic
    // kwarg-append fallback stuffed key's value onto the end of argRes,
    // where it was then misread as a second positional value) while
    // hunting for more bugs.
    {
        llvm::FunctionType* minMaxListTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        for (const char* name : {"PyBuiltin_MinList","PyBuiltin_MaxList"}) {
            llvm::Function::Create(minMaxListTy, llvm::Function::ExternalLinkage, name, module.get());
        }
        llvm::FunctionType* minMax2Ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        for (const char* name : {"PyBuiltin_Min2","PyBuiltin_Max2"}) {
            llvm::Function::Create(minMax2Ty, llvm::Function::ExternalLinkage, name, module.get());
        }
    }

    // Builtins: int, float, abs; string methods; dict/list methods
    // 0-arg builtins: super
    llvm::FunctionType* zeroArgTy = llvm::FunctionType::get(pyObjectPtrTy, {}, false);
    llvm::Function::Create(zeroArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Super", module.get());
    llvm::Function::Create(zeroArgTy, llvm::Function::ExternalLinkage, "Pyc_MissingDefault", module.get());

    for (const char* name : {"PyBuiltin_Int","PyBuiltin_Float","PyBuiltin_Abs",
                              "PyBuiltin_Ord","PyBuiltin_Chr",
                              "PyBuiltin_Bool","PyBuiltin_Type","PyBuiltin_Callable",
                              "PyBuiltin_Hex","PyBuiltin_Oct","PyBuiltin_Bin",
                              "PyBuiltin_Id","PyBuiltin_Repr",
                              "PyString_Upper","PyString_Lower","PyString_Strip",
                              "PyString_SplitWhitespace","PyDict_Keys","PyDict_Values",
                              "PyDict_Items","PyList_Pop"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    // list.sort(key, reverse) — 3-arg (receiver, key, reverse), both key
    // and reverse newly supported (previously silently ignored).
    {
        llvm::FunctionType* sort3Ty = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(sort3Ty, llvm::Function::ExternalLinkage, "PyList_Sort", module.get());
    }
    // 2-arg builtins: divmod, round, pow, complex
    llvm::FunctionType* twoArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Divmod", module.get());
    llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Round", module.get());
    llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Pow", module.get());
    llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Complex", module.get());
    // 3-arg pow(base, exp, mod)
    {
        llvm::FunctionType* threeArgTy = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(threeArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Pow3", module.get());
    }

    for (const char* name : {"PyString_Split","PyString_Join","PyBuiltin_IntBase",
                              "PyString_RFind","PyString_Partition","PyString_RPartition",
                              "PyString_RSplitWhitespace","PyString_SplitWhitespace2"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    for (const char* name : {"PyString_Find3","PyString_RFind3","PyString_RSplit","PyString_Split2",
                              "PyString_Count3","PyString_Index3","PyString_RIndex3",
                              "PyString_StartsWith3","PyString_EndsWith3","PyList_Index3"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_RFind4", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_Find4", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_Count4", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_Index4", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_RIndex4", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_StartsWith4", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_EndsWith4", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyList_Index4", module.get());
    }
    // Additional list / dict / string methods added for completeness.
    // 2-arg: insert(list, idx, item), extend(list, other), center/ljust/rjust(s, w, fill),
    //         pop(key, defval), setdefault(key, defval), fromkeys(keys, defval), zfill(s, w).
    // 3-arg: replace(s, old, new, count) — handled by lowerMethodCall directly.
    auto twoArg = [&](const char* n) {
        llvm::FunctionType* t = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(t, llvm::Function::ExternalLinkage, n, module.get());
    };
    auto threeArg = [&](const char* n) {
        llvm::FunctionType* t = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(t, llvm::Function::ExternalLinkage, n, module.get());
    };
    for (const char* n : {"PyList_Extend","PyList_Remove","PyList_Index","PyList_Count",
                          "PyList_PopAt",
                          "PyDict_Update","PyDict_FromKeys",
                          "PyString_ZFill",
                          "PyString_StartsWith","PyString_EndsWith",
                          "PyNumber_Lshift","PyNumber_Rshift",
                          "PyNumber_BitOr","PyNumber_BitAnd","PyNumber_BitXor"}) twoArg(n);
    for (const char* n : {"PyString_Center","PyString_LJust","PyString_RJust",
                          "PyDict_Pop","PyDict_SetDefault",
                          "PyList_Insert"}) threeArg(n);
    // 1-arg helpers (Is* predicates, casefold/title, lstrip/rstrip, count, copy,
    // clear, popitem, reverse, remove, index, update, fromkeys, remove, etc.).
    for (const char* n : {"PyString_IsAlpha","PyString_IsDigit","PyString_IsAlnum",
                          "PyString_IsLower","PyString_IsUpper","PyString_IsSpace",
                          "PyString_Casefold","PyString_Title",
                          "PyString_Capitalize","PyString_Swapcase","PyString_Splitlines",
                          "PyString_LStrip","PyString_RStrip",
                          "PyList_Reverse","PyList_Copy","PyList_Clear",
                          "PyDict_Copy","PyDict_Clear","PyDict_PopItem",
                          "PyInt_BitLength","PyFloat_IsInteger"}) {
        llvm::FunctionType* t = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(t, llvm::Function::ExternalLinkage, n, module.get());
    }
    // 3-arg helpers (center/ljust/rjust with width+fillchar, etc.)
    for (const char* n : {"PyString_Center","PyString_LJust","PyString_RJust",
                          "PyDict_Pop","PyDict_SetDefault"}) threeArg(n);
    // PyString_ReplaceN(s, old, new, count)
    {
        llvm::FunctionType* t = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(t, llvm::Function::ExternalLinkage, "PyString_ReplaceN", module.get());
    }

    // re module: PyBuiltin_ReMatchGroup stays 2-arg (m, idx). The rest all
    // grew a trailing `flags` param once re.IGNORECASE/MULTILINE/DOTALL
    // support was added: ReFinditer/ReFindall/ReSearch (pattern, subject,
    // flags — 3-arg), ReCompile (pattern, flags — 2-arg, now genuinely
    // using the 2nd slot), ReSub (pattern, repl, subject, count, flags —
    // 5-arg), ReSplit (pattern, subject, maxsplit, flags — 4-arg, maxsplit
    // now actually implemented too).
    twoArg("PyBuiltin_ReMatchGroup");
    twoArg("PyBuiltin_ReCompile");
    for (const char* n : {"PyBuiltin_ReFinditer","PyBuiltin_ReFindall","PyBuiltin_ReSearch","PyBuiltin_ReMatch"}) threeArg(n);
    {
        llvm::FunctionType* t5 = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(t5, llvm::Function::ExternalLinkage, "PyBuiltin_ReSub", module.get());
        llvm::FunctionType* t4 = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(t4, llvm::Function::ExternalLinkage, "PyBuiltin_ReSplit", module.get());
    }

    // Builtins: sum, sorted, any, all; isinstance (2-arg)
    for (const char* name : {"PyBuiltin_Sum","PyBuiltin_Any","PyBuiltin_All"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    // Builtins: map, filter (2-arg: func, iterable)
    for (const char* name : {"PyBuiltin_Map","PyBuiltin_Filter"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    // PyBuiltin_MapN (func, list-of-iterables)
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyBuiltin_MapN", module.get());
    }
    // PyList_UnpackStar (returns PyObject* list of [before..., starList, after...])
    {
        std::vector<llvm::Type*> params = {pyObjectPtrTy, llvm::Type::getInt64Ty(context),
                                            llvm::Type::getInt64Ty(context)};
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, params, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyList_UnpackStar", module.get());
    }
    // sum(iterable, start) — 2-arg variant
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyBuiltin_Sum2", module.get());
    }
    // cmp_to_key(cmp) takes one arg and returns a dict token
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyBuiltin_CmpToKey", module.get());
    }
    // sorted / sorted_with_cmp both take (lst, key-or-cmp, reverse).
    llvm::FunctionType* sortedTy = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(sortedTy, llvm::Function::ExternalLinkage, "PyBuiltin_SortedWithCmp", module.get());
    {
        llvm::FunctionType* sorted3Ty = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(sorted3Ty, llvm::Function::ExternalLinkage, "PyBuiltin_Sorted", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_IsInstance", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_IsSubclass", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_HasAttr", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_Iter", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_Next", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_Apply", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_ApplyKw", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, llvm::Type::getInt32Ty(context), int8PtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_ApplyKwRest", module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
            {pyObjectPtrTy, int8PtrTy, int8PtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "Pyc_ApplyRejectDupKw", module.get());
    }
    // PyCollections_Counter / PyCollections_MostCommon (1-arg: args list)
    // PyCollections_Elements (1-arg: counter dict)
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyCollections_Counter", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyCollections_MostCommon", module.get());
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyCollections_Elements", module.get());
    }
    // PyCollections_Subtract (1-arg: args list). Counter.update() has no
    // entry here: it lowers to PyDict_Update like plain dict.update().
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyCollections_Subtract", module.get());
    }

    // String methods: find, count, replace (find/count return int boxed; replace returns str)
    for (const char* name : {"PyString_Find","PyString_Count","PyString_Index","PyString_RIndex"}) {
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

    llvm::FunctionType* dictGetWithDefaultTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(dictGetWithDefaultTy, llvm::Function::ExternalLinkage, "PyDict_GetItemWithDefault", module.get());
    // Pyc_DictGetOrDefault(dict, key, fallback) — used for f(**some_dict)
    // call sites; see its comment in Runtime.cpp.
    llvm::Function::Create(dictGetWithDefaultTy, llvm::Function::ExternalLinkage, "Pyc_DictGetOrDefault", module.get());
    // Pyc_RouteSpreadKwargs(spread_dict, param_names_list, kwargs_dict) —
    // routes unmatched **dict spread keys into a **kwargs catch-all.
    llvm::FunctionType* routeSpreadKwargsTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(routeSpreadKwargsTy, llvm::Function::ExternalLinkage, "Pyc_RouteSpreadKwargs", module.get());
    llvm::FunctionType* raiseMissingTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {int8PtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(raiseMissingTy, llvm::Function::ExternalLinkage, "Pyc_RaiseMissingArgs", module.get());
    llvm::FunctionType* checkMissingTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(checkMissingTy, llvm::Function::ExternalLinkage, "Pyc_CheckMissingArgs", module.get());

    llvm::FunctionType* dictDelItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(dictDelItemTy, llvm::Function::ExternalLinkage, "PyDict_DelItem", module.get());
    // Pyc_DelItem(obj, key): generic del obj[key], dispatching on obj's
    // runtime type (dict key deletion or list item removal by index) —
    // see its comment in Runtime.cpp for why this replaced calling
    // PyDict_DelItem directly for every `del obj[idx]`.
    llvm::Function::Create(dictDelItemTy, llvm::Function::ExternalLinkage, "Pyc_DelItem", module.get());

    // Set operations (type 20). PySet_New() and the void/boxed-arg helpers
    // cover literal construction, comprehensions, methods, and operators.
    llvm::FunctionType* setNewTy = llvm::FunctionType::get(pyObjectPtrTy, {}, false);
    llvm::Function::Create(setNewTy, llvm::Function::ExternalLinkage, "PySet_New", module.get());
    llvm::FunctionType* setAddTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(setAddTy, llvm::Function::ExternalLinkage, "PySet_Add", module.get());
    llvm::Function::Create(setAddTy, llvm::Function::ExternalLinkage, "PySet_Discard", module.get());
    llvm::Function::Create(setAddTy, llvm::Function::ExternalLinkage, "PySet_Remove", module.get());
    llvm::Function::Create(setAddTy, llvm::Function::ExternalLinkage, "PySet_Update", module.get());
    llvm::FunctionType* setUnaryVoidTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy}, false);
    llvm::Function::Create(setUnaryVoidTy, llvm::Function::ExternalLinkage, "PySet_Clear", module.get());
    llvm::FunctionType* setContainsObjTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(setContainsObjTy, llvm::Function::ExternalLinkage, "PySet_ContainsObj", module.get());
    llvm::FunctionType* setUnaryTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(setUnaryTy, llvm::Function::ExternalLinkage, "PySet_Pop", module.get());
    llvm::Function::Create(setUnaryTy, llvm::Function::ExternalLinkage, "PySet_Copy", module.get());
    llvm::Function::Create(setUnaryTy, llvm::Function::ExternalLinkage, "PySet_ToList", module.get());
    llvm::FunctionType* setBinaryTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(setBinaryTy, llvm::Function::ExternalLinkage, "PySet_Union", module.get());
    llvm::Function::Create(setBinaryTy, llvm::Function::ExternalLinkage, "PySet_Intersection", module.get());
    llvm::Function::Create(setBinaryTy, llvm::Function::ExternalLinkage, "PySet_Difference", module.get());
    llvm::Function::Create(setBinaryTy, llvm::Function::ExternalLinkage, "PySet_SymmetricDifference", module.get());
    llvm::Function::Create(setBinaryTy, llvm::Function::ExternalLinkage, "PySet_IsSubsetObj", module.get());
    llvm::Function::Create(setBinaryTy, llvm::Function::ExternalLinkage, "PySet_IsSupersetObj", module.get());

    // Subscript / membership / power
    llvm::FunctionType* getItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(getItemTy, llvm::Function::ExternalLinkage, "Pyc_GetItem", module.get());
    llvm::FunctionType* subscriptTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(subscriptTy, llvm::Function::ExternalLinkage, "Pyc_Subscript", module.get());
    // Pyc_GetAttr(obj, attrName): wraps Pyc_GetItem for a bare (non-call)
    // attribute read, auto-invoking a @property getter — see its
    // comment in Runtime.cpp.
    llvm::Function::Create(getItemTy, llvm::Function::ExternalLinkage, "Pyc_GetAttr", module.get());
    // Pyc_GetAttrDefault(obj, attrName, default): getattr with a default
    // (I-139). Same boxed-ptr convention as Pyc_GetAttr, plus the default.
    {
        llvm::FunctionType* getAttrDefaultTy = llvm::FunctionType::get(
            pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(getAttrDefaultTy, llvm::Function::ExternalLinkage,
                               "Pyc_GetAttrDefault", module.get());
    }
    // Pyc_CallMethod(methodVal, receiver, argsList): the single dispatch
    // point for obj.method(...)/ClassName.method(...) calls, deciding
    // self/cls-prepending based on @staticmethod/@classmethod tagging —
    // see its comment in Runtime.cpp.
    llvm::FunctionType* callMethodTy = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(callMethodTy, llvm::Function::ExternalLinkage, "Pyc_CallMethod", module.get());

    // Pyc_CallBuiltinMethod(receiver, name, args) — runtime-tag method
    // dispatch for builtin receivers, and Pyc_CallMethodOrBuiltin(method,
    // receiver, args, name), the 4-arg fallback lowerMethodCall emits
    // for arity >= 3: class instances go to Pyc_CallMethod, builtins to
    // the tag dispatch. Pyc_CallMethodOrBuiltinN (arity 0/1/2) pass
    // a0/a1 as PyObject* so the builtin path skips the args-list alloc.
    llvm::Function::Create(callMethodTy, llvm::Function::ExternalLinkage,
                           "Pyc_CallBuiltinMethod", module.get());
    llvm::FunctionType* callMethodOrBuiltinTy = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(callMethodOrBuiltinTy, llvm::Function::ExternalLinkage,
                           "Pyc_CallMethodOrBuiltin", module.get());
    llvm::Function::Create(callMethodTy, llvm::Function::ExternalLinkage,
                           "Pyc_CallMethodOrBuiltin0", module.get());
    llvm::Function::Create(callMethodOrBuiltinTy, llvm::Function::ExternalLinkage,
                           "Pyc_CallMethodOrBuiltin1", module.get());
    llvm::FunctionType* callMethodOrBuiltin2Ty = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(callMethodOrBuiltin2Ty, llvm::Function::ExternalLinkage,
                           "Pyc_CallMethodOrBuiltin2", module.get());
    llvm::FunctionType* callMethodOrBuiltinKwTy = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(callMethodOrBuiltinKwTy, llvm::Function::ExternalLinkage,
                           "Pyc_CallMethodOrBuiltinKw", module.get());
    llvm::FunctionType* callBuiltinMethod0Ty = llvm::FunctionType::get(pyObjectPtrTy,
        {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(callBuiltinMethod0Ty, llvm::Function::ExternalLinkage,
                           "Pyc_CallBuiltinMethod0", module.get());
    llvm::Function::Create(callMethodTy, llvm::Function::ExternalLinkage,
                           "Pyc_CallBuiltinMethod1", module.get());
    llvm::Function::Create(callMethodOrBuiltinTy, llvm::Function::ExternalLinkage,
                           "Pyc_CallBuiltinMethod2", module.get());

    // B6: Extended attribute lookup (instance dict + class dict fallback)
    llvm::FunctionType* getAttrExtTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(getAttrExtTy, llvm::Function::ExternalLinkage, "PyObject_GetAttrExtended", module.get());

    llvm::FunctionType* setItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(setItemTy, llvm::Function::ExternalLinkage, "Pyc_SetItem", module.get());
    // Pyc_SubscriptSetItem(obj, key, val) — genuine obj[key] = val
    // assignment only, dispatching __setitem__ for a class instance;
    // see its comment in Runtime.cpp for why this must be a separate
    // function from Pyc_SetItem (also used for plain attribute
    // assignment, which must never trigger __setitem__).
    llvm::Function::Create(setItemTy, llvm::Function::ExternalLinkage, "Pyc_SubscriptSetItem", module.get());

    llvm::FunctionType* containsTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(containsTy, llvm::Function::ExternalLinkage, "Pyc_Contains", module.get());

    llvm::FunctionType* getSliceTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(getSliceTy, llvm::Function::ExternalLinkage, "Pyc_GetSlice", module.get());

    llvm::FunctionType* setSliceTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(setSliceTy, llvm::Function::ExternalLinkage, "Pyc_SetSlice", module.get());

    llvm::FunctionType* delSliceTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(delSliceTy, llvm::Function::ExternalLinkage, "Pyc_DelSlice", module.get());

    llvm::FunctionType* powerTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(powerTy, llvm::Function::ExternalLinkage, "Pyc_Pow", module.get());

    llvm::FunctionType* powI64ObjTy = llvm::FunctionType::get(pyObjectPtrTy, {llvm::Type::getInt64Ty(context), llvm::Type::getInt64Ty(context)}, false);
    llvm::Function::Create(powI64ObjTy, llvm::Function::ExternalLinkage, "Pyc_PowInt64Obj", module.get());

    llvm::FunctionType* regClassTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(regClassTy, llvm::Function::ExternalLinkage, "pyc_register_class", module.get());

    // Declare libm pow(double, double) for native float power codegen.
    llvm::FunctionType* libmPowTy = llvm::FunctionType::get(
        llvm::Type::getDoubleTy(context),
        {llvm::Type::getDoubleTy(context), llvm::Type::getDoubleTy(context)},
        false);
    llvm::Function::Create(libmPowTy, llvm::Function::ExternalLinkage, "pow", module.get());

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
        // Don't rename __module__ here - it will be renamed in the compile method
        if (name == "main") return std::string("pyc_py_main");
        return name;
    };

    for (const auto& f : ir.functions) {
        if (f.name.empty()) continue;  // Skip functions without names
        // The C runtime's `main` is provided by src/runtime/MainWrapper.cpp.
        // The module entry and a Python `def main` are distinct symbols.
        std::string irName = llvmFunctionName(f.name);
        
        // A6: Detect specialized variants (name starts with "__specialized_").
        // Format: __specialized_<funcName>_<sig> where sig = "i"/"f" per param.
        // Params: [cell params...] + [original param names].
        // The sig length = number of user params (f.args.size() - f.freeCellVars.size()).
        // Native types: i=int(i64), f=float(double). Non-numeric types are not
        // specialized (params would be PyObject* — same as generic — no benefit).
        bool isSpecialized = (f.name.find("__specialized_") == 0);
        std::vector<llvm::Type*> argTypes;
        if (isSpecialized) {
            size_t ncells = f.freeCellVars.size();
            size_t nuserParams = (f.args.size() >= ncells) ? (f.args.size() - ncells) : 0;
            // Parse sig from variant name: everything after "__specialized_<funcName>_"
            // The sig is the last nuserParams chars of the name.
            std::string sig;
            if (nuserParams > 0 && f.name.size() > nuserParams) {
                sig = f.name.substr(f.name.size() - nuserParams);
            }
            for (size_t i = 0; i < f.args.size(); ++i) {
                if (i < ncells) {
                    // Cell params are always PyObject*
                    argTypes.push_back(pyObjectPtrTy);
                } else {
                    // User params: native type (i64/double) for numeric
                    size_t sigIdx = i - ncells;
                    if (sigIdx < sig.size()) {
                        char t = sig[sigIdx];
                        if (t == 'f') {
                            argTypes.push_back(llvm::Type::getDoubleTy(context));
                        } else if (t == 'i') {
                            argTypes.push_back(llvm::Type::getInt64Ty(context));
                        }
                    } else {
                        argTypes.push_back(pyObjectPtrTy);
                    }
                }
            }
        } else {
            argTypes.assign(f.args.size(), pyObjectPtrTy);
        }
        // A6 native return: specialized variants with a proven numeric return type
        // return i64/double directly instead of a boxed PyObject*.
        llvm::Type* retTy = pyObjectPtrTy;
        if (isSpecialized && f.nativeReturnType == "int") {
            retTy = llvm::Type::getInt64Ty(context);
        } else if (isSpecialized && f.nativeReturnType == "float") {
            retTy = llvm::Type::getDoubleTy(context);
        }
        llvm::FunctionType* funcType = llvm::FunctionType::get(retTy, argTypes, false);
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
        // A6: Skip adapter generation for specialized variants.
        // Specialized variants are only called directly from the original function
        // (which boxes arguments), so they don't need indirect dispatch adapters.
        if (f.name.find("__specialized_") == 0) continue;
        std::string adapterName = "__apply__" + f.name;
        llvm::FunctionType* aty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function* adapter = llvm::Function::Create(aty, llvm::Function::ExternalLinkage, adapterName, module.get());

        llvm::BasicBlock* aentry = llvm::BasicBlock::Create(context, "entry", adapter);
        llvm::IRBuilder<> abuilder(aentry);
        llvm::Value* argListVal = adapter->getArg(0);
        argListVal->setName("args");
        llvm::Value* kwArgVal = adapter->getArg(1);
        kwArgVal->setName("kwargs");

        std::string realName = llvmFunctionName(f.name);
        llvm::Function* real = module->getFunction(realName);
        if (!real) real = module->getFunction(f.name);
        if (!real) {
            abuilder.CreateRet(llvm::ConstantPointerNull::get(pyObjectPtrTy));
            continue;
        }

        // Use *original user* param names (with * markers) for user signature.
        // paramNames is always the user-level signature (we never mutate it with cells).
        // It may legitimately be empty (e.g. a nested def with no user parameters).
        const auto& pnames = f.paramNames;  // authoritative user params (may be empty)
        // vidx = index of a *args (single-star) param; kwidx = index of a
        // **kwargs (double-star) param. Found and fixed while bug hunting:
        // this loop used to stop at the *first* star-prefixed name and
        // treat it as "the" vararg slot — for a signature with both *args
        // and **kwargs, the **kwargs marker (which also starts with '*')
        // was silently never detected as a separate slot at all. The real
        // function (declared via ir.addFunction with both bare names as
        // separate parameters) then got called with one argument short,
        // crashing LLVM module verification ("Incorrect number of
        // arguments") on every indirect call to a function combining
        // *args and **kwargs — including the standard generic-decorator
        // wrapper pattern, def wrapper(*args, **kwargs): ...
        size_t vidx = (size_t)-1;
        size_t kwidx = (size_t)-1;
        for (size_t j = 0; j < pnames.size(); ++j) {
            if (pnames[j].size() >= 2 && pnames[j][0] == '*' && pnames[j][1] == '*') {
                kwidx = j;
            } else if (!pnames[j].empty() && pnames[j][0] == '*') {
                if (vidx == (size_t)-1) vidx = j;
            }
        }
        bool hasVar = (vidx != (size_t)-1);
        bool hasKwVar = (kwidx != (size_t)-1);
        // The star-param boundary for computing fixed-param counts below:
        // *args if present (it always precedes **kwargs in a valid
        // signature), else **kwargs if that's the only star param, else
        // there's no star param at all.
        size_t starIdx = hasVar ? vidx : (hasKwVar ? kwidx : pnames.size());
        // Number of *user* fixed params in the original signature (cells are not user params).
        size_t userFixedCount = starIdx;
        // For the *vararg start in the Pyc list we still need the pre-vararg user count.
        size_t fixed = userFixedCount;  // in Pyc list terms (after cells), this is the user fixed count before *

        // B5: For indirect calls via Pyc_Apply + adapter:
        // - When the caller used a descriptor bundle (closure value), the incoming
        //   flat list already has cells prepended: [cell0, cell1, ..., userarg0, ...]
        // - For plain token calls (non-closure), there are no leading cells.
        // The adapter must pass the cells as leading LLVM args to the real target,
        // then unpack the remaining list elements as the user's arguments.
        size_t ncells = f.freeCellVars.size();

        // Compute original user (non-cell) param count from the final args list
        // (we only ever prepend cell slots to args; paramNames holds original user view).
        size_t origUserParams = (f.args.size() >= ncells) ? (f.args.size() - ncells) : 0;
        // Number of leading *user* fixed params in the post-cell Pyc arg list.
        size_t userFixed = hasVar ? vidx : (hasKwVar ? kwidx : origUserParams);

        // Adapter must supply defaults for trailing defaulted params when the
        // incoming Pyc arg list (after cells) has fewer user args than the
        // function's userFixed params. We discover defaults either from the
        // IRFunction annotation or by probing module globals by the
        // conventional __default_<name>_<k> names (defensive for top-level
        // defaulted functions).
        std::vector<std::string> defSlots = f.defaultGlobals;
        size_t ndef = defSlots.size();
        if (ndef == 0) {
            for (int k = 0; ; ++k) {
                std::string s = "__default_" + f.name + "_" + std::to_string(k);
                if (module->getNamedGlobal("pyc_global_" + s)) {
                    defSlots.push_back(s);
                } else {
                    break;
                }
            }
            ndef = defSlots.size();
        }

        std::vector<llvm::Value*> cargs;
        llvm::Function* listGet = module->getFunction("PyList_GetItem");
        llvm::Function* listSize = module->getFunction("PyList_Size");
        llvm::Function* listNew  = module->getFunction("PyList_New");
        llvm::Function* listAppend = module->getFunction("PyList_Append");

        // Extract leading cells (if any) from the Pyc list; these become the
        // hidden leading arguments for the real function.
        for (size_t k = 0; k < ncells; ++k) {
            llvm::Value* idx = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), k);
            llvm::Value* el = nullptr;
            if (listGet) {
                el = abuilder.CreateCall(listGet, {argListVal, idx}, "c" + std::to_string(k));
            } else {
                el = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            }
            cargs.push_back(el);
        }

        // User-fixed prefix starts after the hidden cells in the list.
        // B5/B4 defaults: if fewer user args supplied than fixed params, load
        // from the function's __default_<name>_<k> globals for the trailing positions.
        llvm::Value* ln = nullptr;
        if (listSize) {
            ln = abuilder.CreateCall(listSize, {argListVal}, "ln");
        } else {
            ln = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
        }
        llvm::Value* ncellsV = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), ncells);
        llvm::Value* userLen = abuilder.CreateSub(ln, ncellsV);
        llvm::Value* zero64 = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
        llvm::Value* isNeg = abuilder.CreateICmpSLT(userLen, zero64);
        userLen = abuilder.CreateSelect(isNeg, zero64, userLen, "ulen");
        // Recompute ndef/firstDef from the (possibly probed) defSlots for this adapter.
        ndef = defSlots.size();
        size_t firstDef = (userFixed > ndef) ? (userFixed - ndef) : 0;
        // Missing required args: not supplied positionally AND not in kwDict.
        // A kwargs-only call (hs[0](a=1)) has userLen==0; do not raise until
        // we have checked the separate kw channel. Empty miss list is a no-op.
        std::string applyFnName = !f.displayName.empty() ? f.displayName : f.name;
        llvm::Function* fromStr = module->getFunction("PyUnicode_FromString");
        llvm::Function* dictGetFn = module->getFunction("PyDict_GetItem");
        llvm::Function* rejectDupFn = module->getFunction("Pyc_ApplyRejectDupKw");
        if (firstDef > 0) {
            llvm::Value* firstDefV = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), firstDef);
            llvm::Value* needRaise = abuilder.CreateICmpSLT(userLen, firstDefV, "need.miss");
            llvm::BasicBlock* raiseB = llvm::BasicBlock::Create(context, "missargs", adapter);
            llvm::BasicBlock* okB = llvm::BasicBlock::Create(context, "args.ok", adapter);
            abuilder.CreateCondBr(needRaise, raiseB, okB);
            abuilder.SetInsertPoint(raiseB);
            llvm::Function* raiseFn = module->getFunction("Pyc_RaiseMissingArgs");
            llvm::Value* zeroSz = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
            llvm::Value* mlist = listNew
                ? (llvm::Value*)abuilder.CreateCall(listNew, {zeroSz}, "miss.names")
                : (llvm::Value*)llvm::ConstantPointerNull::get(pyObjectPtrTy);
            llvm::Value* kwIsNull = abuilder.CreateICmpEQ(
                kwArgVal, llvm::ConstantPointerNull::get(pyObjectPtrTy), "miss.kwnull");
            for (size_t i = 0; i < firstDef; ++i) {
                llvm::Value* iV = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), i);
                llvm::Value* isMissPos = abuilder.CreateICmpSLE(userLen, iV, "miss.i" + std::to_string(i));
                llvm::BasicBlock* chkKwB = llvm::BasicBlock::Create(context, "miss.chkw" + std::to_string(i), adapter);
                llvm::BasicBlock* appB = llvm::BasicBlock::Create(context, "miss.app" + std::to_string(i), adapter);
                llvm::BasicBlock* nxtB = llvm::BasicBlock::Create(context, "miss.nxt" + std::to_string(i), adapter);
                abuilder.CreateCondBr(isMissPos, chkKwB, nxtB);
                abuilder.SetInsertPoint(chkKwB);
                llvm::BasicBlock* lookupB = llvm::BasicBlock::Create(context, "miss.lkw" + std::to_string(i), adapter);
                abuilder.CreateCondBr(kwIsNull, appB, lookupB);
                abuilder.SetInsertPoint(lookupB);
                if (fromStr && dictGetFn && i < pnames.size()) {
                    llvm::Value* nm = abuilder.CreateGlobalStringPtr(pnames[i], "miss.kn" + std::to_string(i));
                    llvm::Value* s = abuilder.CreateCall(fromStr, {nm}, "miss.ks" + std::to_string(i));
                    llvm::Value* got = abuilder.CreateCall(dictGetFn, {kwArgVal, s}, "miss.kv" + std::to_string(i));
                    llvm::Value* found = abuilder.CreateICmpNE(
                        got, llvm::ConstantPointerNull::get(pyObjectPtrTy), "miss.kf" + std::to_string(i));
                    if (auto* dec = module->getFunction("Py_DECREF"))
                        abuilder.CreateCall(dec, {got});
                    abuilder.CreateCondBr(found, nxtB, appB);
                } else {
                    abuilder.CreateBr(appB);
                }
                abuilder.SetInsertPoint(appB);
                if (fromStr && listAppend && i < pnames.size()) {
                    llvm::Value* nm = abuilder.CreateGlobalStringPtr(pnames[i], "miss.n" + std::to_string(i));
                    llvm::Value* s = abuilder.CreateCall(fromStr, {nm}, "miss.s" + std::to_string(i));
                    abuilder.CreateCall(listAppend, {mlist, s});
                }
                abuilder.CreateBr(nxtB);
                abuilder.SetInsertPoint(nxtB);
            }
            llvm::BasicBlock* doRaiseB = llvm::BasicBlock::Create(context, "miss.do", adapter);
            llvm::Value* msz = listSize
                ? (llvm::Value*)abuilder.CreateCall(listSize, {mlist}, "miss.sz")
                : (llvm::Value*)llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1);
            llvm::Value* missEmpty = abuilder.CreateICmpEQ(
                msz, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), "miss.empty");
            abuilder.CreateCondBr(missEmpty, okB, doRaiseB);
            abuilder.SetInsertPoint(doRaiseB);
            if (raiseFn) {
                llvm::Value* fns = abuilder.CreateGlobalStringPtr(applyFnName, "miss.fn");
                abuilder.CreateCall(raiseFn, {fns, mlist});
            }
            abuilder.CreateUnreachable();
            abuilder.SetInsertPoint(okB);
        }
        for (size_t i = 0; i < userFixed; ++i) {
            llvm::Value* iV = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), i);
            llvm::Value* have = abuilder.CreateICmpSGT(userLen, iV);
            llvm::BasicBlock* haveB = llvm::BasicBlock::Create(context, "have" + std::to_string(i), adapter);
            llvm::BasicBlock* checkKwB = llvm::BasicBlock::Create(context, "chkw" + std::to_string(i), adapter);
            llvm::BasicBlock* lookupKwB = llvm::BasicBlock::Create(context, "lkw" + std::to_string(i), adapter);
            llvm::BasicBlock* fromKwB = llvm::BasicBlock::Create(context, "fromkw" + std::to_string(i), adapter);
            llvm::BasicBlock* missB = llvm::BasicBlock::Create(context, "miss" + std::to_string(i), adapter);
            llvm::BasicBlock* after = llvm::BasicBlock::Create(context, "arg" + std::to_string(i), adapter);
            abuilder.CreateCondBr(have, haveB, checkKwB);
            abuilder.SetInsertPoint(haveB);
            llvm::Value* idxHave = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), ncells + i);
            llvm::Value* elHave = nullptr;
            if (listGet) elHave = abuilder.CreateCall(listGet, {argListVal, idxHave}, "a" + std::to_string(i));
            if (rejectDupFn && i < pnames.size()) {
                llvm::Value* dnm = abuilder.CreateGlobalStringPtr(pnames[i], "dup.n" + std::to_string(i));
                llvm::Value* dfn = abuilder.CreateGlobalStringPtr(applyFnName, "dup.fn" + std::to_string(i));
                abuilder.CreateCall(rejectDupFn, {kwArgVal, dnm, dfn});
            }
            abuilder.CreateBr(after);
            abuilder.SetInsertPoint(checkKwB);
            llvm::Value* kwNull = abuilder.CreateICmpEQ(
                kwArgVal, llvm::ConstantPointerNull::get(pyObjectPtrTy), "kwnull" + std::to_string(i));
            abuilder.CreateCondBr(kwNull, missB, lookupKwB);
            abuilder.SetInsertPoint(lookupKwB);
            llvm::Value* elKw = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            if (fromStr && dictGetFn && i < pnames.size()) {
                llvm::Value* knm = abuilder.CreateGlobalStringPtr(pnames[i], "kw.n" + std::to_string(i));
                llvm::Value* ks = abuilder.CreateCall(fromStr, {knm}, "kw.s" + std::to_string(i));
                elKw = abuilder.CreateCall(dictGetFn, {kwArgVal, ks}, "kw.v" + std::to_string(i));
            }
            llvm::Value* hasKw = abuilder.CreateICmpNE(
                elKw, llvm::ConstantPointerNull::get(pyObjectPtrTy), "kw.has" + std::to_string(i));
            abuilder.CreateCondBr(hasKw, fromKwB, missB);
            abuilder.SetInsertPoint(fromKwB);
            abuilder.CreateBr(after);
            abuilder.SetInsertPoint(missB);
            llvm::Value* elMiss = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            // Always try to load a default for this user position on miss.
            // For top-level defaulted funcs the convention is __default_<name>_<i>
            // for the i-th declared (regular) param when it has a default.
            // We also try the mapped dk and any recorded defSlots.
            {
                std::vector<size_t> cands;
                // Default slots are 0..ndef-1 (Compiler __default_<fn>_<k>).
                // Probe that index first: param index i equals a later slot
                // when firstDef > 0 and ndef > 1 (I-033).
                if (ndef > 0 && i >= firstDef) cands.push_back(i - firstDef);
                cands.push_back(i);
                for (size_t candk : cands) {
                    // conventional using the IR/python name
                    std::string conv = "__default_" + f.name + "_" + std::to_string(candk);
                    if (auto* gv = module->getNamedGlobal("pyc_global_" + conv)) {
                        elMiss = abuilder.CreateLoad(pyObjectPtrTy, gv, "d" + std::to_string(candk));
                        if (auto* inc = module->getFunction("Py_INCREF")) abuilder.CreateCall(inc, {elMiss});
                        break;
                    }
                    // using any probed/recorded slot name
                    if (candk < defSlots.size()) {
                        std::string gname = "pyc_global_" + defSlots[candk];
                        if (auto* gv = module->getNamedGlobal(gname)) {
                            elMiss = abuilder.CreateLoad(pyObjectPtrTy, gv, "d" + std::to_string(candk));
                            if (auto* inc = module->getFunction("Py_INCREF")) abuilder.CreateCall(inc, {elMiss});
                            break;
                        }
                    }
                }
            }
            abuilder.CreateBr(after);
            abuilder.SetInsertPoint(after);
            llvm::PHINode* phi = abuilder.CreatePHI(pyObjectPtrTy, 3);
            phi->addIncoming(elHave ? elHave : llvm::ConstantPointerNull::get(pyObjectPtrTy), haveB);
            phi->addIncoming(elKw, fromKwB);
            phi->addIncoming(elMiss, missB);
            cargs.push_back(phi);
        }

        // Separate kw channel (Pyc_ApplyKw): leftover keys → **kwargs or TypeError.
        // Trailing type-2 peel stays only when kwArg is null AND hasKwVar
        // (legacy append path). Never peel when kwargs arrived separately —
        // hs[0]({"a": 1}) is positional and uses Pyc_Apply (kwArg==null),
        // and must not be stolen for a function without **kwargs.
        llvm::Function* dictNewFn = module->getFunction("PyDict_New");
        llvm::Function* kwRestFn = module->getFunction("Pyc_ApplyKwRest");
        llvm::Value* kwargsVal = nullptr;
        llvm::Value* varEndIdx = ln;  // upper bound (exclusive) for the *args tail below
        llvm::Value* restKw = nullptr;
        {
            llvm::Value* zeroSz = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
            llvm::Value* nlist = listNew
                ? (llvm::Value*)abuilder.CreateCall(listNew, {zeroSz}, "kw.bound")
                : (llvm::Value*)llvm::ConstantPointerNull::get(pyObjectPtrTy);
            if (fromStr && listAppend && nlist) {
                for (size_t i = 0; i < userFixed && i < pnames.size(); ++i) {
                    llvm::Value* nm = abuilder.CreateGlobalStringPtr(pnames[i], "kw.bn" + std::to_string(i));
                    llvm::Value* s = abuilder.CreateCall(fromStr, {nm}, "kw.bs" + std::to_string(i));
                    abuilder.CreateCall(listAppend, {nlist, s});
                }
            }
            llvm::Value* hasKwI = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), hasKwVar ? 1 : 0);
            llvm::Value* fns = abuilder.CreateGlobalStringPtr(applyFnName, "kw.fn");
            if (kwRestFn) {
                restKw = abuilder.CreateCall(kwRestFn, {kwArgVal, nlist, hasKwI, fns}, "kw.rest");
            } else {
                restKw = dictNewFn
                    ? (llvm::Value*)abuilder.CreateCall(dictNewFn, {}, "kw.rest.empty")
                    : (llvm::Value*)llvm::ConstantPointerNull::get(pyObjectPtrTy);
            }
        }
        if (hasKwVar) {
            llvm::Value* kwNull = abuilder.CreateICmpEQ(
                kwArgVal, llvm::ConstantPointerNull::get(pyObjectPtrTy), "kw.argnull");
            llvm::Value* minRequired = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), ncells + userFixed);
            llvm::Value* hasExtra = abuilder.CreateICmpSGT(ln, minRequired, "kw.hasextra");
            llvm::Value* tryPeel = abuilder.CreateAnd(hasExtra, kwNull, "kw.trypeel");
            llvm::BasicBlock* checkB = llvm::BasicBlock::Create(context, "kw.check", adapter);
            llvm::BasicBlock* foundB = llvm::BasicBlock::Create(context, "kw.found", adapter);
            llvm::BasicBlock* noneB = llvm::BasicBlock::Create(context, "kw.none", adapter);
            llvm::BasicBlock* mergeB = llvm::BasicBlock::Create(context, "kw.merge", adapter);
            abuilder.CreateCondBr(tryPeel, checkB, noneB);
            abuilder.SetInsertPoint(checkB);
            llvm::Value* one64 = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1);
            llvm::Value* lastIdx = abuilder.CreateSub(ln, one64, "kw.lastidx");
            llvm::Value* lastEl = listGet
                ? (llvm::Value*)abuilder.CreateCall(listGet, {argListVal, lastIdx}, "kw.last")
                : (llvm::Value*)llvm::ConstantPointerNull::get(pyObjectPtrTy);
            llvm::Value* lastType = abuilder.CreateAlignedLoad(llvm::Type::getInt32Ty(context),
                abuilder.CreateStructGEP(pyObjectTy, lastEl, 1), llvm::Align(4), "kw.last.type");
            llvm::Value* isDict = abuilder.CreateICmpEQ(lastType,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 2), "kw.isdict");
            abuilder.CreateCondBr(isDict, foundB, noneB);
            abuilder.SetInsertPoint(foundB);
            llvm::Value* foundVal = lastEl;
            llvm::Value* foundEnd = lastIdx;
            abuilder.CreateBr(mergeB);
            abuilder.SetInsertPoint(noneB);
            // Prefer the ApplyKw leftover dict when kwargs arrived separately.
            llvm::Value* emptyVal = restKw ? restKw : (dictNewFn
                ? (llvm::Value*)abuilder.CreateCall(dictNewFn, {}, "kw.empty")
                : (llvm::Value*)llvm::ConstantPointerNull::get(pyObjectPtrTy));
            llvm::Value* emptyEnd = ln;
            abuilder.CreateBr(mergeB);
            abuilder.SetInsertPoint(mergeB);
            llvm::PHINode* valPhi = abuilder.CreatePHI(pyObjectPtrTy, 2, "kw.val");
            valPhi->addIncoming(foundVal, foundB);
            valPhi->addIncoming(emptyVal, noneB);
            llvm::PHINode* endPhi = abuilder.CreatePHI(llvm::Type::getInt64Ty(context), 2, "kw.end");
            endPhi->addIncoming(foundEnd, foundB);
            endPhi->addIncoming(emptyEnd, noneB);
            kwargsVal = valPhi;
            varEndIdx = endPhi;
        }

        if (hasVar) {
            // Collect [ncells + userFixed .. varEndIdx) into a fresh list for
            // the * slot. varEndIdx excludes a trailing kwargs dict (see above).
            llvm::Value* startC = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), ncells + userFixed);
            llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
            llvm::Value* rest = nullptr;
            if (listNew) {
                rest = abuilder.CreateCall(listNew, {zero}, "rest");
            } else {
                rest = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            }

            // Inline counted loop: j = start; while j < varEndIdx { append GetItem(j); j++ }
            llvm::AllocaInst* jAlloca = abuilder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "j");
            abuilder.CreateStore(startC, jAlloca);

            llvm::BasicBlock* lp = llvm::BasicBlock::Create(context, "tail_lp", adapter);
            llvm::BasicBlock* bd = llvm::BasicBlock::Create(context, "tail_bd", adapter);
            llvm::BasicBlock* ex = llvm::BasicBlock::Create(context, "tail_ex", adapter);
            abuilder.CreateBr(lp);
            abuilder.SetInsertPoint(lp);
            llvm::Value* jcur = abuilder.CreateLoad(llvm::Type::getInt64Ty(context), jAlloca, "jcur");
            llvm::Value* cmp = abuilder.CreateICmpSLT(jcur, varEndIdx, "cm");
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

        if (hasKwVar) {
            cargs.push_back(kwargsVal ? kwargsVal : llvm::ConstantPointerNull::get(pyObjectPtrTy));
        }

        llvm::Value* r = abuilder.CreateCall(real, cargs, "r");
        abuilder.CreateRet(r);
    }

    // Declare the registration function so we can call it from the module ctor.
    llvm::FunctionType* regTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {int8PtrTy, pyObjectPtrTy}, false);
    llvm::Function* regFn = module->getFunction("pyc_register_callable");
    if (!regFn) regFn = llvm::Function::Create(regTy, llvm::Function::ExternalLinkage, "pyc_register_callable", module.get());
    llvm::Function* regKwFn = module->getFunction("pyc_register_callable_kw");
    if (!regKwFn) regKwFn = llvm::Function::Create(regTy, llvm::Function::ExternalLinkage, "pyc_register_callable_kw", module.get());

    // Build set of user-defined function names (both IR name and LLVM mangled name).
    // Used by Fix 2 to identify discarded call results that are safe to free immediately.
    // Forward-declared user functions have isDeclaration()=true at codegen time (body not
    // yet generated), so we cannot rely on !callee->isDeclaration() for this check.
    std::unordered_set<std::string> userFunctionNames;
    for (const auto& f : ir.functions) {
        if (!f.name.empty()) {
            userFunctionNames.insert(f.name);
            userFunctionNames.insert(llvmFunctionName(f.name));
        }
    }

    // Debug info: per-function DISubprogram and DILexicalBlock.
    std::unordered_map<std::string, llvm::DISubprogram*> diSubprograms;
    std::unordered_map<std::string, llvm::DILexicalBlock*> diBlocks;

    for (const auto& f : ir.functions) {
        std::string irName = llvmFunctionName(f.name);
        llvm::Function* func = module->getFunction(irName);
        if (!func) continue;

        // Debug info: create DISubprogram for this function
        llvm::DISubprogram* subprog = nullptr;
        llvm::DILexicalBlock* lexBlock = nullptr;
        if (debugInfo && diBuilder && diCU) {
            // Use the function's source file if available, otherwise the module-level file
            llvm::DIFile* funcDiFile = diFile;
            if (!f.sourceFile.empty()) {
                std::filesystem::path p(f.sourceFile);
                funcDiFile = diBuilder->createFile(p.filename().string(),
                    p.parent_path().empty() ? "." : p.parent_path().string());
            }
            int defLine = f.defLineno > 0 ? f.defLineno : 1;
            // For specialized variants, use the original function name for debugging
            std::string displayName = f.name;
            if (displayName.find("__specialized_") == 0) {
                // Extract original function name: __specialized_<name>_<sig>
                size_t prefixLen = 14; // "__specialized_"
                size_t sigStart = displayName.rfind('_');
                if (sigStart != std::string::npos && sigStart > prefixLen) {
                    displayName = displayName.substr(prefixLen, sigStart - prefixLen);
                }
            }
            // A6 specialized variants keep the original Python name so
            // `break fib` matches, but they are FlagArtificial so gdb
            // treats them as compiler-generated (I-019).
            llvm::DINode::DIFlags diFlags = llvm::DINode::FlagPrototyped;
            if (f.name.find("__specialized_") == 0)
                diFlags |= llvm::DINode::FlagArtificial;
            subprog = diBuilder->createFunction(
                diCU,
                displayName,       // display name (Python function name)
                irName,            // linkage name (LLVM function name)
                funcDiFile,
                defLine,
                diSubroutineType,
                defLine,           // scope line
                diFlags,
                llvm::DISubprogram::SPFlagDefinition
            );
            func->setSubprogram(subprog);
            lexBlock = diBuilder->createLexicalBlock(subprog, funcDiFile, defLine, 0);
            diSubprograms[f.name] = subprog;
            diBlocks[f.name] = lexBlock;
        }

        // Variable tracking: DI types and helper (created when debugInfo is enabled).
        // These are used below to emit DbgDeclareInst for each alloca.
        llvm::DIType* diPyObjPtrDI = nullptr;
        llvm::DIType* diIntDI = nullptr;
        llvm::DIType* diFloatDI = nullptr;
        llvm::DILexicalBlock* varLexBlock = nullptr;
        llvm::DIFile* varDiFile = nullptr;
        int varDefLine = 1;
        if (debugInfo && diBuilder && diCU && subprog) {
            llvm::DIFile* funcDiFileForVars = diFile;
            if (!f.sourceFile.empty()) {
                std::filesystem::path p(f.sourceFile);
                funcDiFileForVars = diBuilder->createFile(p.filename().string(),
                    p.parent_path().empty() ? "." : p.parent_path().string());
            }
            int dlv = f.defLineno > 0 ? f.defLineno : 1;
            // 4-field composite matching Codegen's LLVM PyObject (I-043).
            // GDB can then v["type"] / v["value"] / v["dvalue"] on user locals
            // compiled -g -O0. Later C++ fields (list/str/cell_content) are
            // only visible if runtime.bc was built with -g (I-044).
            llvm::DIType* diI32 = diBuilder->createBasicType(
                "int", 32, llvm::dwarf::DW_ATE_signed);
            llvm::DIType* diI64 = diBuilder->createBasicType(
                "int64_t", 64, llvm::dwarf::DW_ATE_signed);
            llvm::DIType* diF64 = diBuilder->createBasicType(
                "double", 64, llvm::dwarf::DW_ATE_float);
            llvm::SmallVector<llvm::Metadata*, 4> pyMembers;
            pyMembers.push_back(diBuilder->createMemberType(
                diCU, "refcount", funcDiFileForVars, 0, 32, 32, 0,
                llvm::DINode::FlagZero, diI32));
            pyMembers.push_back(diBuilder->createMemberType(
                diCU, "type", funcDiFileForVars, 0, 32, 32, 32,
                llvm::DINode::FlagZero, diI32));
            pyMembers.push_back(diBuilder->createMemberType(
                diCU, "value", funcDiFileForVars, 0, 64, 64, 64,
                llvm::DINode::FlagZero, diI64));
            pyMembers.push_back(diBuilder->createMemberType(
                diCU, "dvalue", funcDiFileForVars, 0, 64, 64, 128,
                llvm::DINode::FlagZero, diF64));
            llvm::DIType* pyObjStruct = diBuilder->createStructType(
                diCU, "PyObject", funcDiFileForVars, 0, 192, 64,
                llvm::DINode::FlagZero, nullptr,
                diBuilder->getOrCreateArray(pyMembers));
            diPyObjPtrDI = diBuilder->createPointerType(
                pyObjStruct, 64, 64, std::nullopt, "PyObject*");
            // int64_t and double base types.
            diIntDI = diBuilder->createBasicType(
                "int64_t", 64, llvm::dwarf::DW_ATE_signed);
            diFloatDI = diBuilder->createBasicType(
                "double", 64, llvm::dwarf::DW_ATE_float);
            varLexBlock = lexBlock;
            varDiFile = funcDiFileForVars;
            varDefLine = dlv;
        }

        // Helper lambda: emit DbgDeclareInst for an alloca.
        // Only does work when debugInfo is enabled (DI types are non-null).
        auto emitDbgDeclare = [&](llvm::AllocaInst* alloca, const std::string& pythonName,
                                  llvm::DIType* diType) {
            if (!diType || !varLexBlock || !varDiFile) return;
            llvm::DILocalVariable* diVar = diBuilder->createAutoVariable(
                varLexBlock,       // Scope (lexical block)
                pythonName,        // Variable name
                varDiFile,         // Source file
                varDefLine,        // Line number
                diType,            // Type
                false,             // AlwaysPreserve
                llvm::DINode::FlagZero);  // Flags
            // Insert at the start of the entry block.
            llvm::DILocation* diLoc = llvm::DILocation::get(context, varDefLine, 0, varLexBlock);
            llvm::InsertPosition insertPt(func->getEntryBlock().begin());
            diBuilder->insertDeclare(alloca, diVar, diBuilder->createExpression(), diLoc, insertPt);
        };

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
                // 2-arg adapters (args, kwargs) go in the kw registry.
                if (regKwFn) {
                    builder.CreateCall(regKwFn, {nameStr, adp});
                }
            }
        }

        std::unordered_map<std::string, llvm::Value*> valueMap;
        std::unordered_set<std::string> ownedSlots;
        std::unordered_set<std::string> ownedTemps; // names of temps with new refs (refcount=1)
        std::unordered_map<std::string, int> tempUseCounts; // how many times each name is used as an operand
        // Block where each owned temp was defined. Used to decide whether it is safe to DECREF
        // the temp when used as a call arg or comparison operand: if it was defined in a DIFFERENT
        // block it may be loop-persistent (referenced from multiple loop iterations), so we skip
        // the DECREF there. If it was defined in the SAME block it is definitely not loop-persistent.
        std::unordered_map<std::string, llvm::BasicBlock*> tempDefBlock;
        // Builder for inserting allocas at the function's entry block.
        // All allocas must dominate all uses, so they're always inserted here.
        llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                       func->getEntryBlock().begin());
        // A6: Detect specialized variants for native param allocation.
        bool funcIsSpecialized = (f.name.find("__specialized_") == 0);
        size_t nativeParamStart = 0;
        std::string nativeSig;
        // A6 native return: "int" → i64, "float" → double, "" → boxed PyObject*
        std::string nativeRetType;
        if (funcIsSpecialized) {
            nativeParamStart = f.freeCellVars.size();
            if (nativeParamStart < f.args.size() && f.name.size() > nativeParamStart) {
                nativeSig = f.name.substr(f.name.size() - (f.args.size() - nativeParamStart));
            }
            nativeRetType = f.nativeReturnType;
        }
        for (size_t i = 0; i < f.args.size(); ++i) {
            llvm::Value* arg = func->getArg(i);
            if (!f.args[i].empty()) {
                arg->setName(f.args[i]);
                // A6: For specialized variants, user params get native-typed allocas for numeric types,
                // PyObject* allocas for non-numeric types (str/list/dict).
                llvm::Type* slotType = pyObjectPtrTy;
                if (funcIsSpecialized && i >= nativeParamStart) {
                    size_t sigIdx = i - nativeParamStart;
                    if (sigIdx < nativeSig.size()) {
                        char t = nativeSig[sigIdx];
                        if (t == 'f') {
                            slotType = llvm::Type::getDoubleTy(context);
                        } else if (t == 'i') {
                            slotType = llvm::Type::getInt64Ty(context);
                        }
                        // Non-numeric types (s/l/d) keep slotType as pyObjectPtrTy
                    }
                }
                // For parameters, create an entry-block alloca that shadows
                // the parameter, and add the *alloca* to valueMap. This way
                // subsequent assigns to the parameter name write to the
                // alloca (and can be observed by future loads), and
                // initial reads return the parameter value. The alloca is
                // initialised in the entry block so it dominates all uses.
                llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(slotType, nullptr, f.args[i] + ".slot");
                entryBuilder.CreateStore(arg, alloca);
                valueMap[f.args[i]] = alloca;
                // Debug info: track this parameter variable.
                emitDbgDeclare(alloca, f.args[i], diPyObjPtrDI);
                // B5: if this is a hidden cell parameter (suffixed _cell from freeCellVars),
                // INCREF the received cell so the local slot owns a reference. The provider
                // (bundle list or Pyc arg list) may drop its ref after the call returns.
                if (!f.args[i].empty()) {
                    const std::string& an = f.args[i];
                    if (an.size() > 5 && an.rfind("_cell") == an.size() - 5) {
                        if (llvm::Function* increfFn = module->getFunction("Py_INCREF")) {
                            entryBuilder.CreateCall(increfFn, {arg});
                        }
                    }
                 }
             } else {
                 // Use a synthetic name if args are empty
                 std::string synthName = "arg" + std::to_string(i);
                 arg->setName(synthName);
                 valueMap[synthName] = arg;
             }
         }
          // A7: For params in numericFloatLocals/numericLocals, create native f64/i64 allocas
          // so the native chain can start from the param. Insert at end of entry block.
          {
              llvm::IRBuilder<> endBuilder(&func->getEntryBlock(), func->getEntryBlock().end());
              llvm::LLVMContext& ctx = func->getContext();
              for (size_t pi = 0; pi < f.args.size(); ++pi) {
                  const auto& pname = f.args[pi];
                  if (pname.empty()) continue;
                  bool isFloat = false, isInt = false;
                  for (const auto& nfl : f.numericFloatLocals) { if (nfl == pname) { isFloat = true; break; } }
                  for (const auto& nl : f.numericLocals) { if (nl == pname) { isInt = true; break; } }
                  if (!isFloat && !isInt) continue;
                  if (pname.size() > 5 && pname.rfind("_cell") == pname.size() - 5) continue;
                  llvm::AllocaInst* boxedSlotAlloca = nullptr;
                  auto bsit = valueMap.find(pname);
                  if (bsit != valueMap.end()) {
                      if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(bsit->second)) boxedSlotAlloca = a;
                  }
                  if (!boxedSlotAlloca || boxedSlotAlloca->getAllocatedType() != pyObjectPtrTy) continue;
                   auto nativeTy = isFloat ? llvm::Type::getDoubleTy(ctx) : llvm::Type::getInt64Ty(ctx);
                   auto* nativeAlloca = endBuilder.CreateAlloca(nativeTy, nullptr, pname + ".native");
                   // Debug info: track this native variable slot.
                   emitDbgDeclare(nativeAlloca, pname, isFloat ? diFloatDI : diIntDI);
                    // Unbox from boxed slot and store to native
                   llvm::Value* boxedPtr = endBuilder.CreateLoad(pyObjectPtrTy, boxedSlotAlloca, pname + ".boxed");
                   if (isFloat) {
                       llvm::Value* typeTag = endBuilder.CreateAlignedLoad(llvm::Type::getInt32Ty(ctx),
                           endBuilder.CreateStructGEP(pyObjectTy, boxedPtr, 1), llvm::Align(4), pname + ".type");
                       llvm::Value* isFloatTag = endBuilder.CreateICmpEQ(typeTag,
                           llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 4), pname + ".isfloat");
                       llvm::Value* dval = endBuilder.CreateAlignedLoad(llvm::Type::getDoubleTy(ctx),
                           endBuilder.CreateStructGEP(pyObjectTy, boxedPtr, 3), llvm::Align(8), pname + ".dval");
                       llvm::Value* ival = endBuilder.CreateAlignedLoad(nativeTy,
                           endBuilder.CreateStructGEP(pyObjectTy, boxedPtr, 2), llvm::Align(8), pname + ".ival");
                       llvm::Value* i2f = endBuilder.CreateSIToFP(ival, llvm::Type::getDoubleTy(ctx), pname + ".i2f");
                       llvm::Value* unboxed = endBuilder.CreateSelect(isFloatTag, dval, i2f, pname + ".unboxed");
                       endBuilder.CreateStore(unboxed, nativeAlloca);
                   } else {
                       llvm::Value* typeTag = endBuilder.CreateAlignedLoad(llvm::Type::getInt32Ty(ctx),
                           endBuilder.CreateStructGEP(pyObjectTy, boxedPtr, 1), llvm::Align(4), pname + ".type");
                       llvm::Value* isIntTag = endBuilder.CreateICmpEQ(typeTag,
                           llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), pname + ".isint");
                       llvm::Value* iv = endBuilder.CreateAlignedLoad(llvm::Type::getInt64Ty(ctx),
                           endBuilder.CreateStructGEP(pyObjectTy, boxedPtr, 2), llvm::Align(8), pname + ".ival");
                       llvm::Value* dv = endBuilder.CreateAlignedLoad(llvm::Type::getDoubleTy(ctx),
                           endBuilder.CreateStructGEP(pyObjectTy, boxedPtr, 3), llvm::Align(8), pname + ".dval");
                       llvm::Value* f2i = endBuilder.CreateFPToSI(dv, nativeTy, pname + ".f2i");
                       llvm::Value* sel = endBuilder.CreateSelect(isIntTag, iv, f2i, pname + ".unboxed");
                       endBuilder.CreateStore(sel, nativeAlloca);
                   }
                  valueMap[pname] = nativeAlloca;
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
        {
            llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(), func->getEntryBlock().begin());
            // Free cells (received as hidden leading args)
            for (size_t ci = 0; ci < f.freeCellVars.size(); ++ci) {
                const auto& cname = f.freeCellVars[ci];
                std::string slot = cname + "_cell";
                if (valueMap.count(slot)) continue;
                llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, slot + ".slot");
                // Debug info: track this cell variable.
                emitDbgDeclare(alloca, slot, diPyObjPtrDI);
                // B: explicitly wire the incoming cell arg (hidden leading params) into the slot.
                // Hidden cells are the first N args in the order of freeCellVars.
                if (ci < func->arg_size()) {
                    llvm::Value* cellArg = func->getArg(ci);
                    // Most likely fix: the activation must own a ref to the cell while it runs.
                    // The provider (bundle list or caller) may release its list/arg after the call.
                    // INCREF here; the owned-slot exit cleanup will DECREF.
                    if (llvm::Function* inc = module->getFunction("Py_INCREF")) {
                        entryBuilder.CreateCall(inc, {cellArg});
                    }
                    entryBuilder.CreateStore(cellArg, alloca);
                    // Most likely fix: the closure body must own a ref to the received cell
                    // for the duration of its activation (the bundle list or Pyc arg list may
                    // be released after the call). We INCREFed above; mark the slot owned so
                    // the normal exit cleanup will DECREF it, balancing the INCREF.
                    ownedSlots.insert(slot);
                } else {
                    entryBuilder.CreateStore(llvm::ConstantPointerNull::get(pyObjectPtrTy), alloca);
                }
                valueMap[slot] = alloca;
            }
            // Owned cells (allocated locally via PyCell_New in IR; slot holds the cell object)
            for (const auto& cname : f.cellVars) {
                std::string slot = cname + "_cell";
                if (valueMap.count(slot)) continue;
                llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, slot + ".slot");
                // Debug info: track this cell variable.
                emitDbgDeclare(alloca, slot, diPyObjPtrDI);
                // B: null-init so first assign (PyCell_New) can safely DECREF old (null is safe).
                entryBuilder.CreateStore(llvm::ConstantPointerNull::get(pyObjectPtrTy), alloca);
                valueMap[slot] = alloca;
            }
        }

        std::unordered_map<std::string, llvm::BasicBlock*> blockMap;
        blockMap["entry"] = entry;
        // Map from jmpVar name to a pre-allocated jmp_buf (created in the
        // entry block so its address is stable across longjmps).
        std::unordered_map<std::string, llvm::AllocaInst*> jmpBufAllocas;
        // Set an initial debug location (the function's def line) so that
        // instructions with lineno == 0 (e.g. specialized-variant recursive
        // calls) still have a valid !dbg location — LLVM's verifier rejects
        // inlinable function calls that lack one.
        if (debugInfo && lexBlock) {
            int defLine = f.defLineno > 0 ? f.defLineno : 1;
            builder.SetCurrentDebugLocation(
                llvm::DILocation::get(context, defLine, 0, lexBlock));
        }
        for (const auto& inst : f.body) {
            // Debug info: set the current debug location from the IR instruction's lineno.
            // The builder will attach this DebugLoc to every LLVM instruction it creates.
            // When inst.lineno == 0 (synthesized instructions, e.g. specialized-variant
            // calls), keep the last known non-null location rather than resetting to
            // nullptr — LLVM's verifier rejects inlinable function calls that lack a
            // !dbg location, and "inherit the enclosing statement's line" is the
            // standard DWARF pattern for lineless instructions.
            if (debugInfo && lexBlock && inst.lineno > 0) {
                builder.SetCurrentDebugLocation(
                    llvm::DILocation::get(context, inst.lineno, 0, lexBlock));
            }
            if (inst.op == "label") {
                const std::string& ln = inst.result;
                if (blockMap.find(ln) == blockMap.end()) {
                    blockMap[ln] = llvm::BasicBlock::Create(context, ln, func);
                }
            } else if (inst.op == "try_begin") {
                if (!inst.operands.empty()) {
                    const std::string& jn = inst.operands[0].name;
                    if (jmpBufAllocas.find(jn) == jmpBufAllocas.end()) {
                        // jmp_buf is typically 200 bytes; use 256 to be safe.
                        llvm::AllocaInst* a = entryBuilder.CreateAlloca(
                            llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 256),
                            nullptr, jn + ".buf");
                        jmpBufAllocas[jn] = a;
                    }
                }
             }
         }

                 auto getOrLoad = [&](const std::string& name) -> llvm::Value* {
                    // Special-case `sys` so user code can do `sys.argv` etc. The
                    // runtime provides a `pyc_get_sys_module()` accessor that
                    // returns the same global `sys` object every call.
                    if (name == "__name__") {
                        // Return cached value if still owned; otherwise create fresh and track it.
                        if (ownedTemps.count("__name__") && valueMap.count("__name__"))
                            return valueMap.at("__name__");
                        llvm::Function* fromStr = module->getFunction("PyUnicode_FromString");
                        llvm::Value* strConst = builder.CreateGlobalStringPtr("__main__", "str");
                        llvm::Value* val = builder.CreateCall(fromStr, {strConst}, name + ".name");
                        valueMap["__name__"] = val;
                        ownedTemps.insert("__name__");
                        tempDefBlock["__name__"] = builder.GetInsertBlock();
                        return val;
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

        // Helper: unbox a PyObject* (assumed to be int) to i64.
        // A null boxed pointer (e.g. from a function that returned None
        // or failed) is treated as 0 to avoid a null-pointer deref when
        // the result is used as a branch condition or in arithmetic.
        // We use a static dummy PyObject whose .value field is 0, so the
        // load through it is well-defined even when the original boxed
        // pointer is null.
        static llvm::GlobalVariable* g_zeroPyObj = nullptr;
        auto unboxToI64 = [&](llvm::Value* boxed) -> llvm::Value* {
            if (!boxed || boxed->getType() == llvm::Type::getInt64Ty(context)) return boxed;
            if (!g_zeroPyObj) {
                // Allocate a zero-initialised PyObject in the global
                // address space; we use it as a safe stand-in for null
                // when unboxing.
                llvm::Type* pyObjPtrTy = llvm::PointerType::get(pyObjectTy, 0);
                g_zeroPyObj = new llvm::GlobalVariable(*module, pyObjectTy, false,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::Constant::getNullValue(pyObjectTy), "__pyc_zero_obj");
            }
            llvm::Value* zeroObjPtr = builder.CreateBitCast(g_zeroPyObj,
                boxed->getType(), "zero.obj");
            llvm::Value* safeBoxed = builder.CreateSelect(builder.CreateIsNull(boxed, "isnull"),
                zeroObjPtr, boxed, "safe.boxed");
            llvm::Value* fieldPtr = builder.CreateStructGEP(pyObjectTy, safeBoxed, 2);
            return builder.CreateAlignedLoad(llvm::Type::getInt64Ty(context), fieldPtr, llvm::Align(8), "unboxed");
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
            if (v->getType() == llvm::Type::getInt1Ty(context)) {
                // Native i1 (from native icmp) → box to PyBool_New
                llvm::Function* boolNew = module->getFunction("PyBool_New");
                if (!boolNew) return llvm::ConstantPointerNull::get(pyObjectPtrTy);
                llvm::Value* i32v = builder.CreateZExt(v, llvm::Type::getInt32Ty(context));
                return builder.CreateCall(boolNew, {i32v}, name + ".bool");
            }
            return v;
        };

        auto unboxToDouble = [&](llvm::Value* boxed) -> llvm::Value* {
            llvm::Type* doubleTy = llvm::Type::getDoubleTy(context);
            if (!boxed || boxed->getType() == doubleTy) return boxed;
            if (boxed->getType() == llvm::Type::getInt64Ty(context)) {
                return builder.CreateSIToFP(boxed, doubleTy, "i64.to.double");
            }

            llvm::Value* typeVal = builder.CreateAlignedLoad(llvm::Type::getInt32Ty(context),
                builder.CreateStructGEP(pyObjectTy, boxed, 1), llvm::Align(4), "boxed.type");
            llvm::Value* intVal = builder.CreateAlignedLoad(llvm::Type::getInt64Ty(context),
                builder.CreateStructGEP(pyObjectTy, boxed, 2), llvm::Align(8), "boxed.int");
            llvm::Value* intAsDouble = builder.CreateSIToFP(intVal, doubleTy, "boxed.int.double");
            llvm::Value* doubleVal = builder.CreateAlignedLoad(doubleTy,
                builder.CreateStructGEP(pyObjectTy, boxed, 3), llvm::Align(8), "boxed.double");
            llvm::Value* isFloat = builder.CreateICmpEQ(typeVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 4), "boxed.isfloat");
            return builder.CreateSelect(isFloat, doubleVal, intAsDouble, "unboxed.double");
        };

        auto boxDouble = [&](llvm::Value* val, const std::string& name = "") -> llvm::Value* {
            llvm::Function* fromDouble = module->getFunction("PyFloat_FromDouble");
            if (!fromDouble) return llvm::ConstantPointerNull::get(pyObjectPtrTy);
            return builder.CreateCall(fromDouble, {val}, name);
        };

        // Emit Py_DECREF for a named temp if it is in ownedTemps (new ref not yet consumed).
        // Tracks remaining uses so the DECREF fires only on the last use of the temp,
        // preventing premature frees when the same temp appears as an operand multiple times
        // (e.g., a list passed to several PyList_SetItemBoxed calls before being assigned).
        auto emitDecRefIfOwned = [&](const std::string& name) {
            if (!ownedTemps.count(name)) return;
            auto it = tempUseCounts.find(name);
            if (it != tempUseCounts.end() && it->second > 0) {
                --it->second;
                if (it->second > 0) return; // still more uses pending — don't DECREF yet
            }
            // Resolve the LLVM value BEFORE erasing from ownedTemps.
            // getOrLoad("__name__") re-checks ownedTemps for its cache; erasing
            // first causes it to allocate a fresh PyUnicode instead of returning
            // the original, leaking the original forever.
            llvm::Value* toDecref = getAsPyObject(name);
            ownedTemps.erase(name);
            llvm::Function* decref = module->getFunction("Py_DECREF");
            if (decref) builder.CreateCall(decref, {toDecref});
        };

        // Like emitDecRefIfOwned but only fires when the temp was defined in the SAME basic
        // block as the current insert point. Use this for call arguments and comparison
        // operands: a temp defined in a different block may be loop-persistent (produced before
        // the loop and consumed inside it on every iteration), so freeing it here would cause
        // use-after-free on subsequent iterations. Temps defined in the same block cannot have
        // been created by a prior loop iteration, so freeing them is always safe.
        auto emitDecRefIfOwnedSameBlock = [&](const std::string& name) {
            if (!ownedTemps.count(name)) return;
            auto blkIt = tempDefBlock.find(name);
            if (blkIt == tempDefBlock.end() || blkIt->second != builder.GetInsertBlock()) return;
            emitDecRefIfOwned(name);
        };

        // Record a temp as owned (new ref, refcount=1) and note which basic block defined it.
        auto markOwned = [&](const std::string& name) {
            ownedTemps.insert(name);
            tempDefBlock[name] = builder.GetInsertBlock();
        };

        auto emitNativeNumericBinary = [&](const IRInstruction& inst,
                                           const std::string& op) -> bool {
            if (inst.operands.size() < 2) return false;
            
            // Check if both operands are native (i64 or double).
            // This handles the case where resultType is "boxed" but operands
            // are proven numeric (e.g., function parameters that are numeric).
            llvm::Value* rawL = getOrLoad(inst.operands[0].name);
            llvm::Value* rawR = getOrLoad(inst.operands[1].name);
            bool lhsIsNative = rawL && (rawL->getType() == llvm::Type::getInt64Ty(context)
                                        || rawL->getType()->isDoubleTy());
            bool rhsIsNative = rawR && (rawR->getType() == llvm::Type::getInt64Ty(context)
                                        || rawR->getType()->isDoubleTy());
            bool bothNative = lhsIsNative && rhsIsNative;

            // Guard against a stale compile-time resultType="int" disagreeing with
            // the actual native operand type. inst.resultType is baked in during
            // lowering from a body-only static guess (inferParamTypesFromBody),
            // which runs before call-site type analysis; if a later, authoritative
            // pass (generateParamTypeAnalysis) determines the param is actually
            // float and allocates it as a native double, trusting resultType=="int"
            // here would route a double operand into the i64 unboxing path below,
            // which calls CreateIsNull on it — an LLVM type mismatch that crashes
            // the compiler outright. Found via `def f(y): return y ** 2; f(3.5)`.
            bool eitherRawDouble = (rawL && rawL->getType()->isDoubleTy())
                                 || (rawR && rawR->getType()->isDoubleTy());

            if ((inst.resultType == "int" && !eitherRawDouble) || (bothNative && rawL->getType() == llvm::Type::getInt64Ty(context) && rawR->getType() == llvm::Type::getInt64Ty(context))) {
                llvm::Value* lhs = unboxToI64(getOrLoad(inst.operands[0].name));
                llvm::Value* rhs = unboxToI64(getOrLoad(inst.operands[1].name));
                llvm::Value* native = nullptr;
                if (op == "add") {
                    native = builder.CreateAdd(lhs, rhs, inst.result + ".i64");
                } else if (op == "sub") {
                    native = builder.CreateSub(lhs, rhs, inst.result + ".i64");
                } else if (op == "mul") {
                    native = builder.CreateMul(lhs, rhs, inst.result + ".i64");
                } else if (op == "lshift") {
                    native = builder.CreateShl(lhs, rhs, inst.result + ".i64");
                } else if (op == "rshift") {
                    native = builder.CreateLShr(lhs, rhs, inst.result + ".i64");
                } else if (op == "bitor") {
                    native = builder.CreateOr(lhs, rhs, inst.result + ".i64");
                } else if (op == "bitand") {
                    native = builder.CreateAnd(lhs, rhs, inst.result + ".i64");
                } else if (op == "bitxor") {
                    native = builder.CreateXor(lhs, rhs, inst.result + ".i64");
                } else {
                    return false;
                }
                valueMap[inst.result] = native;
                if (!bothNative) {
                    emitDecRefIfOwned(inst.operands[0].name);
                    emitDecRefIfOwned(inst.operands[1].name);
                }
                return true;
            }
            // Native float if result is float, both sides native doubles, OR either
            // side is already a native double (e.g. mag from pow/fmul chain × boxed m1).
            // But NOT when the boxed side's IR type is "boxed" (not "float") — a boxed
            // operand could be complex, and the native float fast path would silently
            // drop the imaginary part (found: -0.0 + 0j produced 0.0, not 0j).
            bool eitherNativeDbl =
                (lhsIsNative && rawL->getType()->isDoubleTy()) ||
                (rhsIsNative && rawR->getType()->isDoubleTy());
            if (inst.resultType == "float" ||
                (bothNative && rawL->getType()->isDoubleTy() && rawR->getType()->isDoubleTy()) ||
                (eitherNativeDbl && inst.resultType != "boxed" && (op == "add" || op == "sub" || op == "mul"))) {
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
                if (!lhsIsNative) emitDecRefIfOwned(inst.operands[0].name);
                if (!rhsIsNative) emitDecRefIfOwned(inst.operands[1].name);
                return true;
            }
            return false;
        };

        // Pre-count how many times each name appears as an operand across the entire
        // function body. emitDecRefIfOwned uses this to defer the DECREF until the
        // last use, so a temp that feeds multiple instructions (e.g. a list object
        // passed to several PyList_SetItemBoxed calls) isn't freed prematurely.
        for (const auto& instr : f.body) {
            for (const auto& op : instr.operands) {
                if (!op.name.empty()) ++tempUseCounts[op.name];
            }
        }

        // I-009: push a Python-level frame once the entry block will actually
        // run. __apply__ adapters are generated in a separate loop and are
        // not instrumented. Display name matches DWARF: <module> for the
        // module body, original name for __specialized_* variants.
        {
            std::string displayName = !f.displayName.empty() ? f.displayName : f.name;
            if (displayName == "__module__") {
                displayName = "<module>";
            } else if (displayName.find("__specialized_") == 0) {
                size_t prefixLen = 14; // "__specialized_"
                size_t sigStart = displayName.rfind('_');
                if (sigStart != std::string::npos && sigStart > prefixLen) {
                    displayName = displayName.substr(prefixLen, sigStart - prefixLen);
                }
            }
            // Traceback `in` names are co_name, not qualname (I-037).
            // `outer.<locals>.inner` → `inner`; `C.foo` → `foo`.
            {
                size_t dot = displayName.rfind('.');
                if (dot != std::string::npos && dot + 1 < displayName.size())
                    displayName = displayName.substr(dot + 1);
            }
            std::string tbFile = !f.sourceFile.empty() ? f.sourceFile : tbFallbackFile;
            if (llvm::Function* pushFn = module->getFunction("Pyc_PushFrame")) {
                llvm::Value* fileStr = builder.CreateGlobalStringPtr(tbFile, "tb.file." + f.name);
                llvm::Value* funcStr = builder.CreateGlobalStringPtr(displayName, "tb.func." + f.name);
                builder.CreateCall(pushFn, {fileStr, funcStr});
            }
        }
        auto emitPopFrame = [&]() {
            if (llvm::Function* popFn = module->getFunction("Pyc_PopFrame"))
                builder.CreateCall(popFn, {});
        };

        llvm::BasicBlock* curBlock = entry;
        int lastTbLine = 0;
        int specUnboxSeq = 0;
        for (const auto& inst : f.body) {
            // Debug info: set the current debug location from the IR instruction's
            // lineno. Same logic as the pre-pass loop above — keep the last known
            // non-null location when inst.lineno == 0.
            if (debugInfo && lexBlock && inst.lineno > 0) {
                builder.SetCurrentDebugLocation(
                    llvm::DILocation::get(context, inst.lineno, 0, lexBlock));
            }
            // C: skip any non-label instruction if current block already terminated
            // (can happen if IR list has ops after a 'ret' due to control-flow lowering).
            // Labels are allowed because they may switch to a live block.
            if (curBlock->getTerminator() && inst.op != "label") {
                continue;
            }
            if (inst.op == "label") {
                auto it = blockMap.find(inst.result);
                if (it != blockMap.end()) {
                    llvm::BasicBlock* target = it->second;
                    if (target == curBlock) {
                        // Label re-entered the same block — no-op.
                    } else if (curBlock->getTerminator()) {
                        // curBlock is already terminated; just switch.
                    } else {
                        builder.CreateBr(target);
                    }
                    builder.SetInsertPoint(target);
                    curBlock = target;
                }
            }
            // I-009: update the top frame's line before the op runs so a
            // raise (explicit or runtime) records this statement. Skip if
            // the block is already terminated or the line did not change.
            if (!curBlock->getTerminator() && inst.lineno > 0 && inst.lineno != lastTbLine) {
                lastTbLine = inst.lineno;
                if (llvm::Function* setLn = module->getFunction("Pyc_SetLineno")) {
                    builder.CreateCall(setLn, {
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context),
                                               static_cast<uint64_t>(inst.lineno), false)
                    });
                }
            }
            if (inst.op == "label") {
                continue;
            } else if (inst.op == "br") {
                if (inst.operands.size() >= 3) {
                    std::string cname = inst.operands[0].name;
                    std::string tname = inst.operands[1].name;
                    std::string fname = inst.operands[2].name;
                    llvm::Value* cval = getOrLoad(cname);
                    bool cvalWasPointer = cval->getType()->isPointerTy();

                    // Handle different condition types:
                    // - i1: native boolean, use directly
                    // - i32: result of PyObject_CompareBool, convert to i1
                    // - ptr: boxed value — call PyObject_TruthValue for a
                    //   real, type-dispatching truthiness check.
                    //
                    // Severe pre-existing bug, fixed here: this used to
                    // unbox the pointer's raw `.value` int64 field and
                    // compare it to zero directly — correct only for
                    // boxed int/bool (whose `.value` IS the number), but
                    // silently, unconditionally FALSE for every other
                    // boxed type (str/list/dict/...), since their
                    // `.value` field is unused/zero regardless of actual
                    // content. `if s:` for a non-empty string `s`, `if
                    // some_list:` for a non-empty list, `while s:`, and
                    // ternary `x if s else y` were all affected — found
                    // while verifying decimal.Decimal's truthiness this
                    // session (an unrelated, much narrower fix on its
                    // own surfaced this far more general bug). See
                    // IMPLEMENTATION.md.
                    if (cval->getType() != llvm::Type::getInt1Ty(context)) {
                        if (cval->getType()->isIntegerTy() && cval->getType()->getIntegerBitWidth() == 32) {
                            // i32 from PyObject_CompareBool — truncate to i1
                            cval = builder.CreateTrunc(cval, llvm::Type::getInt1Ty(context), "cond.i1");
                        } else if (cval->getType()->isIntegerTy() && cval->getType()->getIntegerBitWidth() == 64) {
                            // i64 from i64const (constant int condition like `if 1:`)
                            // Compare to zero: non-zero is truthy.
                            cval = builder.CreateICmpNE(cval, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), "cond.i1");
                        } else if (cval->getType()->isFloatingPointTy()) {
                            // double from fconst (constant float condition like `if 0.0:`)
                            cval = builder.CreateFCmpUNE(cval, llvm::ConstantFP::get(cval->getType(), 0.0), "cond.i1");
                        } else {
                            llvm::Function* truthFn = module->getFunction("PyObject_TruthValue");
                            llvm::Value* truthy = builder.CreateCall(truthFn, {cval}, "cond.truthy");
                            cval = builder.CreateICmpNE(truthy, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0), "cond.i1");
                        }
                    }

                    // DECREF boxed condition temp after extracting truth value.
                    // Second pre-existing bug found alongside the
                    // truthiness one above: this checked cval's type
                    // *after* the reassignment block above always
                    // rewrites it to i1, so this was unreachable dead
                    // code — the boxed condition temp's ownership was
                    // never released here (a minor, silent refcount leak
                    // on every `if <boxed>:`/`while <boxed>:`/ternary,
                    // not a crash). Fixed by checking the pointer-ness of
                    // the *original* loaded value, captured above.
                    if (cvalWasPointer)
                        emitDecRefIfOwned(cname);

                    auto tit = blockMap.find(tname);
                    auto fit = blockMap.find(fname);
                    if (tit != blockMap.end() && fit != blockMap.end()) {
                        builder.CreateCondBr(cval, tit->second, fit->second);
                    }
                } else if (!inst.result.empty()) {
                    auto it = blockMap.find(inst.result);
                    if (it == blockMap.end()) {
                        blockMap[inst.result] = llvm::BasicBlock::Create(context, inst.result, func);
                        it = blockMap.find(inst.result);
                    }
                    if (!curBlock->getTerminator()) {
                        builder.CreateBr(it->second);
                    }
                }
                continue;
            } else if (inst.op == "try_begin") {
                // operands[0] = jmpVar name
                // operands[1] = normal target label (where setjmp==0)
                // operands[2] = exception target label (where setjmp==1)
                std::string jn = inst.operands.size() > 0 ? inst.operands[0].name : "";
                std::string normalL = inst.operands.size() > 1 ? inst.operands[1].name : "";
                std::string excL    = inst.operands.size() > 2 ? inst.operands[2].name : "";
                llvm::AllocaInst* jbuf = nullptr;
                auto it = jmpBufAllocas.find(jn);
                if (it != jmpBufAllocas.end()) jbuf = it->second;
                llvm::Function* sj = module->getFunction("setjmp");
                llvm::Function* pycTryPush = module->getFunction("pyc_try_push");
                llvm::Value* zero = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                if (jbuf && sj && pycTryPush) {
                    // The jmp_buf is an array of 256 bytes; the setjmp call
                    // wants a pointer to the first byte. Cast the alloca
                    // pointer to i8* (LLVM's setjmp takes i8*).
                    llvm::Value* jbufPtr = builder.CreateBitCast(jbuf, int8PtrTy);
                    // 1) Call setjmp FIRST. setjmp fills the buffer with the
                    //    current register/stack state. The ReturnsTwice
                    //    attribute tells LLVM that the call may return twice
                    //    (once normally, once after longjmp).
                    llvm::Value* rv = builder.CreateCall(sj, {jbufPtr}, "setjmp.rv");
                    // 2) Push the try frame ONLY on first entry (setjmp == 0).
                    //    pyc_raise pops the frame before longjmp'ing, so the
                    //    exception re-entry path must NOT push again — that
                    //    would grow the stack by one dead frame per exception.
                    llvm::Value* isExc = builder.CreateICmpNE(rv,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
                    auto nit = blockMap.find(normalL);
                    auto eit = blockMap.find(excL);
                    if (nit != blockMap.end() && eit != blockMap.end()) {
                        llvm::BasicBlock* pushBB = llvm::BasicBlock::Create(context, "try.push", func);
                        builder.CreateCondBr(isExc, eit->second, pushBB);
                        builder.SetInsertPoint(pushBB);
                        builder.CreateCall(pycTryPush, {jbufPtr, zero});
                        builder.CreateBr(nit->second);
                    } else if (nit != blockMap.end()) {
                        builder.CreateCall(pycTryPush, {jbufPtr, zero});
                        builder.CreateBr(nit->second);
                    }
                } else if (jbuf) {
                    // No setjmp/push available: just branch to normalL.
                    auto nit = blockMap.find(normalL);
                    if (nit != blockMap.end()) builder.CreateBr(nit->second);
                }
                continue;
            } else if (inst.op == "try_end") {
                // operands[0] = jmpVar name.
                // Pop the try frame and branch to the end label. If the
                // current block is already terminated (e.g. by an explicit
                // `return` in the body or handler), do nothing — the function
                // is exiting and the runtime's thread-local cleanup will
                // happen at process exit. In particular, when a longjmp
                // reaches a handler, the handler itself emits a pop
                // (because it always runs), so we must NOT also pop here
                // (that would over-pop into the next outer try or into
                // unrelated code).
                if (!curBlock->getTerminator()) {
                    llvm::Function* pycTryPop = module->getFunction("pyc_try_pop");
                    if (pycTryPop) builder.CreateCall(pycTryPop, {});
                    if (!inst.result.empty()) {
                        auto eit = blockMap.find(inst.result);
                        if (eit != blockMap.end()) builder.CreateBr(eit->second);
                    }
                }
                continue;
            } else if (inst.op == "icmp") {
                // Dispatch to PyObject_CompareBool so both int and float work.
                // op codes: 0=Eq, 1=NotEq, 2=Lt, 3=Gt, 4=LtE, 5=GtE
                std::string opstr = inst.operands.empty() ? "" : inst.operands[0].name;
                std::string lhs = inst.operands.size() > 1 ? inst.operands[1].name : "";
                std::string rhs = inst.operands.size() > 2 ? inst.operands[2].name : "";
                int opcode = 0;
                if      (opstr == "Eq"    || opstr == "eq") opcode = 0;
                else if (opstr == "NotEq" || opstr == "ne") opcode = 1;
                else if (opstr == "Lt"    || opstr == "lt") opcode = 2;
                else if (opstr == "Gt"    || opstr == "gt") opcode = 3;
                else if (opstr == "LtE")                    opcode = 4;
                else if (opstr == "GtE")                    opcode = 5;

                std::string icmpLhsName = inst.operands.size() > 1 ? inst.operands[1].name : "";
                std::string icmpRhsName = inst.operands.size() > 2 ? inst.operands[2].name : "";
                llvm::Value* icmpLhsRaw = icmpLhsName.empty() ? nullptr : getOrLoad(icmpLhsName);
                llvm::Value* icmpRhsRaw = icmpRhsName.empty() ? nullptr : getOrLoad(icmpRhsName);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(context);
                llvm::Type* dblTy = llvm::Type::getDoubleTy(context);
                bool l64 = icmpLhsRaw && icmpLhsRaw->getType() == i64Ty;
                bool r64 = icmpRhsRaw && icmpRhsRaw->getType() == i64Ty;
                bool lf = icmpLhsRaw && icmpLhsRaw->getType() == dblTy;
                bool rf = icmpRhsRaw && icmpRhsRaw->getType() == dblTy;
                llvm::Function* boolNew = module->getFunction("PyBool_New");
                if (boolNew && ((l64 && r64) || (lf && rf))) {
                    llvm::Value* cmpv = nullptr;
                    if (opstr == "Eq" || opstr == "eq") cmpv = l64 ? builder.CreateICmpEQ(icmpLhsRaw, icmpRhsRaw) : builder.CreateFCmpOEQ(icmpLhsRaw, icmpRhsRaw);
                    else if (opstr == "NotEq" || opstr == "ne") cmpv = l64 ? builder.CreateICmpNE(icmpLhsRaw, icmpRhsRaw) : builder.CreateFCmpONE(icmpLhsRaw, icmpRhsRaw);
                    else if (opstr == "Lt" || opstr == "lt") cmpv = l64 ? builder.CreateICmpSLT(icmpLhsRaw, icmpRhsRaw) : builder.CreateFCmpOLT(icmpLhsRaw, icmpRhsRaw);
                    else if (opstr == "Gt" || opstr == "gt") cmpv = l64 ? builder.CreateICmpSGT(icmpLhsRaw, icmpRhsRaw) : builder.CreateFCmpOGT(icmpLhsRaw, icmpRhsRaw);
                    else if (opstr == "LtE") cmpv = l64 ? builder.CreateICmpSLE(icmpLhsRaw, icmpRhsRaw) : builder.CreateFCmpOLE(icmpLhsRaw, icmpRhsRaw);
                    else if (opstr == "GtE") cmpv = l64 ? builder.CreateICmpSGE(icmpLhsRaw, icmpRhsRaw) : builder.CreateFCmpOGE(icmpLhsRaw, icmpRhsRaw);
                    else cmpv = l64 ? builder.CreateICmpNE(icmpLhsRaw, icmpRhsRaw) : builder.CreateFCmpONE(icmpLhsRaw, icmpRhsRaw);
                    // Store the native i1 result directly. This lets the br handler
                    // use it without boxing/unboxing (PyBool_New + load + compare).
                    // When the result is used in a non-branch context (e.g. assigned
                    // to a variable or passed as an argument), getAsPyObject boxes it
                    // lazily via PyBool_New.
                    valueMap[inst.result] = cmpv;
                    // Don't markOwned — i1 is a native value with no refcount.
                    if (!icmpLhsName.empty()) emitDecRefIfOwnedSameBlock(icmpLhsName);
                    if (!icmpRhsName.empty()) emitDecRefIfOwnedSameBlock(icmpRhsName);
                    continue;
                }
                bool icmpLhsNative = icmpLhsRaw && (icmpLhsRaw->getType() == i64Ty || icmpLhsRaw->getType() == dblTy);
                bool icmpRhsNative = icmpRhsRaw && (icmpRhsRaw->getType() == i64Ty || icmpRhsRaw->getType() == dblTy);
                llvm::Value* lhsBox = getAsPyObject(icmpLhsName);
                llvm::Value* rhsBox = getAsPyObject(icmpRhsName);

                llvm::Function* cmpFn  = module->getFunction("PyObject_CompareBool");
                llvm::Value* boxedCmp = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                if (cmpFn && boolNew) {
                    llvm::Value* cmpResult = builder.CreateCall(cmpFn, {
                        lhsBox, rhsBox,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), opcode)
                    });
                    boxedCmp = builder.CreateCall(boolNew, {cmpResult}, inst.result);
                }
                valueMap[inst.result] = boxedCmp;
                markOwned(inst.result);
                if (!icmpLhsName.empty()) emitDecRefIfOwnedSameBlock(icmpLhsName);
                if (!icmpRhsName.empty()) emitDecRefIfOwnedSameBlock(icmpRhsName);
                {
                    llvm::Function* decrefN = module->getFunction("Py_DECREF");
                    if (decrefN) {
                        if (icmpLhsNative) builder.CreateCall(decrefN, {lhsBox});
                        if (icmpRhsNative) builder.CreateCall(decrefN, {rhsBox});
                    }
                }
                continue;
            }
            if (inst.op == "ptricmp") {
                // Pointer identity comparison: compare PyObject* addresses directly.
                // Used for `is` / `is not` operators. No unboxing — compare the
                // actual pointers so that `a = [1,2]; b = a; a is b` returns True.
                // Result is boxed as a bool PyObject* (same as regular icmp).
                std::string opstr = inst.operands.empty() ? "" : inst.operands[0].name;
                // Use getAsPyObject (not getOrLoad) so operands stored as native
                // i64/double (from unboxed numerics) are boxed on demand into
                // PyObject* before the pointer compare.
                std::string lhsName = inst.operands.size() > 1 ? inst.operands[1].name : "";
                std::string rhsName = inst.operands.size() > 2 ? inst.operands[2].name : "";
                llvm::Value* lhs = lhsName.empty() ? llvm::ConstantPointerNull::get(pyObjectPtrTy) : getAsPyObject(lhsName);
                llvm::Value* rhs = rhsName.empty() ? llvm::ConstantPointerNull::get(pyObjectPtrTy) : getAsPyObject(rhsName);
                llvm::Value* cmp = nullptr;
                if      (opstr == "Eq"    || opstr == "eq")     cmp = builder.CreateICmpEQ(lhs, rhs, inst.result);
                else if (opstr == "NotEq" || opstr == "ne")     cmp = builder.CreateICmpNE(lhs, rhs, inst.result);
                else                                             cmp = builder.CreateICmpNE(lhs, rhs, inst.result);
                // Box the i1 result as a PyObject* bool via PyBool_New
                llvm::Function* boolNew = module->getFunction("PyBool_New");
                if (boolNew) {
                    llvm::Value* i32cmp = builder.CreateZExt(cmp, llvm::Type::getInt32Ty(context), inst.result + ".i32");
                    llvm::Value* boxed = builder.CreateCall(boolNew, {i32cmp}, inst.result);
                    valueMap[inst.result] = boxed;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            }
            if (inst.op == "i64const") {
                std::string val = inst.operands.empty() ? "0" : inst.operands[0].name;
                char* end = nullptr;
                errno = 0;
                long v = std::strtol(val.c_str(), &end, 10);
                (void)end; (void)errno;
                valueMap[inst.result] = llvm::ConstantInt::get(context, llvm::APInt(64, v));
            } else if (inst.op == "i64_from_box") {
                const std::string& boxName = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* boxed = getOrLoad(boxName);
                valueMap[inst.result] = unboxToI64(boxed);
                emitDecRefIfOwned(boxName);  // free the PyObject now that i64 is extracted
            } else if (inst.op == "box_i64") {
                llvm::Value* val = getOrLoad(inst.operands.empty() ? "" : inst.operands[0].name);
                valueMap[inst.result] = boxI64(val, inst.result);
                markOwned(inst.result);
            } else if (inst.op == "box_f64") {
                llvm::Value* val = getOrLoad(inst.operands.empty() ? "" : inst.operands[0].name);
                valueMap[inst.result] = boxDouble(val, inst.result);
                markOwned(inst.result);
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
                std::string srcName = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* newVal = getOrLoad(srcName);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(context);
                if (newVal->getType() != i64Ty) {
                    newVal = unboxToI64(newVal);
                    emitDecRefIfOwned(srcName);
                }
                auto it = valueMap.find(inst.result);
                // Use an i64 alloca for names proven as native numeric locals (A2.1)
                // (generalizes previous range-loop only usage). Assign/use paths
                // transition back to PyObject* slot if a later boxed value is assigned.
                llvm::AllocaInst* i64alloca = nullptr;
                if (it != valueMap.end()) {
                    if (auto* existing = llvm::dyn_cast<llvm::AllocaInst>(it->second)) {
                        if (existing->getAllocatedType() == i64Ty) {
                            i64alloca = existing;
                        }
                    }
                }
                if (!i64alloca) {
                    llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                                   func->getEntryBlock().begin());
                    i64alloca = entryBuilder.CreateAlloca(i64Ty, nullptr, inst.result + ".i64");
                    // Debug info: track this native int variable.
                    emitDbgDeclare(i64alloca, inst.result, diIntDI);
                }
                valueMap[inst.result] = i64alloca;
                builder.CreateStore(newVal, i64alloca);
            } else if (inst.op == "bytesconst") {
                // b"..." literal. Passes an explicit length (from the
                // in-memory operand std::string) to
                // PyBytes_FromStringAndSize so embedded \x00 bytes
                // survive. CreateGlobalStringPtr emits the full bytes
                // (LLVM uses size) plus a terminator; pass s.size().
                const std::string& s = inst.operands.empty() ? std::string() : inst.operands[0].name;
                llvm::Function* fromBytes = module->getFunction("PyBytes_FromStringAndSize");
                if (fromBytes) {
                    llvm::Value* ptr = builder.CreateGlobalStringPtr(s, "bytes");
                    llvm::Value* lenConst = llvm::ConstantInt::get(context, llvm::APInt(64, (uint64_t)s.size()));
                    llvm::Value* boxed = builder.CreateCall(fromBytes, {ptr, lenConst}, inst.result);
                    valueMap[inst.result] = boxed;
                    markOwned(inst.result);
                }
            } else if (inst.op == "const") {
                std::string val = inst.operands.empty() ? "0" : inst.operands[0].name;
                if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
                    // Length-explicit like bytesconst: FromString
                    // truncates at the first NUL via strlen.
                    // CreateGlobalStringPtr already emits a terminator;
                    // pass s.size(), not strlen.
                    llvm::Function* fromStr = module->getFunction("PyUnicode_FromStringAndSize");
                    if (fromStr) {
                        std::string s = val.substr(1, val.size() - 2);
                        llvm::Value* strConst = builder.CreateGlobalStringPtr(s, "str");
                        llvm::Value* lenConst = llvm::ConstantInt::get(context, llvm::APInt(64, (uint64_t)s.size()));
                        llvm::Value* boxed = builder.CreateCall(fromStr, {strConst, lenConst}, inst.result);
                        valueMap[inst.result] = boxed;
                        markOwned(inst.result);
                    }
                } else {
                    // Use strtol rather than std::stol: the latter throws
                    // std::out_of_range on values outside the long range, and
                    // we want to be lenient for tests like 2**64 that may be
                    // // used as int literals before being passed to Python.
                    std::string val = inst.operands.empty() ? "0" : inst.operands[0].name;
                    char* end = nullptr;
                    errno = 0;
                    long v = std::strtol(val.c_str(), &end, 10);
                    (void)end; (void)errno;
                    llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
                    if (fromLong) {
                        llvm::Value* boxed = builder.CreateCall(fromLong,
                            {llvm::ConstantInt::get(context, llvm::APInt(64, v))}, inst.result);
                        valueMap[inst.result] = boxed;
                        markOwned(inst.result);
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
                    markOwned(inst.result);
                }
            } else if (inst.op == "nconst") {
                // CPython None is the singleton null PyObject*. Emit a real
                // null pointer so `None is None` and `x is None` work via
                // pointer identity (ptricmp) and so type(None)/PyStr_FromAny
                // return the proper "None" via the runtime's null path.
                valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                // Do NOT markOwned: the null constant is not a heap allocation,
                // so it must not be Py_DECREF'd at the end of its scope.
            } else if (inst.op == "fconst") {
                // Use strtod rather than std::stod: the latter throws
                // std::out_of_range on denormal/subnormal float literals
                // (e.g. 1e-308) that C strtod accepts by mapping to 0.
                std::string val = inst.operands.empty() ? "0" : inst.operands[0].name;
                char* end = nullptr;
                errno = 0;
                double v = std::strtod(val.c_str(), &end);
                (void)end; (void)errno;
                valueMap[inst.result] = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), v);
            } else if (inst.op == "add") {
                if (emitNativeNumericBinary(inst, "add")) continue;
                llvm::Function* numberAdd = module->getFunction("PyNumber_Add");
                std::string lhsNameA = inst.operands.size() > 0 ? inst.operands[0].name : "";
                std::string rhsNameA = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRawA = lhsNameA.empty() ? nullptr : getOrLoad(lhsNameA);
                llvm::Value* rhsRawA = rhsNameA.empty() ? nullptr : getOrLoad(rhsNameA);
                bool lhsNativeA = lhsRawA && (lhsRawA->getType() == llvm::Type::getInt64Ty(context) || lhsRawA->getType()->isDoubleTy());
                bool rhsNativeA = rhsRawA && (rhsRawA->getType() == llvm::Type::getInt64Ty(context) || rhsRawA->getType()->isDoubleTy());
                llvm::Value* lhs = getAsPyObject(lhsNameA);
                llvm::Value* rhs = getAsPyObject(rhsNameA);
                if (numberAdd) {
                    llvm::Value* sum = builder.CreateCall(numberAdd, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = sum;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
                emitDecRefIfOwned(lhsNameA);
                emitDecRefIfOwned(rhsNameA);
                {
                    llvm::Function* decrefN = module->getFunction("Py_DECREF");
                    if (decrefN) {
                        if (lhsNativeA) builder.CreateCall(decrefN, {lhs});
                        if (rhsNativeA) builder.CreateCall(decrefN, {rhs});
                    }
                }
            } else if (inst.op == "sub") {
                if (emitNativeNumericBinary(inst, "sub")) continue;
                llvm::Function* numberSub = module->getFunction("PyNumber_Subtract");
                std::string lhsNameS = inst.operands.size() > 0 ? inst.operands[0].name : "";
                std::string rhsNameS = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRawS = lhsNameS.empty() ? nullptr : getOrLoad(lhsNameS);
                llvm::Value* rhsRawS = rhsNameS.empty() ? nullptr : getOrLoad(rhsNameS);
                bool lhsNativeS = lhsRawS && (lhsRawS->getType() == llvm::Type::getInt64Ty(context) || lhsRawS->getType()->isDoubleTy());
                bool rhsNativeS = rhsRawS && (rhsRawS->getType() == llvm::Type::getInt64Ty(context) || rhsRawS->getType()->isDoubleTy());
                llvm::Value* lhs = getAsPyObject(lhsNameS);
                llvm::Value* rhs = getAsPyObject(rhsNameS);
                if (numberSub) {
                    llvm::Value* diff = builder.CreateCall(numberSub, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = diff;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
                emitDecRefIfOwned(lhsNameS);
                emitDecRefIfOwned(rhsNameS);
                {
                    llvm::Function* decrefN = module->getFunction("Py_DECREF");
                    if (decrefN) {
                        if (lhsNativeS) builder.CreateCall(decrefN, {lhs});
                        if (rhsNativeS) builder.CreateCall(decrefN, {rhs});
                    }
                }
            } else if (inst.op == "div") {
                std::string lname = inst.operands.empty() ? "" : inst.operands[0].name;
                std::string rname = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRaw = lname.empty() ? nullptr : getOrLoad(lname);
                llvm::Value* rhsRaw = rname.empty() ? nullptr : getOrLoad(rname);
                bool bothNativeInt = (lhsRaw && lhsRaw->getType() == llvm::Type::getInt64Ty(context)) &&
                                     (rhsRaw && rhsRaw->getType() == llvm::Type::getInt64Ty(context));
                // See emitNativeNumericBinary above: don't trust a stale
                // resultType=="int" when an operand actually resolved to a
                // native double (crash risk in unboxToI64/CreateIsNull).
                bool eitherRawDoubleDiv = (lhsRaw && lhsRaw->getType()->isDoubleTy())
                                        || (rhsRaw && rhsRaw->getType()->isDoubleTy());

                if ((inst.resultType == "int" && !eitherRawDoubleDiv) || bothNativeInt) {
                    llvm::Value* lhs = unboxToI64(getOrLoad(lname));
                    llvm::Value* rhs = unboxToI64(getOrLoad(rname));
                    llvm::Value* isZero = builder.CreateICmpEQ(rhs, llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Function* numberDiv = module->getFunction("PyNumber_Divide");
                    llvm::Value* boxedL = getAsPyObject(lname);
                    llvm::Value* boxedR = getAsPyObject(rname);
                    llvm::BasicBlock* zeroBlk = llvm::BasicBlock::Create(context, "div.zero", func);
                    llvm::BasicBlock* nzBlk = llvm::BasicBlock::Create(context, "div.nz", func);
                    llvm::BasicBlock* mergeBlk = llvm::BasicBlock::Create(context, "div.merge", func);
                    builder.CreateCondBr(isZero, zeroBlk, nzBlk);
                    builder.SetInsertPoint(nzBlk);
                    llvm::Value* q = builder.CreateSDiv(lhs, rhs);
                    llvm::Value* r = builder.CreateSRem(lhs, rhs);
                    llvm::Value* signsDiffer = builder.CreateICmpSLT(builder.CreateXor(lhs, rhs), llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Value* hasRem = builder.CreateICmpNE(r, llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Value* needAdjust = builder.CreateAnd(signsDiffer, hasRem);
                    llvm::Value* one = llvm::ConstantInt::get(context, llvm::APInt(64, 1));
                    llvm::Value* qAdj = builder.CreateSub(q, one);
                    q = builder.CreateSelect(needAdjust, qAdj, q);
                    llvm::Value* boxedI64 = boxI64(q, inst.result + ".i64");
                    builder.CreateBr(mergeBlk);
                    builder.SetInsertPoint(zeroBlk);
                    llvm::Value* quot = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                    if (numberDiv) {
                        quot = builder.CreateCall(numberDiv, {boxedL, boxedR}, inst.result + ".boxed");
                    }
                    builder.CreateBr(mergeBlk);
                    builder.SetInsertPoint(mergeBlk);
                    curBlock = mergeBlk;
                    llvm::PHINode* phi = builder.CreatePHI(pyObjectPtrTy, 2, inst.result);
                    phi->addIncoming(boxedI64, nzBlk);
                    phi->addIncoming(quot, zeroBlk);
                    valueMap[inst.result] = phi;
                    markOwned(inst.result);
                    emitDecRefIfOwned(lname);
                    emitDecRefIfOwned(rname);
                    continue;
                }
                // float or unknown -> boxed
                llvm::Function* numberDiv = module->getFunction("PyNumber_Divide");
                std::string lhsNameD = inst.operands.size() > 0 ? inst.operands[0].name : "";
                std::string rhsNameD = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRawD = lhsNameD.empty() ? nullptr : getOrLoad(lhsNameD);
                llvm::Value* rhsRawD = rhsNameD.empty() ? nullptr : getOrLoad(rhsNameD);
                bool lhsNativeD = lhsRawD && (lhsRawD->getType() == llvm::Type::getInt64Ty(context) || lhsRawD->getType()->isDoubleTy());
                bool rhsNativeD = rhsRawD && (rhsRawD->getType() == llvm::Type::getInt64Ty(context) || rhsRawD->getType()->isDoubleTy());
                llvm::Value* lhs = getAsPyObject(lhsNameD);
                llvm::Value* rhs = getAsPyObject(rhsNameD);
                if (numberDiv) {
                    llvm::Value* quot = builder.CreateCall(numberDiv, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = quot;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
                emitDecRefIfOwned(lhsNameD);
                emitDecRefIfOwned(rhsNameD);
                {
                    llvm::Function* decrefN = module->getFunction("Py_DECREF");
                    if (decrefN) {
                        if (lhsNativeD) builder.CreateCall(decrefN, {lhs});
                        if (rhsNativeD) builder.CreateCall(decrefN, {rhs});
                    }
                }
             } else if (inst.op == "pow") {
                 // B16: Complex pow — if both operands are complex (boxed), use PyComplex_Pow
                 std::string lhsName = inst.operands.size() > 0 ? inst.operands[0].name : "";
                 std::string rhsName = inst.operands.size() > 1 ? inst.operands[1].name : "";
                 // Check if this is a complex pow by checking if the call was emitted as PyComplex_Pow
                 // (The compiler emits "call" with funcName="PyComplex_Pow" for complex pow)
                 // For the "pow" IR instruction, we need to check if operands are complex
                 // Since complex values are always boxed, we check resultType and operand types
                 // For now, handle via the boxed path if resultType is "boxed" and we detect complex
                 // Actually, the compiler emits a "call" instruction for complex pow, not "pow"
                 // So this "pow" instruction is only for native/boxed numeric pow
                 llvm::Value* lhsRaw = lhsName.empty() ? nullptr : getOrLoad(lhsName);
                 llvm::Value* rhsRaw = rhsName.empty() ? nullptr : getOrLoad(rhsName);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(context);
                llvm::Type* dblTy = llvm::Type::getDoubleTy(context);
                bool lhsNativeI64 = lhsRaw && lhsRaw->getType() == i64Ty;
                bool rhsNativeI64 = rhsRaw && rhsRaw->getType() == i64Ty;
                bool lhsNativeDbl = lhsRaw && lhsRaw->getType() == dblTy;
                bool rhsNativeDbl = rhsRaw && rhsRaw->getType() == dblTy;
                bool bothNativeI64 = lhsNativeI64 && rhsNativeI64;
                bool bothNative = (lhsNativeI64 || lhsNativeDbl) && (rhsNativeI64 || rhsNativeDbl);
                // Native path: both operands are numeric (i64 or double)
                if (bothNativeI64 && inst.resultType != "float") {
                    // Integer power: exp >= 0 yields int, exp < 0 yields float
                    llvm::Function* powInt64Obj = module->getFunction("Pyc_PowInt64Obj");
                    valueMap[inst.result] = builder.CreateCall(powInt64Obj, {lhsRaw, rhsRaw}, inst.result);
                    markOwned(inst.result);
                } else if (bothNative || inst.resultType == "float") {
                    // Float power: LLVM pow on doubles; keep native when result is float
                    // so mag = r ** -1.5 feeds native m1*mag / dt*mag chains.
                    llvm::Value* lhsUnboxed = unboxToDouble(lhsRaw ? lhsRaw : getOrLoad(lhsName));
                    llvm::Value* rhsUnboxed = unboxToDouble(rhsRaw ? rhsRaw : getOrLoad(rhsName));
                    llvm::Value* powResult = builder.CreateBinaryIntrinsic(
                        llvm::Intrinsic::pow, lhsUnboxed, rhsUnboxed, nullptr, "pow.result");
                    if (inst.resultType == "float") {
                        valueMap[inst.result] = powResult;
                    } else {
                        valueMap[inst.result] = boxDouble(powResult, inst.result);
                        markOwned(inst.result);
                    }
                    if (!lhsNativeDbl && !lhsNativeI64) emitDecRefIfOwned(lhsName);
                    if (!rhsNativeDbl && !rhsNativeI64) emitDecRefIfOwned(rhsName);
                } else {
                    // Boxed path: call Pyc_Pow
                    llvm::Function* fn = module->getFunction("Pyc_Pow");
                    llvm::Value* lhs = getAsPyObject(lhsName);
                    llvm::Value* rhs = getAsPyObject(rhsName);
                    bool lhsWasNative = lhsRaw && (lhsRaw->getType() == i64Ty || lhsRaw->getType()->isDoubleTy());
                    bool rhsWasNative = rhsRaw && (rhsRaw->getType() == i64Ty || rhsRaw->getType()->isDoubleTy());
                    if (fn) {
                        valueMap[inst.result] = builder.CreateCall(fn, {lhs, rhs}, inst.result);
                        markOwned(inst.result);
                    } else {
                        valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                    }
                    emitDecRefIfOwned(lhsName);
                    emitDecRefIfOwned(rhsName);
                    {
                        llvm::Function* decref = module->getFunction("Py_DECREF");
                        if (decref) {
                            if (lhsWasNative) builder.CreateCall(decref, {lhs});
                            if (rhsWasNative) builder.CreateCall(decref, {rhs});
                        }
                    }
                }
            } else if (inst.op == "truediv") {
                llvm::Function* numberTrueDiv = module->getFunction("PyNumber_TrueDivide");
                std::string lhsNameT = inst.operands.size() > 0 ? inst.operands[0].name : "";
                std::string rhsNameT = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRawT = lhsNameT.empty() ? nullptr : getOrLoad(lhsNameT);
                llvm::Value* rhsRawT = rhsNameT.empty() ? nullptr : getOrLoad(rhsNameT);
                bool lhsNativeT = lhsRawT && (lhsRawT->getType() == llvm::Type::getInt64Ty(context) || lhsRawT->getType()->isDoubleTy());
                bool rhsNativeT = rhsRawT && (rhsRawT->getType() == llvm::Type::getInt64Ty(context) || rhsRawT->getType()->isDoubleTy());
                llvm::Value* lhs = getAsPyObject(lhsNameT);
                llvm::Value* rhs = getAsPyObject(rhsNameT);
                if (numberTrueDiv) {
                    llvm::Value* quot = builder.CreateCall(numberTrueDiv, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = quot;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
                emitDecRefIfOwned(lhsNameT);
                emitDecRefIfOwned(rhsNameT);
                {
                    llvm::Function* decrefN = module->getFunction("Py_DECREF");
                    if (decrefN) {
                        if (lhsNativeT) builder.CreateCall(decrefN, {lhs});
                        if (rhsNativeT) builder.CreateCall(decrefN, {rhs});
                    }
                }
            } else if (inst.op == "mod") {
                std::string lname = inst.operands.empty() ? "" : inst.operands[0].name;
                std::string rname = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRawM = lname.empty() ? nullptr : getOrLoad(lname);
                llvm::Value* rhsRawM = rname.empty() ? nullptr : getOrLoad(rname);
                bool bothNativeIntM = (lhsRawM && lhsRawM->getType() == llvm::Type::getInt64Ty(context)) &&
                                      (rhsRawM && rhsRawM->getType() == llvm::Type::getInt64Ty(context));
                // See emitNativeNumericBinary above: don't trust a stale
                // resultType=="int" when an operand actually resolved to a
                // native double (crash risk in unboxToI64/CreateIsNull).
                bool eitherRawDoubleMod = (lhsRawM && lhsRawM->getType()->isDoubleTy())
                                        || (rhsRawM && rhsRawM->getType()->isDoubleTy());

                if ((inst.resultType == "int" && !eitherRawDoubleMod) || bothNativeIntM) {
                    llvm::Value* lhs = unboxToI64(getOrLoad(lname));
                    llvm::Value* rhs = unboxToI64(getOrLoad(rname));
                    llvm::Value* isZero = builder.CreateICmpEQ(rhs, llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Function* numberRem = module->getFunction("PyNumber_Remainder");
                    llvm::Value* boxedL = getAsPyObject(lname);
                    llvm::Value* boxedR = getAsPyObject(rname);
                    llvm::BasicBlock* zeroBlk = llvm::BasicBlock::Create(context, "mod.zero", func);
                    llvm::BasicBlock* nzBlk = llvm::BasicBlock::Create(context, "mod.nz", func);
                    llvm::BasicBlock* mergeBlk = llvm::BasicBlock::Create(context, "mod.merge", func);
                    builder.CreateCondBr(isZero, zeroBlk, nzBlk);
                    builder.SetInsertPoint(nzBlk);
                    llvm::Value* r = builder.CreateSRem(lhs, rhs);
                    llvm::Value* rnz = builder.CreateICmpNE(r, llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Value* signD = builder.CreateICmpSLT(builder.CreateXor(r, rhs), llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    llvm::Value* need = builder.CreateAnd(rnz, signD);
                    llvm::Value* rAdj = builder.CreateAdd(r, rhs);
                    r = builder.CreateSelect(need, rAdj, r);
                    llvm::Value* boxedI64 = boxI64(r, inst.result + ".i64");
                    builder.CreateBr(mergeBlk);
                    builder.SetInsertPoint(zeroBlk);
                    llvm::Value* rem = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                    if (numberRem) {
                        rem = builder.CreateCall(numberRem, {boxedL, boxedR}, inst.result + ".boxed");
                    }
                    builder.CreateBr(mergeBlk);
                    builder.SetInsertPoint(mergeBlk);
                    curBlock = mergeBlk;
                    llvm::PHINode* phi = builder.CreatePHI(pyObjectPtrTy, 2, inst.result);
                    phi->addIncoming(boxedI64, nzBlk);
                    phi->addIncoming(rem, zeroBlk);
                    valueMap[inst.result] = phi;
                    markOwned(inst.result);
                    emitDecRefIfOwned(lname);
                    emitDecRefIfOwned(rname);
                    continue;
                }
                // float or unknown -> boxed
                llvm::Function* numberRem = module->getFunction("PyNumber_Remainder");
                llvm::Value* lhsBoxed = getAsPyObject(lname);
                llvm::Value* rhsBoxed = getAsPyObject(rname);
                bool lhsNativeBoxed = lhsRawM && (lhsRawM->getType() == llvm::Type::getInt64Ty(context) || lhsRawM->getType()->isDoubleTy());
                bool rhsNativeBoxed = rhsRawM && (rhsRawM->getType() == llvm::Type::getInt64Ty(context) || rhsRawM->getType()->isDoubleTy());
                if (numberRem) {
                    llvm::Value* rem = builder.CreateCall(numberRem, {lhsBoxed, rhsBoxed}, inst.result);
                    valueMap[inst.result] = rem;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
                emitDecRefIfOwned(lname);
                emitDecRefIfOwned(rname);
                {
                    llvm::Function* decrefN = module->getFunction("Py_DECREF");
                    if (decrefN) {
                        if (lhsNativeBoxed) builder.CreateCall(decrefN, {lhsBoxed});
                        if (rhsNativeBoxed) builder.CreateCall(decrefN, {rhsBoxed});
                    }
                }
            } else if (inst.op == "mul") {
                if (emitNativeNumericBinary(inst, "mul")) continue;
                llvm::Function* numberMult = module->getFunction("PyNumber_Multiply");
                std::string lhsNameMu = inst.operands.size() > 0 ? inst.operands[0].name : "";
                std::string rhsNameMu = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRawMu = lhsNameMu.empty() ? nullptr : getOrLoad(lhsNameMu);
                llvm::Value* rhsRawMu = rhsNameMu.empty() ? nullptr : getOrLoad(rhsNameMu);
                bool lhsNativeMu = lhsRawMu && (lhsRawMu->getType() == llvm::Type::getInt64Ty(context) || lhsRawMu->getType()->isDoubleTy());
                bool rhsNativeMu = rhsRawMu && (rhsRawMu->getType() == llvm::Type::getInt64Ty(context) || rhsRawMu->getType()->isDoubleTy());
                llvm::Value* lhs = getAsPyObject(lhsNameMu);
                llvm::Value* rhs = getAsPyObject(rhsNameMu);
                if (numberMult) {
                    llvm::Value* prod = builder.CreateCall(numberMult, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = prod;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
                emitDecRefIfOwned(lhsNameMu);
                emitDecRefIfOwned(rhsNameMu);
                {
                    llvm::Function* decrefN = module->getFunction("Py_DECREF");
                    if (decrefN) {
                        if (lhsNativeMu) builder.CreateCall(decrefN, {lhs});
                        if (rhsNativeMu) builder.CreateCall(decrefN, {rhs});
                    }
                }
            } else if (inst.op == "lshift" || inst.op == "rshift" ||
                       inst.op == "bitor"  || inst.op == "bitand" ||
                       inst.op == "bitxor") {
                // Bitwise operations: try native i64 path first, then
                // fall back to a runtime helper that unboxes + shifts.
                if (emitNativeNumericBinary(inst, inst.op)) continue;
                const char* fnName = nullptr;
                if      (inst.op == "lshift") fnName = "PyNumber_Lshift";
                else if (inst.op == "rshift") fnName = "PyNumber_Rshift";
                else if (inst.op == "bitor")  fnName = "PyNumber_BitOr";
                else if (inst.op == "bitand") fnName = "PyNumber_BitAnd";
                else if (inst.op == "bitxor") fnName = "PyNumber_BitXor";
                llvm::Function* fn = module->getFunction(fnName);
                std::string lhsNameB = inst.operands.size() > 0 ? inst.operands[0].name : "";
                std::string rhsNameB = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRawB = lhsNameB.empty() ? nullptr : getOrLoad(lhsNameB);
                llvm::Value* rhsRawB = rhsNameB.empty() ? nullptr : getOrLoad(rhsNameB);
                bool lhsNativeB = lhsRawB && (lhsRawB->getType() == llvm::Type::getInt64Ty(context) || lhsRawB->getType()->isDoubleTy());
                bool rhsNativeB = rhsRawB && (rhsRawB->getType() == llvm::Type::getInt64Ty(context) || rhsRawB->getType()->isDoubleTy());
                llvm::Value* lhs = getAsPyObject(lhsNameB);
                llvm::Value* rhs = getAsPyObject(rhsNameB);
                if (fn) {
                    llvm::Value* resB = builder.CreateCall(fn, {lhs, rhs}, inst.result);
                    valueMap[inst.result] = resB;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
                emitDecRefIfOwned(lhsNameB);
                emitDecRefIfOwned(rhsNameB);
                llvm::Function* decrefB = module->getFunction("Py_DECREF");
                if (decrefB) {
                    if (lhsNativeB) builder.CreateCall(decrefB, {lhs});
                    if (rhsNativeB) builder.CreateCall(decrefB, {rhs});
                }
            } else if (inst.op == "neg") {
                // Unary minus. For proven numeric resultType, keep native result (i64/double)
                // so it can participate in further unboxed arithmetic (A3 widening).
                // Box only on escape via getAsPyObject.
                std::string opName = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* rawOp = opName.empty() ? nullptr : getOrLoad(opName);
                bool opIsNativeI64 = rawOp && rawOp->getType() == llvm::Type::getInt64Ty(context);
                bool opIsNativeDouble = rawOp && rawOp->getType()->isDoubleTy();
                
                if (inst.resultType == "int" || opIsNativeI64) {
                    llvm::Value* v = unboxToI64(getOrLoad(opName));
                    llvm::Value* n = builder.CreateNeg(v, inst.result + ".i64");
                    valueMap[inst.result] = n;  // native i64 for longer unboxed life
                    if (!opIsNativeI64) emitDecRefIfOwned(opName);
                    continue;
                }
                if (inst.resultType == "float" || opIsNativeDouble) {
                    llvm::Value* v = unboxToDouble(getOrLoad(opName));
                    llvm::Value* n = builder.CreateFNeg(v, inst.result + ".double");
                    valueMap[inst.result] = n;
                    if (!opIsNativeDouble) emitDecRefIfOwned(opName);
                    continue;
                }
                // Fallback: boxed runtime path, but check if operand is native first.
                std::string negArg = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* negArgRaw = negArg.empty() ? nullptr : getOrLoad(negArg);
                bool negArgIsNativeI64 = negArgRaw && negArgRaw->getType() == llvm::Type::getInt64Ty(context);
                bool negArgIsNativeDouble = negArgRaw && negArgRaw->getType()->isDoubleTy();
                
                if (negArgIsNativeI64) {
                    // Native i64 path: negate directly without boxing.
                    llvm::Value* v = builder.CreateNeg(negArgRaw, inst.result + ".i64");
                    valueMap[inst.result] = v;
                    continue;
                }
                if (negArgIsNativeDouble) {
                    // Native double path: negate directly without boxing.
                    llvm::Value* v = builder.CreateFNeg(negArgRaw, inst.result + ".double");
                    valueMap[inst.result] = v;
                    continue;
                }
                
                // Truly boxed path: call runtime.
                llvm::Function* fn = module->getFunction("PyNumber_Negate");
                llvm::Value* arg = negArg.empty() ? llvm::ConstantPointerNull::get(pyObjectPtrTy) : getAsPyObject(negArg);
                if (fn) {
                    valueMap[inst.result] = builder.CreateCall(fn, {arg}, inst.result);
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
                emitDecRefIfOwned(negArg);
            } else if (inst.op == "getattr") {
                llvm::Function* getAttr = module->getFunction("PyObject_GetAttr");
                llvm::Value* obj = getOrLoad(inst.operands[0].name);
                if (getAttr) {
                    llvm::Value* attrName = builder.CreateGlobalStringPtr(inst.operands[1].name, "attr");
                    llvm::Value* result = builder.CreateCall(getAttr, {obj, attrName}, inst.result);
                    valueMap[inst.result] = result;
                    markOwned(inst.result);
                } else {
                    valueMap[inst.result] = llvm::ConstantPointerNull::get(pyObjectPtrTy);
                }
            } else if (inst.op == "subscript") {
                // Handle list indexing: list[index]
                // Use PyList_GetItemObj (not PyList_GetItem) because it:
                // 1) Accepts a PyObject* index (handles negative indices correctly)
                // 2) Returns a new reference (caller owns it, needs DECREF)
                llvm::Function* listGetItemObj = module->getFunction("PyList_GetItemObj");
                if (listGetItemObj) {
                    llvm::Value* list = getOrLoad(inst.operands[0].name);
                    // Box the index as PyObject* so PyList_GetItemObj can handle
                    // negative indices (index < 0 => index += len(list)).
                    llvm::Value* index = getAsPyObject(inst.operands[1].name);
                    llvm::Value* item = builder.CreateCall(listGetItemObj, {list, index}, inst.result);
                    valueMap[inst.result] = item;
                    markOwned(inst.result);
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
            } else if (inst.op == "f64assign") {
                // Dedicated native float store — always f64 alloca, never PyObject*.
                std::string srcName = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* src = getOrLoad(srcName);
                llvm::Value* dsrc = src && src->getType()->isDoubleTy() ? src : unboxToDouble(src);
                if (src && !src->getType()->isDoubleTy() && ownedTemps.count(srcName))
                    emitDecRefIfOwned(srcName);
                llvm::AllocaInst* f64a = nullptr;
                auto tit = valueMap.find(inst.result);
                if (tit != valueMap.end()) {
                    if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(tit->second)) {
                        if (a->getAllocatedType()->isDoubleTy()) f64a = a;
                        else if (a->getAllocatedType() == pyObjectPtrTy) {
                            // Demote PyObject* slot: DECREF current, keep null for safety
                            llvm::Value* oldV = builder.CreateLoad(pyObjectPtrTy, a, inst.result + ".old");
                            if (auto* d = module->getFunction("Py_DECREF")) builder.CreateCall(d, {oldV});
                            builder.CreateStore(llvm::ConstantPointerNull::get(pyObjectPtrTy), a);
                        }
                    }
                }
                if (!f64a) {
                    llvm::IRBuilder<> eb(&func->getEntryBlock(), func->getEntryBlock().begin());
                    f64a = eb.CreateAlloca(llvm::Type::getDoubleTy(context), nullptr, inst.result + ".f64");
                    // Debug info: track this native float variable.
                    emitDbgDeclare(f64a, inst.result, diFloatDI);
                    valueMap[inst.result] = f64a;
                }
                builder.CreateStore(dsrc, f64a);
                continue;
            } else if (inst.op == "assign") {
                // If the source is a Python name AND it's a module global, load
                // the global directly (don't go through valueMap, which may have
                // been polluted by a later 'const' instruction with the same name).
                std::string srcNameAssign = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* src = nullptr;
                if (!srcNameAssign.empty()) {
                    std::string globalName = "pyc_global_" + srcNameAssign;
                    if (module->getNamedGlobal(globalName)) {
                        llvm::GlobalVariable* gvsrc = module->getNamedGlobal(globalName);
                        src = builder.CreateLoad(pyObjectPtrTy, gvsrc, srcNameAssign + ".gload");
                    } else {
                        src = getOrLoad(srcNameAssign);
                    }
                } else {
                    src = getOrLoad(srcNameAssign);
                }
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(context);
                std::string srcName = srcNameAssign;
                // A module-level global must always end up boxed in its pyc_global_*
                // slot, never diverted into a function-local native i64/f64 alloca —
                // otherwise other functions that read the global (e.g. via a call
                // argument) see it as permanently null. Check this once up front and
                // use it to gate the native-local fast paths below.
                bool isModuleGlobal = module->getNamedGlobal("pyc_global_" + inst.result) != nullptr;
                // If RHS is already a native double (e.g. dt*pow chain), store as f64 local
                // even when lowering typed the temp "boxed" (param types known only post-pass).
                bool forceF64 = !isModuleGlobal && ((inst.resultType == "float") ||
                    (src && src->getType()->isDoubleTy() &&
                     !inst.result.empty() && inst.result[0] != 't' && inst.result[0] != 'c' &&
                     inst.result.rfind("__", 0) != 0));

                 // If target currently has an i64 slot (A2.1 numeric local or range var), handle separately.
                 auto tit0 = valueMap.find(inst.result);
                 if (tit0 != valueMap.end()) {
                     if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(tit0->second)) {
                         if (alloca->getAllocatedType() == i64Ty) {
                             if (src->getType() == i64Ty) {
                                 builder.CreateStore(src, alloca);
                             } else {
                                 llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                                               func->getEntryBlock().begin());
                                  llvm::AllocaInst* newAlloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, inst.result + ".slot");
                                  // Debug info: track this local variable.
                                  emitDbgDeclare(newAlloca, inst.result, diPyObjPtrDI);
                                  llvm::Value* toStore = src;
                                 if (toStore->getType() == i64Ty) toStore = boxI64(toStore);
                                 builder.CreateStore(toStore, newAlloca);
                                 valueMap[inst.result] = newAlloca;
                                 ownedSlots.insert(inst.result);
                             }
                             continue;
                         }
                     }
                 }

                  // A5: If target is a numeric local (proven to stay numeric), use native i64 storage.
                  bool isNumericLocal = false;
                  if (!isModuleGlobal) {
                      for (const auto& nl : f.numericLocals) {
                          if (nl == inst.result) { isNumericLocal = true; break; }
                      }
                  }
                 if (isNumericLocal && src->getType() == i64Ty) {
                     // Check if target already has an i64 alloca (from a prior i64assign or numeric local setup)
                     bool hasI64Alloca = false;
                     if (tit0 != valueMap.end()) {
                         if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(tit0->second)) {
                             if (alloca->getAllocatedType() == i64Ty) hasI64Alloca = true;
                         }
                     }
                     if (!hasI64Alloca) {
                         // Create a new i64 alloca in the entry block.
                         llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                                        func->getEntryBlock().begin());
                          llvm::AllocaInst* i64alloca = entryBuilder.CreateAlloca(i64Ty, nullptr, inst.result + ".i64");
                          // Debug info: track this native int variable.
                          emitDbgDeclare(i64alloca, inst.result, diIntDI);
                          valueMap[inst.result] = i64alloca;
                     }
                     // Store the native i64 value.
                     auto tit2 = valueMap.find(inst.result);
                     if (tit2 != valueMap.end()) {
                         if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(tit2->second)) {
                             if (alloca->getAllocatedType() == i64Ty) {
                                 builder.CreateStore(src, alloca);
                                 continue;
                             }
                         }
                     }
                  }

                   // A6 / f64assign: native f64 storage for proven float locals
                   bool isFloatLocal = forceF64;
                   if (!isFloatLocal && !isModuleGlobal) {
                       for (const auto& nfl : f.numericFloatLocals) {
                           if (nfl == inst.result) { isFloatLocal = true; break; }
                       }
                   }
                   if (isFloatLocal) {
                       llvm::Value* dsrc = src;
                       if (!dsrc->getType()->isDoubleTy()) {
                           dsrc = unboxToDouble(dsrc);
                           if (ownedTemps.count(srcName)) {
                               emitDecRefIfOwned(srcName);
                           }
                       }
                       bool hasF64Alloca = false;
                       if (tit0 != valueMap.end()) {
                           if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(tit0->second)) {
                               if (alloca->getAllocatedType()->isDoubleTy()) hasF64Alloca = true;
                           }
                       }
                       if (!hasF64Alloca) {
                           // If a PyObject* slot already exists, DECREF its current value once
                           // then replace the slot with an f64 alloca for the rest of the function.
                           if (tit0 != valueMap.end()) {
                               if (auto* oldA = llvm::dyn_cast<llvm::AllocaInst>(tit0->second)) {
                                   if (oldA->getAllocatedType() == pyObjectPtrTy) {
                                       llvm::Value* oldV = builder.CreateLoad(pyObjectPtrTy, oldA, inst.result + ".old");
                                       llvm::Function* decref = module->getFunction("Py_DECREF");
                                       if (decref) builder.CreateCall(decref, {oldV});
                                       // Keep null in old slot so exit cleanup is harmless if still referenced
                                       builder.CreateStore(llvm::ConstantPointerNull::get(pyObjectPtrTy), oldA);
                                   }
                               }
                           }
                           llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                                          func->getEntryBlock().begin());
                            llvm::AllocaInst* f64alloca = entryBuilder.CreateAlloca(
                                llvm::Type::getDoubleTy(context), nullptr, inst.result + ".f64");
                            // Debug info: track this native float variable.
                            emitDbgDeclare(f64alloca, inst.result, diFloatDI);
                            valueMap[inst.result] = f64alloca;
                       }
                       auto tit2 = valueMap.find(inst.result);
                       if (tit2 != valueMap.end()) {
                           if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(tit2->second)) {
                               if (alloca->getAllocatedType()->isDoubleTy()) {
                                   builder.CreateStore(dsrc, alloca);
                                   continue;
                               }
                           }
                       }
                   }

                 // Determine ownership of source. Owned temps already have refcount=1.
                bool srcIsOwned = ownedTemps.count(srcName) > 0;
                if (srcIsOwned) ownedTemps.erase(srcName);

                // Box native values. The box call creates a new owned ref.
                //
                // Severe, general, previously-unnoticed bug fixed here:
                // this switch didn't have an i1 case, even though the
                // "icmp" opcode's handler (above) deliberately stores a
                // *native*, unboxed i1 for a comparison between two
                // native i64/double operands (its own comment there says
                // so explicitly: "the result is used in a non-branch
                // context... getAsPyObject boxes it lazily via
                // PyBool_New" — getAsPyObject (this function's sibling
                // helper, used by "call" instruction arguments) already
                // has exactly this i1 case; "assign" duplicates
                // getAsPyObject's i64/double logic inline instead of
                // calling it, and that duplicate never got the i1 case
                // added). Result: assigning ANY comparison between two
                // proven-native values to a variable — not just chained
                // comparisons, and not just literal-vs-literal — crashed
                // LLVM module verification outright (`Call parameter
                // type does not match function signature! ... i1 true
                // ... call void @Py_INCREF(i1 true)`), since the raw i1
                // got passed straight to Py_INCREF as if it were a
                // PyObject*. Confirmed broad, realistic impact: `x = 1 <
                // 2`, `def f(a,b): x = a < b` (proven-native function
                // parameters), and `flag = i < 3` inside a loop over
                // range() (a proven-native loop variable) all crashed —
                // an entirely ordinary "name a comparison result" idiom,
                // not a rare edge case.
                llvm::Value* newVal = src;
                if (newVal->getType() == i64Ty) {
                    newVal = boxI64(newVal, srcName + ".boxed");
                    srcIsOwned = true;
                } else if (newVal->getType()->isDoubleTy()) {
                    newVal = boxDouble(newVal, srcName + ".boxed");
                    srcIsOwned = true;
                } else if (newVal->getType() == llvm::Type::getInt1Ty(context)) {
                    llvm::Function* boolNewAssign = module->getFunction("PyBool_New");
                    if (boolNewAssign) {
                        llvm::Value* i32v = builder.CreateZExt(newVal, llvm::Type::getInt32Ty(context));
                        newVal = builder.CreateCall(boolNewAssign, {i32v}, srcName + ".boxed");
                        srcIsOwned = true;
                    }
                }

                llvm::Function* incref = module->getFunction("Py_INCREF");
                llvm::Function* decref = module->getFunction("Py_DECREF");

                 auto tit = valueMap.find(inst.result);
                 if (tit != valueMap.end()) {
                     if (llvm::GlobalVariable* gv = llvm::dyn_cast<llvm::GlobalVariable>(tit->second)) {
                         // Global reassignment: DECREF old value (null-safe), INCREF new value.
                         // Module globals always own their values, so always INCREF unless source is null.
                         llvm::Value* oldVal = builder.CreateLoad(pyObjectPtrTy, gv, inst.result + ".old");
                         if (decref) builder.CreateCall(decref, {oldVal});
                         if (newVal && incref) builder.CreateCall(incref, {newVal});
                         builder.CreateStore(newVal, gv);
                    } else if (ownedSlots.count(inst.result)) {
                        // Owned slot: DECREF old value, store new.
                        llvm::Value* oldVal = builder.CreateLoad(pyObjectPtrTy, tit->second, inst.result + ".old");
                        if (decref) builder.CreateCall(decref, {oldVal});
                        if (!srcIsOwned && incref) builder.CreateCall(incref, {newVal});
                        builder.CreateStore(newVal, tit->second);
                     } else if (auto* paramAlloca = llvm::dyn_cast<llvm::AllocaInst>(tit->second)) {
                         // Borrowed slot (param): INCREF the initial value right after the
                         // parameter setup store so the slot owns a ref from function entry.
                         // This makes every subsequent reassignment (including loop iterations)
                         // safe to use the owned-slot pattern (DECREF old, store new).
                         if (incref) {
                             // Find the store that initializes this alloca (the param setup store
                             // emitted in the entry block during parameter setup) and insert the
                             // INCREF immediately after it.
                             llvm::Instruction* setupStore = nullptr;
                             for (auto& I : func->getEntryBlock()) {
                                 if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                                     if (SI->getPointerOperand() == paramAlloca) {
                                         setupStore = SI;
                                         break;
                                     }
                                 }
                             }
                             if (setupStore) {
                                 llvm::IRBuilder<> initBuilder(setupStore->getNextNode()
                                     ? setupStore->getNextNode()
                                     : &func->getEntryBlock().back());
                                 if (!setupStore->getNextNode())
                                     initBuilder.SetInsertPoint(
                                         &func->getEntryBlock(), func->getEntryBlock().end());
                                 else
                                     initBuilder.SetInsertPoint(setupStore->getNextNode());
                                 llvm::Value* initVal = initBuilder.CreateLoad(
                                     pyObjectPtrTy, paramAlloca, inst.result + ".init");
                                 initBuilder.CreateCall(incref, {initVal});
                             }
                         }
                         llvm::Value* oldVal = builder.CreateLoad(
                             pyObjectPtrTy, paramAlloca, inst.result + ".old");
                         if (decref) builder.CreateCall(decref, {oldVal});
                         // Always INCREF new value so the target owns its own reference
                         if (newVal && incref) builder.CreateCall(incref, {newVal});
                         builder.CreateStore(newVal, paramAlloca);
                         ownedSlots.insert(inst.result);
                     } else {
                         // Borrowed slot (cell or other non-alloca): simple take-ownership.
                         if (!srcIsOwned && incref) builder.CreateCall(incref, {newVal});
                         builder.CreateStore(newVal, tit->second);
                         ownedSlots.insert(inst.result);
                     }
                } else {
                    if (llvm::GlobalVariable* gv = module->getNamedGlobal("pyc_global_" + inst.result)) {
                        valueMap[inst.result] = gv;
                        if (!srcIsOwned && incref) builder.CreateCall(incref, {newVal});
                        builder.CreateStore(newVal, gv);
                    } else {
                        llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                                       func->getEntryBlock().begin());
                        llvm::AllocaInst* alloca = entryBuilder.CreateAlloca(pyObjectPtrTy, nullptr, inst.result);
                        // Debug info: track this local variable.
                        emitDbgDeclare(alloca, inst.result, diPyObjPtrDI);
                        // Null-init so DECREF of old is safe even on first use (loop re-assignment)
                        entryBuilder.CreateStore(llvm::ConstantPointerNull::get(pyObjectPtrTy), alloca);
                        valueMap[inst.result] = alloca;
                        ownedSlots.insert(inst.result);
                        // Always DECREF old: null on first iter (no-op), old ref on re-assignments
                        llvm::Value* oldVal = builder.CreateLoad(pyObjectPtrTy, alloca, inst.result + ".old");
                        if (decref) builder.CreateCall(decref, {oldVal});
                        if (!srcIsOwned && incref) builder.CreateCall(incref, {newVal});
                        builder.CreateStore(newVal, alloca);
                    }
                }
             } else if (inst.op == "call") {
                  std::string funcName = inst.operands.empty() ? "" : inst.operands[0].name;
                  if (std::getenv("PYC_DUMP_IR")) {
                      for (size_t ii = 1; ii < inst.operands.size(); ++ii) {
                          auto vit = valueMap.find(inst.operands[ii].name);
                          llvm::errs() << "  call " << funcName << " operand[" << ii << "]=" << inst.operands[ii].name << " in valueMap=" << (vit != valueMap.end() ? "yes" : "no") << "\n";
                      }
                  }
                   // PyObject_CompareBool(a, b, op) — third arg is i32, not ptr
                   if (funcName == "PyObject_CompareBool" && inst.operands.size() >= 4) {
                       std::string aName = inst.operands[1].name;
                       std::string bName = inst.operands[2].name;
                       std::string opStr = inst.operands[3].name;
                       int opVal = 0;
                       try { opVal = std::stoi(opStr); } catch (...) {}
                       llvm::Value* a = getAsPyObject(aName);
                       llvm::Value* b = getAsPyObject(bName);
                       llvm::Value* op = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), opVal);
                       llvm::Function* cmpFn = module->getFunction("PyObject_CompareBool");
                       if (cmpFn) {
                           llvm::Value* res = builder.CreateCall(cmpFn, {a, b, op}, inst.result);
                           if (!inst.result.empty()) {
                               valueMap[inst.result] = res;
                           }
                       }
                        emitDecRefIfOwned(aName);
                        emitDecRefIfOwned(bName);
                        continue;
                    }
                   // Pyc_Apply(token, argList) — dynamic dispatch via callable registry
                   if (funcName == "Pyc_Apply" && inst.operands.size() >= 3) {
                       std::string tokenName = inst.operands[1].name;
                       std::string argListName = inst.operands[2].name;
                       llvm::Value* tokenVal = getAsPyObject(tokenName);
                       llvm::Value* argListVal = getAsPyObject(argListName);
                       llvm::Function* applyFn = module->getFunction("Pyc_Apply");
                       if (applyFn) {
                           llvm::Value* res = builder.CreateCall(applyFn, {tokenVal, argListVal}, inst.result);
                           if (!inst.result.empty()) {
                               valueMap[inst.result] = res;
                               markOwned(inst.result);
                           }
                       }
                       emitDecRefIfOwned(tokenName);
                       emitDecRefIfOwned(argListName);
                       continue;
                   }
                   // Pyc_ApplyKw(token, argList, kwDict) — same dispatch, separate kwargs
                   if (funcName == "Pyc_ApplyKw" && inst.operands.size() >= 4) {
                       std::string tokenName = inst.operands[1].name;
                       std::string argListName = inst.operands[2].name;
                       std::string kwName = inst.operands[3].name;
                       llvm::Value* tokenVal = getAsPyObject(tokenName);
                       llvm::Value* argListVal = getAsPyObject(argListName);
                       llvm::Value* kwVal = getAsPyObject(kwName);
                       llvm::Function* applyFn = module->getFunction("Pyc_ApplyKw");
                       if (applyFn) {
                           llvm::Value* res = builder.CreateCall(applyFn, {tokenVal, argListVal, kwVal}, inst.result);
                           if (!inst.result.empty()) {
                               valueMap[inst.result] = res;
                               markOwned(inst.result);
                           }
                       }
                       emitDecRefIfOwned(tokenName);
                       emitDecRefIfOwned(argListName);
                       emitDecRefIfOwned(kwName);
                       continue;
                   }
                    // PyComplex_New(real: double, imag: double) — takes native doubles, not boxed ptrs
                    if (funcName == "PyComplex_New" && inst.operands.size() >= 3) {
                        std::string realName = inst.operands[1].name;
                        std::string imagName = inst.operands[2].name;
                        llvm::Value* realVal = getOrLoad(realName);
                        llvm::Value* imagVal = getOrLoad(imagName);
                        // Ensure they are double type
                        if (realVal->getType() != llvm::Type::getDoubleTy(context)) {
                            realVal = builder.CreateBitCast(realVal, llvm::Type::getDoubleTy(context), "real.cast");
                        }
                        if (imagVal->getType() != llvm::Type::getDoubleTy(context)) {
                            imagVal = builder.CreateBitCast(imagVal, llvm::Type::getDoubleTy(context), "imag.cast");
                        }
                        llvm::Function* complexFn = module->getFunction("PyComplex_New");
                        if (complexFn) {
                            llvm::Value* res = builder.CreateCall(complexFn, {realVal, imagVal}, inst.result);
                            if (!inst.result.empty()) {
                                valueMap[inst.result] = res;
                            }
                        }
                        emitDecRefIfOwned(realName);
                        emitDecRefIfOwned(imagName);
                        continue;
                    }
                    // PyComplex_Add/Sub/Mul/Div — complex arithmetic
                    if ((funcName == "PyComplex_Add" || funcName == "PyComplex_Sub" ||
                         funcName == "PyComplex_Mul" || funcName == "PyComplex_Div") &&
                        inst.operands.size() >= 3) {
                        std::string lhsName = inst.operands[1].name;
                        std::string rhsName = inst.operands[2].name;
                        llvm::Value* lhs = getAsPyObject(lhsName);
                        llvm::Value* rhs = getAsPyObject(rhsName);
                        llvm::Function* complexFn = module->getFunction(funcName);
                        if (complexFn) {
                            llvm::Value* res = builder.CreateCall(complexFn, {lhs, rhs}, inst.result);
                            if (!inst.result.empty()) {
                                valueMap[inst.result] = res;
                                markOwned(inst.result);
                            }
                        }
                        emitDecRefIfOwned(lhsName);
                        emitDecRefIfOwned(rhsName);
                        continue;
                    }
                  // i64-index list get: constant digit string or i64 SSA/local name
                  if (funcName == "PyList_GetItemI64" && inst.operands.size() >= 3) {
                      std::string listName = inst.operands[1].name;
                      std::string idxStr = inst.operands[2].name;
                      llvm::Value* listVal = getAsPyObject(listName);
                      llvm::Value* idxVal = nullptr;
                      bool isDigits = !idxStr.empty() && (isdigit(idxStr[0]) || idxStr[0]=='-');
                      if (isDigits) {
                          for (size_t k = 1; k < idxStr.size() && isDigits; ++k)
                              if (!isdigit(idxStr[k])) isDigits = false;
                      }
                      if (isDigits) {
                          long idx = 0;
                          try { idx = std::stol(idxStr); } catch (...) { idx = 0; }
                          idxVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), idx);
                      } else {
                          idxVal = getOrLoad(idxStr);
                          if (idxVal && idxVal->getType() != llvm::Type::getInt64Ty(context))
                              idxVal = unboxToI64(idxVal);
                      }
                      llvm::Function* getFn = module->getFunction("PyList_GetItemI64");
                      if (getFn && idxVal) {
                          llvm::Value* item = builder.CreateCall(getFn, {listVal, idxVal}, inst.result);
                          if (!inst.result.empty()) {
                              valueMap[inst.result] = item;
                              markOwned(inst.result);
                          }
                          emitDecRefIfOwnedSameBlock(listName);
                          continue;
                      }
                  }
                  if ((funcName == "PyList_SizeI64") && inst.operands.size() >= 2) {
                      llvm::Value* listVal = getAsPyObject(inst.operands[1].name);
                      llvm::Function* sz = module->getFunction("PyList_SizeI64");
                      if (sz) {
                          llvm::Value* n = builder.CreateCall(sz, {listVal}, inst.result);
                          if (!inst.result.empty()) valueMap[inst.result] = n;
                          emitDecRefIfOwnedSameBlock(inst.operands[1].name);
                          continue;
                      }
                  }
                  // PyList_UnpackStar(list, nBefore, nAfter) — nBefore/nAfter are raw i64 constants
                  if (funcName == "PyList_UnpackStar" && inst.operands.size() >= 3) {
                      llvm::Value* listVal = getAsPyObject(inst.operands[1].name);
                      // nBefore and nAfter are digit strings → raw i64 constants
                      long nBefore = 0, nAfter = 0;
                      try { nBefore = std::stol(inst.operands[2].name); } catch (...) {}
                      if (inst.operands.size() >= 4) {
                          try { nAfter = std::stol(inst.operands[3].name); } catch (...) {}
                      }
                      llvm::Value* nb = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), (uint64_t)nBefore);
                      llvm::Value* na = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), (uint64_t)nAfter);
                      llvm::Function* usFn = module->getFunction("PyList_UnpackStar");
                      if (usFn) {
                          llvm::Value* res = builder.CreateCall(usFn, {listVal, nb, na}, inst.result);
                          if (!inst.result.empty()) { valueMap[inst.result] = res; }
                          emitDecRefIfOwnedSameBlock(inst.operands[1].name);
                          continue;
                      }
                  }
                  // unpack2/3: operands = [func, list, e0, e1, (e2)]
                  if ((funcName == "PyList_Unpack2" || funcName == "PyList_Unpack3") &&
                      inst.operands.size() >= 4) {
                      std::string listName = inst.operands[1].name;
                      llvm::Value* listVal = getAsPyObject(listName);
                      size_t nOut = (funcName == "PyList_Unpack3") ? 3 : 2;
                      llvm::IRBuilder<> eb(&func->getEntryBlock(), func->getEntryBlock().begin());
                      std::vector<llvm::AllocaInst*> slots;
                      std::vector<std::string> outNames;
                       for (size_t k = 0; k < nOut; ++k) {
                           outNames.push_back(inst.operands[2 + k].name);
                           llvm::AllocaInst* slot = eb.CreateAlloca(pyObjectPtrTy, nullptr, outNames.back() + ".uslot");
                           // Debug info: track this unpack target variable.
                           emitDbgDeclare(slot, outNames.back(), diPyObjPtrDI);
                           eb.CreateStore(llvm::ConstantPointerNull::get(pyObjectPtrTy), slot);
                           slots.push_back(slot);
                      }
                      llvm::Function* ufn = module->getFunction(funcName);
                      if (ufn) {
                          std::vector<llvm::Value*> args = {listVal};
                          for (auto* s : slots) args.push_back(s);
                          builder.CreateCall(ufn, args);
                          for (size_t k = 0; k < nOut; ++k) {
                              llvm::Value* v = builder.CreateLoad(pyObjectPtrTy, slots[k], outNames[k]);
                              valueMap[outNames[k]] = v;
                              markOwned(outNames[k]);
                          }
                          emitDecRefIfOwnedSameBlock(listName);
                          continue;
                      }
                  }
                  // A4: native list subscript get for proven homogeneous lists.
                 if ((funcName == "Pyc_GetItem" || funcName == "Pyc_Subscript") && inst.operands.size() >= 3) {
                     std::string listName = inst.operands[1].name;
                     std::string idxName = inst.operands[2].name;
                     // Check if result type is known to be int or float (set by lowering).
                     if (inst.resultType == "int" || inst.resultType == "float") {
                         llvm::Value* listVal = getAsPyObject(listName);
                         llvm::Value* idxVal = getOrLoad(idxName);
                         // Index may be native i64 or a boxed PyObject* — always unbox properly.
                         if (idxVal->getType() != llvm::Type::getInt64Ty(context)) {
                             idxVal = unboxToI64(idxVal);
                         }
                         llvm::Value* nativeVal = nullptr;
                         if (inst.resultType == "int") {
                             llvm::Function* getInt64 = module->getFunction("PyList_GetItemInt64");
                             if (getInt64) {
                                 nativeVal = builder.CreateCall(getInt64, {listVal, idxVal}, inst.result + ".i64");
                             }
                         } else {
                             llvm::Function* getDouble = module->getFunction("PyList_GetItemDouble");
                             if (getDouble) {
                                 nativeVal = builder.CreateCall(getDouble, {listVal, idxVal}, inst.result + ".double");
                             }
                         }
                         if (nativeVal) {
                             // Box into valueMap; native arithmetic still unboxes via resultType.
                             llvm::Value* boxed = (inst.resultType == "int")
                                 ? boxI64(nativeVal, inst.result + ".boxed")
                                 : boxDouble(nativeVal, inst.result + ".boxed");
                             if (!inst.result.empty()) {
                                 valueMap[inst.result] = boxed;
                                 markOwned(inst.result);
                             }
                             emitDecRefIfOwnedSameBlock(listName);
                             emitDecRefIfOwnedSameBlock(idxName);
                             continue;
                         }
                          // Fall through to generic path if native functions not found.
                      }
                  }
                  // A4: native list subscript set for proven homogeneous lists.
                  if (funcName == "PyList_SetItemInt64" && inst.operands.size() >= 4) {
                      std::string listName = inst.operands[1].name;
                      std::string idxName = inst.operands[2].name;
                      std::string valName = inst.operands[3].name;
                      llvm::Value* listVal = getAsPyObject(listName);
                      llvm::Value* idxVal = getOrLoad(idxName);
                      // Unbox index to i64 if it's a boxed PyObject*
                      if (idxVal->getType() != llvm::Type::getInt64Ty(context)) {
                          idxVal = unboxToI64(idxVal);
                      }
                      llvm::Value* valVal = getOrLoad(valName);
                      // Unbox value to i64 if it's a boxed PyObject*
                      if (valVal->getType() != llvm::Type::getInt64Ty(context)) {
                          valVal = unboxToI64(valVal);
                      }
                      llvm::Function* setFn = module->getFunction("PyList_SetItemInt64");
                      if (setFn) builder.CreateCall(setFn, {listVal, idxVal, valVal});
                      emitDecRefIfOwnedSameBlock(listName);
                      emitDecRefIfOwnedSameBlock(idxName);
                      emitDecRefIfOwnedSameBlock(valName);
                      continue;
                  }
                  if (funcName == "PyList_SetItemDouble" && inst.operands.size() >= 4) {
                      std::string listName = inst.operands[1].name;
                      std::string idxName = inst.operands[2].name;
                      std::string valName = inst.operands[3].name;
                      llvm::Value* listVal = getAsPyObject(listName);
                      llvm::Value* idxVal = getOrLoad(idxName);
                      // Unbox index to i64 if it's a boxed PyObject*
                      if (idxVal->getType() != llvm::Type::getInt64Ty(context)) {
                          idxVal = unboxToI64(idxVal);
                      }
                      llvm::Value* valVal = getOrLoad(valName);
                      // Unbox and convert value to double if it's a boxed PyObject*
                      if (valVal->getType() != llvm::Type::getDoubleTy(context)) {
                          if (valVal->getType() == llvm::Type::getInt64Ty(context)) {
                              valVal = builder.CreateSIToFP(valVal, llvm::Type::getDoubleTy(context), "setval.double");
                          } else {
                              // Boxed value - unbox to double
                              valVal = unboxToDouble(valVal);
                          }
                      }
                      llvm::Function* setFn = module->getFunction("PyList_SetItemDouble");
                      if (setFn) builder.CreateCall(setFn, {listVal, idxVal, valVal});
                      emitDecRefIfOwnedSameBlock(listName);
                      emitDecRefIfOwnedSameBlock(idxName);
                       emitDecRefIfOwnedSameBlock(valName);
                       continue;
                   }
                   if (funcName == "PyList_SetItemInt64Auto" && inst.operands.size() >= 4) {
                       std::string listName = inst.operands[1].name;
                       std::string idxName = inst.operands[2].name;
                       std::string valName = inst.operands[3].name;
                       llvm::Value* listVal = getAsPyObject(listName);
                       llvm::Value* idxVal = getOrLoad(idxName);
                       if (idxVal->getType() != llvm::Type::getInt64Ty(context)) {
                           idxVal = unboxToI64(idxVal);
                       }
                       llvm::Value* valVal = getOrLoad(valName);
                       if (valVal->getType() != llvm::Type::getInt64Ty(context)) {
                           valVal = unboxToI64(valVal);
                       }
                       llvm::Function* setFn = module->getFunction("PyList_SetItemInt64Auto");
                       if (setFn) builder.CreateCall(setFn, {listVal, idxVal, valVal});
                       emitDecRefIfOwnedSameBlock(listName);
                       emitDecRefIfOwnedSameBlock(idxName);
                       emitDecRefIfOwnedSameBlock(valName);
                       continue;
                   }
                   if (funcName == "PyList_SetItemDoubleAuto" && inst.operands.size() >= 4) {
                       std::string listName = inst.operands[1].name;
                       std::string idxName = inst.operands[2].name;
                       std::string valName = inst.operands[3].name;
                       llvm::Value* listVal = getAsPyObject(listName);
                       llvm::Value* idxVal = getOrLoad(idxName);
                       if (idxVal->getType() != llvm::Type::getInt64Ty(context)) {
                           idxVal = unboxToI64(idxVal);
                       }
                       llvm::Value* valVal = getOrLoad(valName);
                       if (valVal->getType() != llvm::Type::getDoubleTy(context)) {
                           if (valVal->getType() == llvm::Type::getInt64Ty(context)) {
                               valVal = builder.CreateSIToFP(valVal, llvm::Type::getDoubleTy(context), "setval.double");
                           } else {
                               valVal = unboxToDouble(valVal);
                           }
                       }
                       llvm::Function* setFn = module->getFunction("PyList_SetItemDoubleAuto");
                       if (setFn) builder.CreateCall(setFn, {listVal, idxVal, valVal});
                       emitDecRefIfOwnedSameBlock(listName);
                       emitDecRefIfOwnedSameBlock(idxName);
                       emitDecRefIfOwnedSameBlock(valName);
                       continue;
                   }
                  if (funcName == "print") {
                    // Legacy single-arg print fast-path: pyc_print covers the
                    // general case (multi-arg + kwargs) at the lowering level.
                    llvm::Function* pyPrint = module->getFunction("PyObject_Print");
                    if (pyPrint) {
                        std::string argName = inst.operands.size() > 1 ? inst.operands[1].name : "";
                        llvm::Value* arg = argName.empty() ? llvm::ConstantPointerNull::get(pyObjectPtrTy)
                                                           : getAsPyObject(argName);
                        builder.CreateCall(pyPrint, {arg, llvm::ConstantPointerNull::get(int8PtrTy)});
                        if (!argName.empty()) emitDecRefIfOwned(argName);
                    }
                } else {
                    llvm::Function* callee = module->getFunction(funcName);
                    if (!callee) callee = module->getFunction(llvmFunctionName(funcName));
                    if (!callee) {
                        // Function not found in this module - create external declaration
                        // This handles cross-module calls like __module__utils
                        // Use the actual number of arguments from the instruction
                        size_t numArgs = inst.operands.size() - 1;
                        std::vector<llvm::Type*> argTypes(numArgs, pyObjectPtrTy);
                        llvm::FunctionType* extTy = llvm::FunctionType::get(pyObjectPtrTy, argTypes, false);
                        callee = llvm::Function::Create(extTy, llvm::Function::ExternalLinkage, funcName, module.get());
                    }
                    if (callee) {
                        std::vector<llvm::Value*> callArgs;
                        // Track which args were native (i64/double) so that getAsPyObject's
                        // anonymous box can be DECREFed after the call.
                        std::vector<bool> argWasNative;

                        // A6: Check if there's a specialized variant for this call.
                        // When all args are numeric (native i64/double), dispatch to the
                        // specialized variant. This applies both to ordinary functions and
                        // to specialized variants themselves — the latter enables native
                        // self-recursion (e.g. __specialized_fib_i calling itself for
                        // recursive fib(n-1)/fib(n-2) without boxing).
                        //
                        // I-014 / W5.8: if some args are still boxed PyObject*, speculate
                        // with a runtime tag check (type==0 int / type==4 float) and call
                        // the matching variant; wrong tag or null falls back to the boxed
                        // callee. Indirect calls (Pyc_Apply) never reach this arm.
                        bool useSpecialized = false;
                        std::string specializedName;
                        llvm::Type* i64TyCall = llvm::Type::getInt64Ty(context);
                        llvm::Type* doubleTyCall = llvm::Type::getDoubleTy(context);
                        llvm::Type* i32TyCall = llvm::Type::getInt32Ty(context);
                        size_t numCallArgs = inst.operands.size() - 1;
                        std::vector<std::string> argTypes; // "i64", "double", or "boxed"
                        std::vector<llvm::Value*> rawArgVals;
                        bool specShapesOk = true;
                        {
                            if (numCallArgs > 0) {
                                for (size_t i = 1; i < inst.operands.size(); ++i) {
                                    llvm::Value* raw = getOrLoad(inst.operands[i].name);
                                    rawArgVals.push_back(raw);
                                    if (raw && raw->getType() == i64TyCall) {
                                        argTypes.push_back("i64");
                                    } else if (raw && raw->getType()->isDoubleTy()) {
                                        argTypes.push_back("double");
                                    } else if (raw && raw->getType()->isPointerTy()) {
                                        argTypes.push_back("boxed");
                                    } else {
                                        argTypes.push_back("boxed");
                                        specShapesOk = false;
                                    }
                                }

                                // Fast path: all args are already native (i64/double) — dispatch directly.
                                // This handles self-recursion in specialized variants.
                                bool allNative = true;
                                std::string nativeSig;
                                for (size_t i = 0; i < argTypes.size(); ++i) {
                                    if (argTypes[i] == "i64") {
                                        nativeSig += "i";
                                    } else if (argTypes[i] == "double") {
                                        nativeSig += "f";
                                    } else {
                                        allNative = false;
                                        break;
                                    }
                                }
                                if (allNative) {
                                    std::string candidate = "__specialized_" + funcName + "_" + nativeSig;
                                    llvm::Function* spec = module->getFunction(candidate);
                                    if (spec) {
                                        useSpecialized = true;
                                        specializedName = candidate;
                                        callee = spec;
                                    }
                                }
                            }
                        }

                        // Collect specialized-variant candidates for mixed boxed/native args.
                        const IRFunction* calleeIR = nullptr;
                        for (const auto& cf : ir.functions) {
                            if (cf.name == funcName) { calleeIR = &cf; break; }
                        }
                        size_t ncells = calleeIR ? calleeIR->freeCellVars.size() : 0;
                        if (ncells >= numCallArgs) ncells = 0;
                        size_t nuser = (numCallArgs >= ncells) ? (numCallArgs - ncells) : 0;
                        struct SpecCand { std::string sig; llvm::Function* fn; };
                        std::vector<SpecCand> specCands;
                        if (!useSpecialized && specShapesOk && nuser > 0 && !callee->getReturnType()->isVoidTy()) {
                            bool anyBoxedUser = false;
                            for (size_t u = 0; u < nuser; ++u) {
                                if (argTypes[ncells + u] == "boxed") anyBoxedUser = true;
                            }
                            if (anyBoxedUser) {
                                std::vector<std::string> trySigs;
                                if (calleeIR && !calleeIR->specializedSignatures.empty()) {
                                    for (const auto& s : calleeIR->specializedSignatures)
                                        trySigs.push_back(s);
                                } else {
                                    std::vector<size_t> boxedIdx;
                                    for (size_t u = 0; u < nuser; ++u) {
                                        if (argTypes[ncells + u] == "boxed") boxedIdx.push_back(u);
                                    }
                                    if (boxedIdx.size() <= 8) {
                                        size_t ncomb = size_t{1} << boxedIdx.size();
                                        for (size_t mask = 0; mask < ncomb; ++mask) {
                                            std::string sig(nuser, 'i');
                                            for (size_t u = 0; u < nuser; ++u) {
                                                if (argTypes[ncells + u] == "double") sig[u] = 'f';
                                                else if (argTypes[ncells + u] == "i64") sig[u] = 'i';
                                            }
                                            for (size_t bi = 0; bi < boxedIdx.size(); ++bi)
                                                sig[boxedIdx[bi]] = (mask & (size_t{1} << bi)) ? 'f' : 'i';
                                            trySigs.push_back(sig);
                                        }
                                    }
                                }
                                for (const auto& sig : trySigs) {
                                    if (sig.size() != nuser) continue;
                                    bool compat = true;
                                    for (size_t u = 0; u < nuser; ++u) {
                                        const std::string& at = argTypes[ncells + u];
                                        if (at == "i64" && sig[u] != 'i') compat = false;
                                        else if (at == "double" && sig[u] != 'f') compat = false;
                                        else if (at == "boxed" && sig[u] != 'i' && sig[u] != 'f') compat = false;
                                    }
                                    if (!compat) continue;
                                    std::string candName = "__specialized_" + funcName + "_" + sig;
                                    llvm::Function* spec = module->getFunction(candName);
                                    if (!spec) continue;
                                    if (spec->arg_size() != ncells + nuser) continue;
                                    llvm::FunctionType* sty = spec->getFunctionType();
                                    bool argsOk = true;
                                    for (size_t c = 0; c < ncells; ++c) {
                                        if (!sty->getParamType(c)->isPointerTy()) argsOk = false;
                                    }
                                    for (size_t u = 0; u < nuser; ++u) {
                                        llvm::Type* pt = sty->getParamType(ncells + u);
                                        if (sig[u] == 'i' && pt != i64TyCall) argsOk = false;
                                        if (sig[u] == 'f' && !pt->isDoubleTy()) argsOk = false;
                                    }
                                    if (!argsOk) continue;
                                    specCands.push_back({sig, spec});
                                }
                            }
                        }

                        if (useSpecialized) {
                            // Call specialized variant with native args (no boxing).
                            for (size_t i = 1; i < inst.operands.size(); ++i) {
                                callArgs.push_back(getOrLoad(inst.operands[i].name));
                            }
                            if (callee->getReturnType()->isVoidTy()) {
                                builder.CreateCall(callee, callArgs);
                            } else {
                                llvm::Value* callRes = builder.CreateCall(callee, callArgs, inst.result);
                                valueMap[inst.result] = callRes;
                                // A6 native return: if the variant returns i64/double,
                                // the result is a native value with no refcount.
                                bool retIsNative = callee->getReturnType() == llvm::Type::getInt64Ty(context)
                                                   || callee->getReturnType()->isDoubleTy();
                                if (retIsNative) {
                                    // Native values have no refcount — don't DECREF or markOwned.
                                } else {
                                    bool isUserFunc = !callee->isDeclaration()
                                                     || userFunctionNames.count(specializedName) > 0;
                                    if (!inst.result.empty() && tempUseCounts.count(inst.result) == 0
                                        && isUserFunc) {
                                        llvm::Function* decref2 = module->getFunction("Py_DECREF");
                                        if (decref2) builder.CreateCall(decref2, {callRes});
                                    } else {
                                        markOwned(inst.result);
                                    }
                                }
                            }
                        } else if (!specCands.empty()) {
                            // I-014 speculative unbox: runtime tag check, then variant or boxed fallback.
                            llvm::Function* boxedCallee = callee;
                            std::string sid = inst.result.empty()
                                ? ("s" + std::to_string(specUnboxSeq++))
                                : inst.result;

                            // Native join only when every candidate returns the same native type
                            // AND the call result is assigned to a proven local (never a module global).
                            bool allSpecI64 = true, allSpecF64 = true;
                            for (const auto& c : specCands) {
                                if (c.fn->getReturnType() != i64TyCall) allSpecI64 = false;
                                if (!c.fn->getReturnType()->isDoubleTy()) allSpecF64 = false;
                            }
                            std::string assignDest;
                            for (const auto& later : f.body) {
                                if (later.op == "assign" && !later.operands.empty()
                                    && later.operands[0].name == inst.result) {
                                    assignDest = later.result;
                                    break;
                                }
                            }
                            bool destIsGlobal = !assignDest.empty()
                                && module->getNamedGlobal("pyc_global_" + assignDest) != nullptr;
                            bool destProvenInt = false, destProvenFloat = false;
                            if (!assignDest.empty() && !destIsGlobal) {
                                auto vit = valueMap.find(assignDest);
                                if (vit != valueMap.end()) {
                                    if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(vit->second)) {
                                        if (a->getAllocatedType() == i64TyCall) destProvenInt = true;
                                        if (a->getAllocatedType()->isDoubleTy()) destProvenFloat = true;
                                    }
                                }
                                for (const auto& nl : f.numericLocals)
                                    if (nl == assignDest) destProvenInt = true;
                                for (const auto& nfl : f.numericFloatLocals)
                                    if (nfl == assignDest) destProvenFloat = true;
                            }
                            enum class JoinKind { Boxed, I64, F64 };
                            JoinKind joinKind = JoinKind::Boxed;
                            if (destProvenInt && !destProvenFloat && allSpecI64)
                                joinKind = JoinKind::I64;
                            else if (destProvenFloat && allSpecF64)
                                joinKind = JoinKind::F64;
                            llvm::Type* joinTy = pyObjectPtrTy;
                            if (joinKind == JoinKind::I64) joinTy = i64TyCall;
                            else if (joinKind == JoinKind::F64) joinTy = doubleTyCall;

                            auto convertToJoin = [&](llvm::Value* v) -> llvm::Value* {
                                if (!v) return llvm::Constant::getNullValue(joinTy);
                                if (joinKind == JoinKind::Boxed) {
                                    if (v->getType() == i64TyCall)
                                        return boxI64(v, sid + ".specbox");
                                    if (v->getType()->isDoubleTy())
                                        return boxDouble(v, sid + ".specbox");
                                    return v;
                                }
                                if (joinKind == JoinKind::I64) {
                                    if (v->getType() == i64TyCall) return v;
                                    if (v->getType()->isDoubleTy())
                                        return builder.CreateFPToSI(v, i64TyCall, sid + ".f2i");
                                    return unboxToI64(v);
                                }
                                if (v->getType()->isDoubleTy()) return v;
                                if (v->getType() == i64TyCall)
                                    return builder.CreateSIToFP(v, doubleTyCall, sid + ".i2f");
                                return unboxToDouble(v);
                            };

                            llvm::BasicBlock* originBB = builder.GetInsertBlock();
                            std::vector<std::string> boxedTempsToDecref;
                            for (size_t i = 1; i < inst.operands.size(); ++i) {
                                llvm::Value* raw = rawArgVals[i - 1];
                                bool isNat = raw && (raw->getType() == i64TyCall
                                                     || raw->getType()->isDoubleTy());
                                if (isNat) continue;
                                const std::string& nm = inst.operands[i].name;
                                if (!ownedTemps.count(nm)) continue;
                                auto bit = tempDefBlock.find(nm);
                                if (bit != tempDefBlock.end() && bit->second == originBB)
                                    boxedTempsToDecref.push_back(nm);
                            }

                            llvm::BasicBlock* joinBB = llvm::BasicBlock::Create(
                                context, "spec.join." + sid, func);
                            llvm::BasicBlock* fallbackBB = llvm::BasicBlock::Create(
                                context, "spec.fallback." + sid, func);
                            std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> phiInc;

                            auto emitNullAndTag = [&](llvm::Value* obj, char expect,
                                                      llvm::BasicBlock* passBB,
                                                      llvm::BasicBlock* failBB,
                                                      const std::string& tag) {
                                llvm::BasicBlock* tagBB = llvm::BasicBlock::Create(
                                    context, tag + ".tag", func);
                                llvm::Value* isnull = builder.CreateICmpEQ(
                                    obj, llvm::ConstantPointerNull::get(pyObjectPtrTy),
                                    tag + ".isnull");
                                builder.CreateCondBr(isnull, failBB, tagBB);
                                builder.SetInsertPoint(tagBB);
                                llvm::Value* typePtr = builder.CreateStructGEP(
                                    pyObjectTy, obj, 1, tag + ".tptr");
                                llvm::Value* typeVal = builder.CreateAlignedLoad(
                                    i32TyCall, typePtr, llvm::Align(4), tag + ".type");
                                unsigned expectTag = (expect == 'f') ? 4u : 0u;
                                llvm::Value* ok = builder.CreateICmpEQ(
                                    typeVal,
                                    llvm::ConstantInt::get(i32TyCall, expectTag),
                                    tag + ".ok");
                                builder.CreateCondBr(ok, passBB, failBB);
                            };

                            llvm::BasicBlock* tryBB = originBB;
                            for (size_t ci = 0; ci < specCands.size(); ++ci) {
                                const std::string& sig = specCands[ci].sig;
                                llvm::Function* specFn = specCands[ci].fn;
                                bool last = (ci + 1 == specCands.size());
                                llvm::BasicBlock* nextTry = last ? fallbackBB
                                    : llvm::BasicBlock::Create(context,
                                        "spec.try." + sid + "." + std::to_string(ci), func);
                                llvm::BasicBlock* specBB = llvm::BasicBlock::Create(
                                    context, "spec.then." + sid + "." + std::to_string(ci), func);

                                builder.SetInsertPoint(tryBB);
                                llvm::BasicBlock* passSoFar = tryBB;
                                bool anyCheck = false;
                                for (size_t u = 0; u < nuser; ++u) {
                                    if (argTypes[ncells + u] != "boxed") continue;
                                    llvm::BasicBlock* nextPass = specBB;
                                    // If more boxed user args remain after this one, land in a new block.
                                    bool more = false;
                                    for (size_t u2 = u + 1; u2 < nuser; ++u2) {
                                        if (argTypes[ncells + u2] == "boxed") { more = true; break; }
                                    }
                                    if (more) {
                                        nextPass = llvm::BasicBlock::Create(
                                            context,
                                            "spec.chk." + sid + "." + std::to_string(ci)
                                                + "." + std::to_string(u),
                                            func);
                                    }
                                    builder.SetInsertPoint(passSoFar);
                                    emitNullAndTag(
                                        rawArgVals[ncells + u], sig[u], nextPass, nextTry,
                                        "spec." + sid + "." + std::to_string(ci) + "." + std::to_string(u));
                                    passSoFar = nextPass;
                                    anyCheck = true;
                                }
                                if (!anyCheck) {
                                    builder.SetInsertPoint(tryBB);
                                    builder.CreateBr(specBB);
                                }

                                builder.SetInsertPoint(specBB);
                                std::vector<llvm::Value*> specArgs;
                                for (size_t c = 0; c < ncells; ++c)
                                    specArgs.push_back(rawArgVals[c]);
                                for (size_t u = 0; u < nuser; ++u) {
                                    llvm::Value* raw = rawArgVals[ncells + u];
                                    if (argTypes[ncells + u] == "i64") {
                                        specArgs.push_back(raw);
                                    } else if (argTypes[ncells + u] == "double") {
                                        specArgs.push_back(raw);
                                    } else {
                                        if (sig[u] == 'f') {
                                            llvm::Value* dptr = builder.CreateStructGEP(
                                                pyObjectTy, raw, 3, sid + ".fptr");
                                            specArgs.push_back(builder.CreateAlignedLoad(
                                                doubleTyCall, dptr, llvm::Align(8), sid + ".f"));
                                        } else {
                                            llvm::Value* iptr = builder.CreateStructGEP(
                                                pyObjectTy, raw, 2, sid + ".iptr");
                                            specArgs.push_back(builder.CreateAlignedLoad(
                                                i64TyCall, iptr, llvm::Align(8), sid + ".i"));
                                        }
                                    }
                                }
                                llvm::Value* specRes = builder.CreateCall(
                                    specFn, specArgs, sid + ".spec");
                                llvm::Value* conv = convertToJoin(specRes);
                                llvm::BasicBlock* specEnd = builder.GetInsertBlock();
                                builder.CreateBr(joinBB);
                                phiInc.push_back({conv, specEnd});
                                tryBB = nextTry;
                            }

                            builder.SetInsertPoint(fallbackBB);
                            std::vector<llvm::Value*> boxedArgs;
                            std::vector<bool> fbWasNative;
                            for (size_t i = 1; i < inst.operands.size(); ++i) {
                                llvm::Value* raw = rawArgVals[i - 1];
                                bool isNat = raw && (raw->getType() == i64TyCall
                                                     || raw->getType()->isDoubleTy());
                                fbWasNative.push_back(isNat);
                                boxedArgs.push_back(getAsPyObject(inst.operands[i].name));
                            }
                            llvm::Value* fbRes = builder.CreateCall(
                                boxedCallee, boxedArgs, sid + ".boxed");
                            llvm::Function* argDecrefFb = module->getFunction("Py_DECREF");
                            for (size_t i = 0; i < fbWasNative.size(); ++i) {
                                if (fbWasNative[i] && argDecrefFb)
                                    builder.CreateCall(argDecrefFb, {boxedArgs[i]});
                            }
                            llvm::Value* fbConv = convertToJoin(fbRes);
                            llvm::BasicBlock* fbEnd = builder.GetInsertBlock();
                            builder.CreateBr(joinBB);
                            phiInc.push_back({fbConv, fbEnd});

                            builder.SetInsertPoint(joinBB);
                            curBlock = joinBB;
                            llvm::PHINode* phi = builder.CreatePHI(joinTy, phiInc.size(), inst.result);
                            for (auto& inc : phiInc)
                                phi->addIncoming(inc.first, inc.second);
                            if (!inst.result.empty()) {
                                valueMap[inst.result] = phi;
                                if (joinKind == JoinKind::Boxed) {
                                    bool isUserFunc = !boxedCallee->isDeclaration()
                                                     || userFunctionNames.count(funcName) > 0;
                                    if (tempUseCounts.count(inst.result) == 0 && isUserFunc) {
                                        llvm::Function* decref2 = module->getFunction("Py_DECREF");
                                        if (decref2) builder.CreateCall(decref2, {phi});
                                    } else {
                                        markOwned(inst.result);
                                    }
                                }
                            }
                            for (const auto& nm : boxedTempsToDecref)
                                emitDecRefIfOwned(nm);
                        } else {
                            // Original boxed path.
                            for (size_t i = 1; i < inst.operands.size(); ++i) {
                                llvm::Value* raw = rawArgVals.empty()
                                    ? getOrLoad(inst.operands[i].name)
                                    : rawArgVals[i - 1];
                                bool isNative = raw && (raw->getType() == llvm::Type::getInt64Ty(context)
                                                        || raw->getType()->isDoubleTy());
                                argWasNative.push_back(isNative);
                                callArgs.push_back(getAsPyObject(inst.operands[i].name));
                            }
                            if (callee->getReturnType()->isVoidTy()) {
                                builder.CreateCall(callee, callArgs);
                            } else {
                                llvm::Value* callRes = builder.CreateCall(callee, callArgs, inst.result);
                                valueMap[inst.result] = callRes;
                                bool isUserFunc = !callee->isDeclaration()
                                                 || userFunctionNames.count(funcName) > 0;
                                if (!inst.result.empty() && tempUseCounts.count(inst.result) == 0
                                    && isUserFunc) {
                                    // Result of a user-defined function is never used — free immediately.
                                    // User functions always return new refs.
                                    // Runtime library functions may return borrowed refs, so we only
                                    // do this for user-defined functions (identified by userFunctionNames
                                    // set, which handles forward-declared functions not yet having bodies).
                                    llvm::Function* decref2 = module->getFunction("Py_DECREF");
                                    if (decref2) builder.CreateCall(decref2, {callRes});
                                } else {
                                    markOwned(inst.result);
                                }
                            }
                            // Only DECREF call arguments that were defined in THIS block.
                            // Arguments from a different (outer) block may be loop-persistent
                            // (e.g., a range/list passed to GetItem on every loop iteration).
                            llvm::Function* argDecref = module->getFunction("Py_DECREF");
                            for (size_t i = 1; i < inst.operands.size(); ++i) {
                                if (argWasNative[i - 1]) {
                                    // Anonymous box created by getAsPyObject — DECREF unconditionally.
                                    if (argDecref) builder.CreateCall(argDecref, {callArgs[i - 1]});
                                } else {
                                    emitDecRefIfOwnedSameBlock(inst.operands[i].name);
                                }
                            }
                        }
                    }
                }
            } else if (inst.op == "ret") {
                std::string retName = inst.operands.empty() ? "" : inst.operands[0].name;

                // A6 native return: specialized variants with a proven numeric return
                // type return i64/double directly. This skips all refcounting on the
                // return value and enables fully native recursive chains.
                if (!nativeRetType.empty()) {
                    llvm::Value* nativeRetVal = nullptr;
                    if (retName.empty()) {
                        nativeRetVal = (nativeRetType == "float")
                            ? (llvm::Value*)llvm::ConstantFP::get(context, llvm::APFloat(0.0))
                            : (llvm::Value*)llvm::ConstantInt::get(context, llvm::APInt(64, 0));
                    } else {
                        llvm::Value* raw = getOrLoad(retName);
                        if (nativeRetType == "int") {
                            nativeRetVal = unboxToI64(raw);
                        } else { // float
                            nativeRetVal = unboxToDouble(raw);
                        }
                    }
                    // DECREF all owned boxed slots at function exit (native return
                    // value has no refcount, but locals still need cleanup).
                    {
                        llvm::Function* slotDecref = module->getFunction("Py_DECREF");
                        if (slotDecref) {
                            for (const auto& slotName : ownedSlots) {
                                auto vit = valueMap.find(slotName);
                                if (vit != valueMap.end()) {
                                    if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(vit->second)) {
                                        if (alloca->getAllocatedType() != pyObjectPtrTy) continue;
                                        llvm::Value* slotVal = builder.CreateLoad(
                                            pyObjectPtrTy, alloca, slotName + ".exit");
                                        builder.CreateCall(slotDecref, {slotVal});
                                    }
                                }
                            }
                        }
                    }
                    if (!curBlock->getTerminator()) {
                        emitPopFrame();
                        builder.CreateRet(nativeRetVal);
                    }
                    continue;  // skip the boxed return path below
                }

                llvm::Value* retVal = getAsPyObject(retName);
                bool retIsOwned = ownedTemps.count(retName) > 0;
                if (retIsOwned) ownedTemps.erase(retName);

                if (retVal == llvm::ConstantPointerNull::get(pyObjectPtrTy)) {
                    llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
                    if (fromLong) {
                        retVal = builder.CreateCall(fromLong, {llvm::ConstantInt::get(context, llvm::APInt(64, 0))});
                        retIsOwned = true;
                    }
                }

                if (!retIsOwned) {
                    // Borrowed ref (param/slot): INCREF to give caller a proper new ref.
                    // Without this, if the caller DECREFs the argument that was also returned,
                    // both the arg-decref and result-decref would free the same object.
                    llvm::Function* incref = module->getFunction("Py_INCREF");
                    if (incref) builder.CreateCall(incref, {retVal});
                }

                // DECREF all owned slots at function exit. retVal is already
                // INCREFd if it came from a slot, so slot cleanup is safe.
                // Slots null-initialized at alloca creation make cross-path cleanup
                // a no-op (DECREF(null) is safe in our runtime).
                // Skip native f64/i64 allocas (not PyObject*).
                {
                    llvm::Function* slotDecref = module->getFunction("Py_DECREF");
                    if (slotDecref) {
                        for (const auto& slotName : ownedSlots) {
                            auto vit = valueMap.find(slotName);
                            if (vit != valueMap.end()) {
                                if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(vit->second)) {
                                    if (alloca->getAllocatedType() != pyObjectPtrTy) continue;
                                    llvm::Value* slotVal = builder.CreateLoad(
                                        pyObjectPtrTy, alloca, slotName + ".exit");
                                    builder.CreateCall(slotDecref, {slotVal});
                                }
                            }
                        }
                    }
                }

                if (!curBlock->getTerminator()) {
                    emitPopFrame();
                    builder.CreateRet(retVal);
                }
                // No break — continue processing labels/branches for other paths
            }
        }
        if (!curBlock->getTerminator()) {
            // A6 native return: implicit fall-through return for specialized variants
            // with native return type returns 0 as the native type.
            if (!nativeRetType.empty()) {
                llvm::Value* zero = (nativeRetType == "float")
                    ? (llvm::Value*)llvm::ConstantFP::get(context, llvm::APFloat(0.0))
                    : (llvm::Value*)llvm::ConstantInt::get(context, llvm::APInt(64, 0));
                // DECREF owned slots (skip native f64/i64)
                {
                    llvm::Function* slotDecref = module->getFunction("Py_DECREF");
                    if (slotDecref) {
                        for (const auto& slotName : ownedSlots) {
                            auto vit = valueMap.find(slotName);
                            if (vit != valueMap.end()) {
                                if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(vit->second)) {
                                    if (alloca->getAllocatedType() != pyObjectPtrTy) continue;
                                    llvm::Value* slotVal = builder.CreateLoad(
                                        pyObjectPtrTy, alloca, slotName + ".exit");
                                    builder.CreateCall(slotDecref, {slotVal});
                                }
                            }
                        }
                    }
                }
                emitPopFrame();
                builder.CreateRet(zero);
            } else {
                // Return a boxed 0 as a sensible default instead of null
                llvm::Function* fromLong = module->getFunction("PyInt_FromLong");
                llvm::Value* zero = fromLong
                    ? (llvm::Value*)builder.CreateCall(fromLong, {llvm::ConstantInt::get(context, llvm::APInt(64, 0))})
                    : (llvm::Value*)llvm::ConstantPointerNull::get(pyObjectPtrTy);
                // DECREF all owned slots before the implicit return (skip native f64/i64)
                {
                    llvm::Function* slotDecref = module->getFunction("Py_DECREF");
                    if (slotDecref) {
                        for (const auto& slotName : ownedSlots) {
                            auto vit = valueMap.find(slotName);
                            if (vit != valueMap.end()) {
                                if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(vit->second)) {
                                    if (alloca->getAllocatedType() != pyObjectPtrTy) continue;
                                    llvm::Value* slotVal = builder.CreateLoad(
                                        pyObjectPtrTy, alloca, slotName + ".exit");
                                    builder.CreateCall(slotDecref, {slotVal});
                                }
                            }
                        }
                    }
                }
                emitPopFrame();
                builder.CreateRet(zero);
            }
        }
    }
    if (llvm::verifyModule(*module, &llvm::errs())) {
        std::cerr << "Module verification failed\n";
        if (std::getenv("PYC_DUMP_BAD_IR")) {
            std::string s;
            llvm::raw_string_ostream os(s);
            module->print(os, nullptr);
            std::cerr << s << std::endl;
        }
        return nullptr;
    }
    // DEBUG: print instructions for the module function
    if (std::getenv("PYC_DUMP_IR")) {
        for (auto& f : *module) {
            if (f.getName() == "__module__") {
                std::string s;
                llvm::raw_string_ostream os(s);
                f.print(os);
                std::cerr << "=== __module__ IR ===\n" << s << "\n=== END ===\n";
            }
        }
    }

    // Debug info: finalize the DIBuilder before returning the module.
    if (diBuilder) {
        diBuilder->finalize();
    }
    return module;
}

bool Codegen::emitObject(llvm::Module* module, const std::string& outputPath) {
    return emitObject(module, outputPath, "", "");
}

bool Codegen::emitObject(llvm::Module* module, const std::string& outputPath,
                         const std::string& mcpu, const std::string& march) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    llvm::Triple targetTriple("x86_64-unknown-linux-gnu");
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    if (!target) {
        std::cerr << error << std::endl;
        return false;
    }

    llvm::TargetOptions opt;
    std::string cpu = mcpu.empty() ? "" : mcpu;
    std::string arch = march.empty() ? "" : march;
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(targetTriple, cpu, arch, opt, llvm::Reloc::PIC_, std::nullopt));
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

void Codegen::optimize(llvm::Module* module, int optLevel,
                       const std::string& mcpu,
                       const std::string& march,
                       const std::string& pgoInstrument,
                       const std::string& pgoProfile) {
  if (!module) return;
  // True O0: no module passes (debug / IR inspection). Runtime bitcode LTO is
  // also skipped by Compiler when optLevel <= 0.
  if (optLevel <= 0) return;

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

  // Determine optimization level
  llvm::OptimizationLevel opt = llvm::OptimizationLevel::O2;
  if (optLevel == 1) opt = llvm::OptimizationLevel::O1;
  else if (optLevel == 2) opt = llvm::OptimizationLevel::O2;
  else if (optLevel >= 3) opt = llvm::OptimizationLevel::O3;

  // Build the base pipeline
  llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(opt);

  // O4+: Add aggressive loop vectorization passes
  // Focus on vectorization over unrolling for SIMD performance
  if (optLevel >= 4) {
    // Create a function pass manager for loop-level optimizations
    llvm::FunctionPassManager FPM = PB.buildFunctionSimplificationPipeline(llvm::OptimizationLevel::O3, llvm::ThinOrFullLTOPhase::None);
    
    // Add aggressive loop vectorizer with interleaving for SIMD
    // Enable all vectorization heuristics
    llvm::LoopVectorizeOptions vecOpts;
    vecOpts.setInterleaveOnlyWhenForced(false);
    vecOpts.setVectorizeOnlyWhenForced(false);
    FPM.addPass(llvm::LoopVectorizePass(vecOpts));
    
    // Add SLP vectorizer for basic-block level vectorization
    FPM.addPass(llvm::SLPVectorizerPass());
    
    // Wrap the function pipeline as a module pass using the adaptor
    MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
  }

  MPM.run(*module, MAM);
}

bool Codegen::emitBitcode(llvm::Module* module, const std::string& outputPath) {
    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "Could not open " << outputPath << ": " << ec.message() << "\n";
        return false;
    }
    llvm::WriteBitcodeToFile(*module, dest);
    dest.flush();
    return true;
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
    return emitAssembly(module, outputPath, "", "");
}

bool Codegen::emitAssembly(llvm::Module* module, const std::string& outputPath,
                           const std::string& mcpu, const std::string& march) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    llvm::Triple targetTriple("x86_64-unknown-linux-gnu");
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    if (!target) {
        std::cerr << error << std::endl;
        return false;
    }

    llvm::TargetOptions opt;
    std::string cpu = mcpu.empty() ? "" : mcpu;
    std::string arch = march.empty() ? "" : march;
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(targetTriple, cpu, arch, opt, llvm::Reloc::PIC_, std::nullopt));
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

std::unique_ptr<llvm::Module> Codegen::mergeModules(
    std::vector<std::unique_ptr<llvm::Module>>& modules,
    llvm::LLVMContext& context,
    const std::string& outputModuleName) {
    
    if (modules.empty()) {
        return nullptr;
    }
    
    // Start with the first module as the base
    auto result = std::move(modules[0]);
    
    // Merge remaining modules into the result
    for (size_t i = 1; i < modules.size(); ++i) {
        if (!modules[i]) continue;
        
        // Use LLVM's Linker to link the next module into result
        // This handles name conflicts by renaming as needed
        llvm::Linker linker(*result);
        if (linker.linkInModule(std::move(modules[i]))) {
            std::cerr << "Error: Failed to link module " << i << std::endl;
            return nullptr;
        }
    }
    
    // Rename the module
    result->setModuleIdentifier(outputModuleName);
    
    return result;
}

bool Codegen::linkRuntimeBitcode(
    llvm::Module* module,
    const std::string& bitcodePath) {
    
    if (bitcodePath.empty()) {
        return true; // not fatal - continue without LTO
    }
    
    // Open and read the bitcode file as a MemoryBuffer
    auto bufferOrError = llvm::MemoryBuffer::getFile(bitcodePath);
    if (!bufferOrError) {
        std::cerr << "Warning: cannot open runtime bitcode: "
                  << bufferOrError.getError().message() << "\n";
        return true; // not fatal
    }
    
    // Parse bitcode from the buffer using LLVM 22 API
    auto result = llvm::parseBitcodeFile((*bufferOrError)->getMemBufferRef(), module->getContext());
    if (!result) {
        std::cerr << "Warning: failed to parse runtime bitcode\n";
        return true; // not fatal
    }
    std::unique_ptr<llvm::Module> rtModule = std::move(*result);
    
    // Set data layout and triple to match the main module
    rtModule->setDataLayout(module->getDataLayout());
    rtModule->setTargetTriple(module->getTargetTriple());
    
    // Link runtime bitcode into the main module using LLVM's linker
    llvm::Linker linker(*module);
    if (linker.linkInModule(std::move(rtModule))) {
        std::cerr << "Warning: linker failed to link runtime bitcode\n";
        return true; // not fatal - continue without LTO
    }
    
    return true;
}

} // namespace pyc
