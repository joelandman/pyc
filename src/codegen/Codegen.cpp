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

    // PyBuiltin_Open(path, mode) -> file dict (2 args).
    llvm::FunctionType* builtinOpenTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(builtinOpenTy, llvm::Function::ExternalLinkage, "PyBuiltin_Open", module.get());

    llvm::FunctionType* printNewlineTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
    llvm::Function::Create(printNewlineTy, llvm::Function::ExternalLinkage, "PyBuiltin_PrintNewline", module.get());

    llvm::FunctionType* pycPrintTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
        {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(pycPrintTy, llvm::Function::ExternalLinkage, "pyc_print", module.get());

    llvm::FunctionType* pycImportFailedTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
    llvm::Function::Create(pycImportFailedTy, llvm::Function::ExternalLinkage, "pyc_import_failed", module.get());

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
    // setjmp is special: declaration with the ReturnsTwice attribute.
    {
        llvm::FunctionType* setjmpTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {int8PtrTy}, false);
        llvm::Function* sj = llvm::Function::Create(setjmpTy, llvm::Function::ExternalLinkage, "setjmp", module.get());
        sj->addFnAttr(llvm::Attribute::ReturnsTwice);
    }

    // Builtins: min/max, list, reversed, enumerate, zip
    for (const char* name : {"PyBuiltin_MinList","PyBuiltin_MaxList",
                              "PyBuiltin_List","PyBuiltin_Reversed",
                              "PyBuiltin_Enumerate"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    for (const char* name : {"PyBuiltin_Min2","PyBuiltin_Max2","PyBuiltin_Zip2"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }

    // Builtins: int, float, abs; string methods; dict/list methods
    // 0-arg builtins: super
    llvm::FunctionType* zeroArgTy = llvm::FunctionType::get(pyObjectPtrTy, {}, false);
    llvm::Function::Create(zeroArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Super", module.get());

    for (const char* name : {"PyBuiltin_Int","PyBuiltin_Float","PyBuiltin_Abs",
                              "PyBuiltin_Ord","PyBuiltin_Chr",
                              "PyBuiltin_Bool","PyBuiltin_Type",
                              "PyBuiltin_Hex","PyBuiltin_Oct","PyBuiltin_Bin",
                              "PyBuiltin_Id","PyBuiltin_Repr",
                              "PyString_Upper","PyString_Lower","PyString_Strip",
                              "PyString_SplitWhitespace","PyDict_Keys","PyDict_Values",
                              "PyDict_Items","PyList_Sort","PyList_Pop"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    // 2-arg builtins: divmod, round, pow
    llvm::FunctionType* twoArgTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Divmod", module.get());
    llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Round", module.get());
    llvm::Function::Create(twoArgTy, llvm::Function::ExternalLinkage, "PyBuiltin_Pow", module.get());

    for (const char* name : {"PyString_Split","PyString_Join","PyBuiltin_IntBase",
                              "PyString_RFind"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    for (const char* name : {"PyString_Find3","PyString_RFind3"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "PyString_RFind4", module.get());
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
                          "PyString_LStrip","PyString_RStrip",
                          "PyList_Reverse","PyList_Copy","PyList_Clear",
                          "PyDict_Copy","PyDict_Clear","PyDict_PopItem"}) {
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

    // re module: PyBuiltin_ReFinditer/ReFindall/ReCompile/ReSearch (2-arg),
    // PyBuiltin_ReMatchGroup (2-arg), PyBuiltin_ReSub (4-arg for count).
    for (const char* n : {"PyBuiltin_ReFinditer","PyBuiltin_ReFindall",
                          "PyBuiltin_ReCompile","PyBuiltin_ReSearch",
                          "PyBuiltin_ReMatchGroup"}) twoArg(n);
    {
        llvm::FunctionType* t4 = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(t4, llvm::Function::ExternalLinkage, "PyBuiltin_ReSub", module.get());
        llvm::FunctionType* t3 = llvm::FunctionType::get(pyObjectPtrTy,
            {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
        llvm::Function::Create(t3, llvm::Function::ExternalLinkage, "PyBuiltin_ReSplit", module.get());
    }

    // Builtins: sum, sorted, any, all; isinstance (2-arg)
    for (const char* name : {"PyBuiltin_Sum","PyBuiltin_Any","PyBuiltin_All"}) {
        llvm::FunctionType* ty = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy}, false);
        llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    // sorted(lst, key) and sorted_with_cmp(lst, cmp) take 2 args.
    llvm::FunctionType* sortedTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(sortedTy, llvm::Function::ExternalLinkage, "PyBuiltin_Sorted", module.get());
    llvm::Function::Create(sortedTy, llvm::Function::ExternalLinkage, "PyBuiltin_SortedWithCmp", module.get());
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

    llvm::FunctionType* dictGetWithDefaultTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(dictGetWithDefaultTy, llvm::Function::ExternalLinkage, "PyDict_GetItemWithDefault", module.get());

    llvm::FunctionType* dictDelItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(dictDelItemTy, llvm::Function::ExternalLinkage, "PyDict_DelItem", module.get());

    // Subscript / membership / power
    llvm::FunctionType* getItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(getItemTy, llvm::Function::ExternalLinkage, "Pyc_GetItem", module.get());

    // B6: Extended attribute lookup (instance dict + class dict fallback)
    llvm::FunctionType* getAttrExtTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(getAttrExtTy, llvm::Function::ExternalLinkage, "PyObject_GetAttrExtended", module.get());

    llvm::FunctionType* setItemTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(setItemTy, llvm::Function::ExternalLinkage, "Pyc_SetItem", module.get());

    llvm::FunctionType* containsTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(containsTy, llvm::Function::ExternalLinkage, "Pyc_Contains", module.get());

    llvm::FunctionType* getSliceTy = llvm::FunctionType::get(pyObjectPtrTy, {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
    llvm::Function::Create(getSliceTy, llvm::Function::ExternalLinkage, "Pyc_GetSlice", module.get());

    llvm::FunctionType* setSliceTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy, pyObjectPtrTy}, false);
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
        
        // A6: Detect specialized variants (name starts with "__specialized_").
        // Format: __specialized_<funcName>_<sig> where sig = "i"/"f" per param.
        // Params: [cell params...] + [original param names].
        // The sig length = number of user params (f.args.size() - f.freeCellVars.size()).
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
                    // User params: native type based on sig position
                    size_t sigIdx = i - ncells;
                    if (sigIdx < sig.size() && sig[sigIdx] == 'f') {
                        argTypes.push_back(llvm::Type::getDoubleTy(context));
                    } else {
                        argTypes.push_back(llvm::Type::getInt64Ty(context));
                    }
                }
            }
        } else {
            argTypes.assign(f.args.size(), pyObjectPtrTy);
        }
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
        // A6: Skip adapter generation for specialized variants.
        // Specialized variants are only called directly from the original function
        // (which boxes arguments), so they don't need indirect dispatch adapters.
        if (f.name.find("__specialized_") == 0) continue;
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

        // Use *original user* param names (with * markers) for user signature.
        // paramNames is always the user-level signature (we never mutate it with cells).
        // It may legitimately be empty (e.g. a nested def with no user parameters).
        const auto& pnames = f.paramNames;  // authoritative user params (may be empty)
        size_t vidx = (size_t)-1;
        for (size_t j = 0; j < pnames.size(); ++j) {
            if (!pnames[j].empty() && pnames[j][0] == '*') { vidx = j; break; }
        }
        bool hasVar = (vidx != (size_t)-1);
        // Number of *user* fixed params in the original signature (cells are not user params).
        size_t userFixedCount = hasVar ? vidx : pnames.size();
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
        size_t userFixed = hasVar ? vidx : origUserParams;

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
        for (size_t i = 0; i < userFixed; ++i) {
            llvm::Value* iV = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), i);
            llvm::Value* have = abuilder.CreateICmpSGT(userLen, iV);
            llvm::BasicBlock* haveB = llvm::BasicBlock::Create(context, "have" + std::to_string(i), adapter);
            llvm::BasicBlock* missB = llvm::BasicBlock::Create(context, "miss" + std::to_string(i), adapter);
            llvm::BasicBlock* after = llvm::BasicBlock::Create(context, "arg" + std::to_string(i), adapter);
            abuilder.CreateCondBr(have, haveB, missB);
            abuilder.SetInsertPoint(haveB);
            llvm::Value* idxHave = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), ncells + i);
            llvm::Value* elHave = nullptr;
            if (listGet) elHave = abuilder.CreateCall(listGet, {argListVal, idxHave}, "a" + std::to_string(i));
            abuilder.CreateBr(after);
            abuilder.SetInsertPoint(missB);
            llvm::Value* elMiss = llvm::ConstantPointerNull::get(pyObjectPtrTy);
            // Always try to load a default for this user position on miss.
            // For top-level defaulted funcs the convention is __default_<name>_<i>
            // for the i-th declared (regular) param when it has a default.
            // We also try the mapped dk and any recorded defSlots.
            {
                std::vector<size_t> cands;
                cands.push_back(i);
                if (ndef > 0 && i >= firstDef) cands.push_back(i - firstDef);
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
            llvm::PHINode* phi = abuilder.CreatePHI(pyObjectPtrTy, 2);
            phi->addIncoming(elHave ? elHave : llvm::ConstantPointerNull::get(pyObjectPtrTy), haveB);
            phi->addIncoming(elMiss, missB);
            cargs.push_back(phi);
        }

        if (hasVar) {
            // Collect [ncells + userFixed .. len) into a fresh list for the * slot.
            llvm::Value* ln = nullptr;
            if (listSize) {
                ln = abuilder.CreateCall(listSize, {argListVal}, "ln");
            } else {
                ln = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
            }
            llvm::Value* startC = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), ncells + userFixed);
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
        if (funcIsSpecialized) {
            nativeParamStart = f.freeCellVars.size();
            if (nativeParamStart < f.args.size() && f.name.size() > nativeParamStart) {
                nativeSig = f.name.substr(f.name.size() - (f.args.size() - nativeParamStart));
            }
        }
        for (size_t i = 0; i < f.args.size(); ++i) {
            llvm::Value* arg = func->getArg(i);
            if (!f.args[i].empty()) {
                arg->setName(f.args[i]);
                // A6: For specialized variants, user params get native-typed allocas.
                llvm::Type* slotType = pyObjectPtrTy;
                if (funcIsSpecialized && i >= nativeParamStart) {
                    size_t sigIdx = i - nativeParamStart;
                    if (sigIdx < nativeSig.size() && nativeSig[sigIdx] == 'f') {
                        slotType = llvm::Type::getDoubleTy(context);
                    } else {
                        slotType = llvm::Type::getInt64Ty(context);
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
        for (const auto& inst : f.body) {
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
            return builder.CreateLoad(llvm::Type::getInt64Ty(context), fieldPtr, "unboxed");
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
            
            if (inst.resultType == "int" || (bothNative && rawL->getType() == llvm::Type::getInt64Ty(context) && rawR->getType() == llvm::Type::getInt64Ty(context))) {
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
            if (inst.resultType == "float" || (bothNative && rawL->getType()->isDoubleTy() && rawR->getType()->isDoubleTy())) {
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
                if (!bothNative) {
                    emitDecRefIfOwned(inst.operands[0].name);
                    emitDecRefIfOwned(inst.operands[1].name);
                }
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

        llvm::BasicBlock* curBlock = entry;
        for (const auto& inst : f.body) {
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
                        cval = builder.CreateICmpNE(unboxed, llvm::ConstantInt::get(context, llvm::APInt(64, 0)));
                    }

                    // DECREF boxed condition temp after extracting truth value.
                    emitDecRefIfOwned(cname);

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
                    // 2) After setjmp has filled the buffer, push the try
                    //    frame and copy the buffer into it. The matching
                    //    pyc_raise will longjmp to the address setjmp recorded.
                    builder.CreateCall(pycTryPush, {jbufPtr, zero});
                    llvm::Value* isExc = builder.CreateICmpNE(rv,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
                    auto nit = blockMap.find(normalL);
                    auto eit = blockMap.find(excL);
                    if (nit != blockMap.end() && eit != blockMap.end()) {
                        builder.CreateCondBr(isExc, eit->second, nit->second);
                    } else if (nit != blockMap.end()) {
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
                    llvm::Value* i32v = builder.CreateZExt(cmpv, llvm::Type::getInt32Ty(context));
                    llvm::Value* bcmp = builder.CreateCall(boolNew, {i32v}, inst.result);
                    valueMap[inst.result] = bcmp;
                    markOwned(inst.result);
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
                }
                valueMap[inst.result] = i64alloca;
                builder.CreateStore(newVal, i64alloca);
            } else if (inst.op == "const") {
                std::string val = inst.operands.empty() ? "0" : inst.operands[0].name;
                if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
                    llvm::Function* fromStr = module->getFunction("PyUnicode_FromString");
                    if (fromStr) {
                        std::string s = val.substr(1, val.size() - 2);
                        llvm::Value* strConst = builder.CreateGlobalStringPtr(s, "str");
                        llvm::Value* boxed = builder.CreateCall(fromStr, {strConst}, inst.result);
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
                
                if (inst.resultType == "int" || bothNativeInt) {
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
                llvm::Function* fn = module->getFunction("Pyc_Pow");
                std::string lhsName = inst.operands.size() > 0 ? inst.operands[0].name : "";
                std::string rhsName = inst.operands.size() > 1 ? inst.operands[1].name : "";
                llvm::Value* lhsRaw = lhsName.empty() ? nullptr : getOrLoad(lhsName);
                llvm::Value* rhsRaw = rhsName.empty() ? nullptr : getOrLoad(rhsName);
                llvm::Value* lhs = getAsPyObject(lhsName);
                llvm::Value* rhs = getAsPyObject(rhsName);
                // Track whether getAsPyObject created an anonymous box for a native value.
                // Native (i64/double) operands are boxed inline by getAsPyObject without
                // entering ownedTemps, so they must be explicitly DECREFed after the call.
                bool lhsWasNative = lhsRaw && (lhsRaw->getType() == llvm::Type::getInt64Ty(context)
                                               || lhsRaw->getType()->isDoubleTy());
                bool rhsWasNative = rhsRaw && (rhsRaw->getType() == llvm::Type::getInt64Ty(context)
                                               || rhsRaw->getType()->isDoubleTy());
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
                
                if (inst.resultType == "int" || bothNativeIntM) {
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
            } else if (inst.op == "assign") {
                // If the source is a Python name AND it's a module global, load
                // the global directly (don't go through valueMap, which may have
                // been polluted by a later 'const' instruction with the same name).
                std::string srcNameAssign = inst.operands.empty() ? "" : inst.operands[0].name;
                llvm::Value* src = nullptr;
                if (!srcNameAssign.empty() && module->getNamedGlobal("pyc_global_" + srcNameAssign)) {
                    llvm::GlobalVariable* gvsrc = module->getNamedGlobal("pyc_global_" + srcNameAssign);
                    src = builder.CreateLoad(pyObjectPtrTy, gvsrc, srcNameAssign + ".gload");
                } else {
                    src = getOrLoad(srcNameAssign);
                }
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(context);
                std::string srcName = srcNameAssign;

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
                 for (const auto& nl : f.numericLocals) {
                     if (nl == inst.result) { isNumericLocal = true; break; }
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

                // Determine ownership of source. Owned temps already have refcount=1.
                bool srcIsOwned = ownedTemps.count(srcName) > 0;
                if (srcIsOwned) ownedTemps.erase(srcName);

                // Box native values. The box call creates a new owned ref.
                llvm::Value* newVal = src;
                if (newVal->getType() == i64Ty) {
                    newVal = boxI64(newVal, srcName + ".boxed");
                    srcIsOwned = true;
                } else if (newVal->getType()->isDoubleTy()) {
                    newVal = boxDouble(newVal, srcName + ".boxed");
                    srcIsOwned = true;
                }

                llvm::Function* incref = module->getFunction("Py_INCREF");
                llvm::Function* decref = module->getFunction("Py_DECREF");

                auto tit = valueMap.find(inst.result);
                if (tit != valueMap.end()) {
                    if (llvm::GlobalVariable* gv = llvm::dyn_cast<llvm::GlobalVariable>(tit->second)) {
                        // Global reassignment: DECREF old value (null-safe), take/transfer ownership.
                        llvm::Value* oldVal = builder.CreateLoad(pyObjectPtrTy, gv, inst.result + ".old");
                        if (decref) builder.CreateCall(decref, {oldVal});
                        if (!srcIsOwned && incref) builder.CreateCall(incref, {newVal});
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
                        if (!srcIsOwned && incref) builder.CreateCall(incref, {newVal});
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
                 // A4: native list subscript get for proven homogeneous lists.
                 if (funcName == "Pyc_GetItem" && inst.operands.size() >= 3) {
                     std::string listName = inst.operands[1].name;
                     std::string idxName = inst.operands[2].name;
                     // Check if result type is known to be int or float (set by lowering).
                     if (inst.resultType == "int" || inst.resultType == "float") {
                         llvm::Value* listVal = getAsPyObject(listName);
                         llvm::Value* idxVal = getOrLoad(idxName);
                         // Ensure index is i64.
                         if (idxVal->getType() != llvm::Type::getInt64Ty(context)) {
                             idxVal = builder.CreateCast(llvm::Instruction::SExt, idxVal,
                                 llvm::Type::getInt64Ty(context), "idx.i64");
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
                             // Box the native value.
                             llvm::Value* boxed = nullptr;
                             if (inst.resultType == "int") {
                                 boxed = boxI64(nativeVal, inst.result + ".boxed");
                             } else {
                                 boxed = boxDouble(nativeVal, inst.result + ".boxed");
                             }
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
                     if (idxVal->getType() != llvm::Type::getInt64Ty(context)) {
                         idxVal = builder.CreateCast(llvm::Instruction::SExt, idxVal,
                             llvm::Type::getInt64Ty(context), "setidx.i64");
                     }
                     llvm::Value* valVal = getOrLoad(valName);
                     if (valVal->getType() != llvm::Type::getInt64Ty(context)) {
                         valVal = builder.CreateCast(llvm::Instruction::SExt, valVal,
                             llvm::Type::getInt64Ty(context), "setval.i64");
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
                     if (idxVal->getType() != llvm::Type::getInt64Ty(context)) {
                         idxVal = builder.CreateCast(llvm::Instruction::SExt, idxVal,
                             llvm::Type::getInt64Ty(context), "setidx.i64");
                     }
                     llvm::Value* valVal = getOrLoad(valName);
                     // Ensure double type.
                     if (valVal->getType() != llvm::Type::getDoubleTy(context)) {
                         valVal = builder.CreateSIToFP(valVal, llvm::Type::getDoubleTy(context), "setval.double");
                     }
                     llvm::Function* setFn = module->getFunction("PyList_SetItemDouble");
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
                    if (callee) {
                        std::vector<llvm::Value*> callArgs;
                        // Track which args were native (i64/double) so that getAsPyObject's
                        // anonymous box can be DECREFed after the call.
                        std::vector<bool> argWasNative;
                        
                        // A6: Check if there's a specialized variant for this call.
                        // If the current function is NOT a specialized variant itself,
                        // and all args are numeric, dispatch to the specialized variant.
                        bool useSpecialized = false;
                        std::string specializedName;
                        if (f.name.find("__specialized_") == std::string::npos) {
                            // Check each possible specialized variant signature
                            size_t numArgs = inst.operands.size() - 1;
                            if (numArgs > 0) {
                                // Check if all args are numeric
                                bool allNumeric = true;
                                std::string sig;
                                for (size_t i = 1; i < inst.operands.size(); ++i) {
                                    llvm::Value* raw = getOrLoad(inst.operands[i].name);
                                    if (raw && raw->getType() == llvm::Type::getInt64Ty(context)) {
                                        sig += "i";
                                    } else if (raw && raw->getType()->isDoubleTy()) {
                                        sig += "f";
                                    } else {
                                        allNumeric = false;
                                        break;
                                    }
                                }
                                if (allNumeric && !sig.empty()) {
                                    std::string candidate = "__specialized_" + funcName + "_" + sig;
                                    llvm::Function* spec = module->getFunction(candidate);
                                    if (spec) {
                                        useSpecialized = true;
                                        specializedName = candidate;
                                        callee = spec;
                                    }
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
                        } else {
                            // Original boxed path.
                            for (size_t i = 1; i < inst.operands.size(); ++i) {
                                llvm::Value* raw = getOrLoad(inst.operands[i].name);
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
                {
                    llvm::Function* slotDecref = module->getFunction("Py_DECREF");
                    if (slotDecref) {
                        for (const auto& slotName : ownedSlots) {
                            auto vit = valueMap.find(slotName);
                            if (vit != valueMap.end()) {
                                if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(vit->second)) {
                                    llvm::Value* slotVal = builder.CreateLoad(
                                        pyObjectPtrTy, alloca, slotName + ".exit");
                                    builder.CreateCall(slotDecref, {slotVal});
                                }
                            }
                        }
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
            llvm::Value* zero = fromLong
                ? (llvm::Value*)builder.CreateCall(fromLong, {llvm::ConstantInt::get(context, llvm::APInt(64, 0))})
                : (llvm::Value*)llvm::ConstantPointerNull::get(pyObjectPtrTy);
            // DECREF all owned slots before the implicit return
            {
                llvm::Function* slotDecref = module->getFunction("Py_DECREF");
                if (slotDecref) {
                    for (const auto& slotName : ownedSlots) {
                        auto vit = valueMap.find(slotName);
                        if (vit != valueMap.end()) {
                            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(vit->second)) {
                                llvm::Value* slotVal = builder.CreateLoad(
                                    pyObjectPtrTy, alloca, slotName + ".exit");
                                builder.CreateCall(slotDecref, {slotVal});
                            }
                        }
                    }
                }
            }
            builder.CreateRet(zero);
        }

        // Small helper to box an i64 value on demand (used by some numeric
        // binary paths and for range loop visible var boxing on escape).
        (void)boxI64; // already defined above; keep reference for future native paths.
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
    return module;
}

bool Codegen::emitObject(llvm::Module* module, const std::string& outputPath) {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();

    llvm::Triple targetTriple("x86_64-unknown-linux-gnu");
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

    llvm::Triple targetTriple("x86_64-unknown-linux-gnu");
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
