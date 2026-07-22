#include "pyc/Compiler.h"
#include "pyc/PythonParser.h"
#include "pyc/IR.h"
#include "pyc/Codegen.h"
#include <llvm/IR/LLVMContext.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <filesystem>
#include <algorithm>

#ifndef PYC_SOURCE_DIR
#define PYC_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;

namespace pyc {

class LoweringVisitor {
 public:
     LoweringVisitor(ModuleIR& moduleIR,
                     const std::unordered_set<std::string>& compiledModules = {},
                     const std::unordered_map<std::string, std::vector<std::string>>& importedModuleGlobals = {})
         : ir(moduleIR), compiledModules(compiledModules), importedModuleGlobals(importedModuleGlobals) {}

    void lower(const ASTNode* node, const std::string& funcName = "") {
        if (!node) return;
        if (!funcName.empty()) currentFunc = funcName;

         if (node->type == "Module") {
             ir.addFunction("__module__", {});
             currentFunc = "__module__";
             tempCounter = 0;
             valueTypes.clear();
             numericLocals.clear();
            complexVars.clear();
            // Pre-scan: collect module bindings and global-declared variable
            // names so top-level assignments are visible from functions.
            collectModuleBindings(node);
            collectGlobalDecls(node);
            ir.setFunctionGlobals("__module__", ir.moduleGlobals);
            listLiteralElemASTs.clear();
            callableTokenTemps.clear();
            listsContainingCallableTokens.clear();
            knownFloatLists.clear();
            knownIntLists.clear();
            dictValueTypes.clear();
            currentFnReturnsCallable = false;
            currentFnReturnType = "boxed";
            funcNonlocals.clear();
            funcCells.clear();
            funcFreeCells.clear();
            funcOwnedCells.clear();
            // Scan all function bodies for yield expressions to detect generators.
            // Must happen before any function lowering so that lowerCall can
            // detect generator calls and wrap them with clear→call→get_buffer.
            scanForGenerators(node);
            // B4: pre-populate knownIRFunctions with all user FunctionDef ids
            // (including nested) so that calls (even forward refs) see them as
            // direct targets. Lambdas are added when their expr is lowered.
            // This keeps ordinary calls direct while still allowing bare-name
            // variables/parameters holding callable tokens to take the Pyc_Apply path.
            // Decorated defs are excluded everywhere below: their python name
            // is a variable bound to the decorator's result, so calls and
            // value references must resolve dynamically, never to the
            // undecorated IR function.
            auto hasDecorators = [](const ASTNode* n) {
                for (const auto& c : n->children)
                    if (c && c->type == "Decorator") return true;
                return false;
            };
            std::function<void(const ASTNode*)> collectDefs = [&](const ASTNode* n) {
                if (!n) return;
                if (n->type == "FunctionDef" && !n->id.empty() && !hasDecorators(n)) {
                    knownIRFunctions.insert(n->id);
                }
                for (const auto& c : n->children) collectDefs(c.get());
            };
            collectDefs(node);
            // First-class defs: top-level def names may be referenced in value
            // position (including forward refs from inside earlier functions).
            // Track them separately from the special builtin shims below so
            // bare-name value uses produce callable tokens (lowerExpr Name).
            for (const auto& c : node->children) {
                if (c && c->type == "FunctionDef" && !c->id.empty() && !hasDecorators(c.get()))
                    userDefFunctions.insert(c->id);
            }
            // Pre-populate our special builtin shims (print, len, range, sum, sorted, min/max,
            // any/all, isinstance, int/float/abs/str, list, enumerate, zip, ...) so bare-name
            // calls to them are recognized as "direct" and never routed through the B4 dynamic
            // Pyc_Apply(token) path. This preserves all the fast/special lowering paths while
            // still giving full lambda-as-value (B4) behavior for user callables.
            for (const char* s : {"print","len","range","min","max","sum","sorted","any","all","isinstance",
                                   "int","float","complex","abs","str","list","reversed","enumerate","zip","bool","type","id",
                                   "repr","hex","oct","bin","ord","chr","round","cmp_to_key","open"}) {
                knownIRFunctions.insert(s);
            }
            for (const auto& c : node->children) {
                lower(c.get());
            }
            
            // B7: Create module dict containing all module globals
            // This dict will be stored in a global variable (e.g., pyc_module_utils)
            // so that importing modules can access it.
            if (!ir.moduleGlobals.empty()) {
                std::string modDict = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyDict_New"}, modDict);
                
                // Add __name__ to the module dict
                std::string nameKey = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"__name__\""}, nameKey, "str");
                std::string nameVal = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + currentFunc + "\""}, nameVal, "str");
                ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, nameKey, nameVal}, "set_name");
                
                // Add each global to the module dict
                for (auto& gname : ir.moduleGlobals) {
                    std::string key = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + gname + "\""}, key, "str");
                    // For known IR functions (user-defined functions), register a string token
                    // pointing to the function name (not null, not the global which is uninitialised).
                    std::string val;
                    auto knownIt = knownIRFunctions.find(gname);
                    if (knownIt != knownIRFunctions.end()) {
                        // Store the function name as a string token
                        val = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + gname + "\""}, val, "str");
                    } else {
                        val = gname;  // Load the global value for regular variables
                    }
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, key, val}, "set_item");
                }
                
                // Store the module dict and return it
                ir.addInstruction(currentFunc, "ret", {modDict});
                
                // B7: Populate stub module dicts for os, sys, subprocess
                if (ir.moduleName == "os") {
                    // os.environ = {}
                    std::string envDict = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyDict_New"}, envDict);
                    std::string envKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"environ\""}, envKey, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, envKey, envDict}, "set_env");
                    
                    // os.path = {exists: fn, isfile: fn, isdir: fn, unlink: fn}
                    std::string pathDict = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyDict_New"}, pathDict);
                    std::string pathKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"path\""}, pathKey, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, pathKey, pathDict}, "set_path");
                    
                    // os.path.exists = Pyc_OsPathExists
                    std::string existsKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"exists\""}, existsKey, "str");
                    std::string existsFn = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_OsPathExists\""}, existsFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", pathDict, existsKey, existsFn}, "set_exists");
                    
                    // os.path.isfile = Pyc_OsPathIsFile
                    std::string isfileKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"isfile\""}, isfileKey, "str");
                    std::string isfileFn = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_OsPathIsFile\""}, isfileFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", pathDict, isfileKey, isfileFn}, "set_isfile");
                    
                    // os.path.isdir = Pyc_OsPathIsDir
                    std::string isdirKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"isdir\""}, isdirKey, "str");
                    std::string isdirFn = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_OsPathIsDir\""}, isdirFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", pathDict, isdirKey, isdirFn}, "set_isdir");
                    
                    // os.unlink = Pyc_OsUnlink
                    std::string unlinkKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"unlink\""}, unlinkKey, "str");
                    std::string unlinkFn = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_OsUnlink\""}, unlinkFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, unlinkKey, unlinkFn}, "set_unlink");
                } else if (ir.moduleName == "sys") {
                    // sys.argv = [] (placeholder)
                    std::string argvKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"argv\""}, argvKey, "str");
                    std::string argvSize = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, argvSize, "int");
                    std::string argvList = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", argvSize}, argvList);
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, argvKey, argvList}, "set_argv");
                } else if (ir.moduleName == "subprocess") {
                    // subprocess.call = Pyc_SubprocessCall
                    std::string callKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"call\""}, callKey, "str");
                    std::string callFn = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_SubprocessCall\""}, callFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, callKey, callFn}, "set_call");
                    
                    // subprocess.check_output = Pyc_SubprocessCheckOutput
                    std::string outKey = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"check_output\""}, outKey, "str");
                    std::string outFn = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_SubprocessCheckOutput\""}, outFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, outKey, outFn}, "set_output");
                }
            }
        } else if (node->type == "FunctionDef") {
            std::string saved = currentFunc;

            // Compute views:
            // - funcParamNames gets the original (starred) view for call-site analysis
            //   and callee-side *args collection.
            // - The IRFunction gets bare names (no leading * or **) so that inside the
            //   function body the parameter names used for allocas/valueMap are normal
            //   identifiers.
            funcParamNames[node->id] = node->args;
            std::vector<std::string> bareParams;
            for (auto& a : node->args) {
                std::string b = a;
                if (!b.empty() && b[0] == '*') {
                    b = b.substr(1);
                    if (!b.empty() && b[0] == '*') b = b.substr(1);
                }
                bareParams.push_back(b);
            }
            // Decorators (synthetic "Decorator" children appended by the parser).
            std::vector<const ASTNode*> decorators;
            for (const auto& c : node->children)
                if (c && c->type == "Decorator" && !c->children.empty()) decorators.push_back(c->children[0].get());

            // Use a unique IR name for nested defs to avoid collisions on source name
            // (e.g. two 'def inner()' in different enclosing functions). Top-level defs
            // keep their Python id. This is required for correct per-function cell metadata,
            // separate bodies, signatures, and token registration.
            // Decorated defs ALWAYS get a synthetic IR name (and no alias): the
            // Python name is bound to the decorator's result, so bare-name calls
            // must resolve dynamically through the variable, never directly to
            // the undecorated IR function.
            std::string defIRName = node->id;
            bool isNestedDef = !currentFunc.empty() && currentFunc != "__module__";
            if (!decorators.empty()) {
                defIRName = "__decorated_" + std::to_string(nestedFuncCounter++);
            } else if (isNestedDef) {
                defIRName = "__nesteddef_" + std::to_string(nestedFuncCounter++);
                // Make bare-name references to this python id inside the enclosing resolve
                // to the unique synthetic (for direct calls and value/bundle construction).
                lambdaAliases[node->id] = defIRName;
            }

            ir.addFunction(defIRName, bareParams);
            knownIRFunctions.insert(defIRName);
            userDefFunctions.insert(defIRName);
            for (auto& fnr : ir.functions) if (fnr.name == defIRName) { fnr.paramNames = node->args; break; }
            for (auto& fnr : ir.functions) if (fnr.name == defIRName) { /* freeCellVars set later */ break; }

            // Switch currentFunc to the (possibly unique) IR name so that all cell analysis,
            // instruction emission (addInstruction), and map keys use the name we registered.
            std::string savedForIR = currentFunc;
            currentFunc = defIRName;

            // Record the python -> IR name mapping for this nested def so that
            // later name lookups and bundle tokens can resolve to the registered name.
            // Not for decorated defs: their python name is a plain variable
            // holding the decorator's result.
            if (decorators.empty()) enclosingToNestedDef[saved].emplace(node->id, defIRName);

            // Collect python-level names of nested FunctionDefs defined directly in this scope.
            // These names are *bindings* in this scope (like locals), not variables closed over
            // from an enclosing scope. When computing free cells for this scope we must never
            // treat a nested def's name as something we need to receive via a hidden cell param.
            std::unordered_set<std::string> nestedDefNamesInThisScope;
            std::function<void(const ASTNode*)> collectNestedDefs = [&](const ASTNode* n) {
                if (!n) return;
                if (n->type == "FunctionDef" && !n->id.empty()) {
                    nestedDefNamesInThisScope.insert(n->id);
                }
                for (const auto& c : n->children) collectNestedDefs(c.get());
            };
            for (const auto& c : node->children) collectNestedDefs(c.get());

            // B5: stash cell metadata on the IRFunction for later codegen (cellVars + freeCellVars).
            // We will also synthesize hidden cell parameters for nested functions that need free cells.
            // (Owned cells and free cells are finalized after the nonlocal/assigned scan below.)
            {
                // cellVars/freeCellVars will be (re)written after the owned/free computation below.
            }

            // Collect this function's global declarations (scan body before lowering).
            std::vector<std::string> funcGlobals = scanFuncGlobals(node);
            for (const auto& g : ir.moduleGlobals) {
                if (std::find(funcGlobals.begin(), funcGlobals.end(), g) == funcGlobals.end())
                    funcGlobals.push_back(g);
            }
            // Remove names that are also parameters (params shadow globals) using bare names.
            for (const auto& bp : bareParams) {
                if (!bp.empty())
                    funcGlobals.erase(std::remove(funcGlobals.begin(), funcGlobals.end(), bp),
                                      funcGlobals.end());
            }
            ir.setFunctionGlobals(defIRName, funcGlobals);

            // Count defaults and collect their values.
            // IMPORTANT: lower the default *expressions* in the definition context (the
            // outer 'saved' scope) so that any temps they allocate (e.g. fconst for 0.0)
            // and the values they produce are valid in the scope where we emit the
            // "assign" into 'saved'. Previously we lowered while currentFunc was the
            // inner function, so Constant defaults (floats especially) produced temps
            // that didn't exist in the outer, resulting in nulls stored to the default
            // globals.
            std::vector<std::string> defaults;
            size_t defaultIndex = 0;
            {
                std::string savedCF = currentFunc;
                currentFunc = saved;
                for (const auto& c : node->children) {
                    if (c && c->type == "Default") {
                        std::string defVal = lowerExpr(c.get());
                        std::string slot = "__default_" + node->id + "_" + std::to_string(defaultIndex++);
                        ir.addModuleGlobal(slot);
                        ir.addInstruction(saved, "assign", {defVal}, slot);
                        defaults.push_back(slot);
                    }
                }
                currentFunc = savedCF;
            }
            if (!defaults.empty()) {
                funcDefaultCount[node->id] = defaults.size();
                funcDefaultValues[node->id] = defaults;
                funcDefaultValues[defIRName] = defaults;
                funcDefaultCount[defIRName] = defaults.size();
                for (auto& fnr : ir.functions) if (fnr.name == defIRName) { fnr.defaultGlobals = defaults; break; }
                for (auto& fnr : ir.functions) if (fnr.name == node->id) { fnr.defaultGlobals = defaults; break; }
            }

            // (The IRFunction was already inserted above with bare param names.
            // We keep funcParamNames with the original starred view for call analysis.)
            // No additional addFunction here; the early-return in IR would have ignored
            // a second insert anyway. We just ensure the starred view is recorded.
            funcParamNames[node->id] = node->args;

            // B5: record this function's nonlocal declarations (declaration-only; cells later).
            funcNonlocals[defIRName] = scanFuncNonlocals(node);

            // B5: determine which names in this function require cell storage.
            // - Any name declared 'nonlocal' here needs a cell (cell lives in an outer scope).
            // - Any name we assign here that is declared 'nonlocal' in some descendant nested
            //   function must be cell-allocated here so the nested function can share it.
            // - Any name that a nested function reads from this scope (implicit closure capture)
            //   must also be a cell, so the inner can access it via the cell.
            {
                std::vector<std::string> cells;
                auto nlit = funcNonlocals.find(defIRName);
                if (nlit != funcNonlocals.end()) {
                    for (const auto& nm : nlit->second) cells.push_back(nm);
                }
                // Look for nested functions and their nonlocal sets; any name assigned here
                // that appears in a nested nonlocal must be a cell.
                std::function<void(const ASTNode*)> scanNestedNonlocals = [&](const ASTNode* n) {
                    if (!n) return;
                    if (n->type == "FunctionDef") {
                        auto innerNL = scanFuncNonlocals(n);
                        auto assigned = scanAssignedNames(node);
                        for (const auto& nm : innerNL) {
                            if (std::find(assigned.begin(), assigned.end(), nm) != assigned.end()) {
                                if (std::find(cells.begin(), cells.end(), nm) == cells.end())
                                    cells.push_back(nm);
                            }
                        }
                    }
                    for (const auto& c : n->children) scanNestedNonlocals(c.get());
                };
                for (const auto& c : node->children) scanNestedNonlocals(c.get());

                // B5 enhancement: also promote to cell any name that is demanded (via nonlocal)
                // anywhere in the nested subtree, *even if this scope does not assign it*.
                // This is required for correct forwarding through intermediate scopes
                // (e.g. outer owns; middle only declares nonlocal and calls inner; inner assigns).
                // The intermediate scope must still treat the name as a cell so it can
                // receive the cell object (hidden param) and pass it down.
                {
                    auto demanded = collectDemandedNonlocals(node);
                    for (const auto& nm : demanded) {
                        if (std::find(cells.begin(), cells.end(), nm) == cells.end())
                            cells.push_back(nm);
                    }
                }

                // B5 (closure capture): any name that a nested function reads from
                // this scope (i.e. the inner body references the name and the
                // name is visible in this scope as a local/param) must be a cell
                // so the inner can capture it. Without this, a `step` local
                // read by the inner via implicit closure capture would not be
                // passed as a hidden cell arg and would resolve to null at the
                // call site.
                {
                    std::function<void(const ASTNode*, std::unordered_set<std::string>&)> walkNames =
                        [&](const ASTNode* n, std::unordered_set<std::string>& out) {
                            if (!n) return;
                            if (n->type == "Name" && !n->id.empty()) out.insert(n->id);
                            for (const auto& c : n->children) walkNames(c.get(), out);
                        };
                    // Collect the names defined in this scope (params + locals).
                    std::unordered_set<std::string> localsHere;
                    for (const auto& p : node->args) {
                        if (!p.empty()) localsHere.insert(p);
                    }
                    std::function<void(const ASTNode*)> collectLocals =
                        [&](const ASTNode* n) {
                            if (!n) return;
                            if (n->type == "Assign" && !n->id.empty()) localsHere.insert(n->id);
                            for (const auto& c : n->children) collectLocals(c.get());
                        };
                    for (const auto& c : node->children) collectLocals(c.get());
                    // Walk nested function bodies; any name they read that is
                    // also a local/param here must be a cell in this scope.
                    std::function<void(const ASTNode*)> walkNested =
                        [&](const ASTNode* n) {
                            if (!n) return;
                            if (n->type == "FunctionDef") {
                                std::unordered_set<std::string> used;
                                walkNames(n, used);
                                for (const auto& nm : used) {
                                    if (localsHere.count(nm)) {
                                        if (std::find(cells.begin(), cells.end(), nm) == cells.end())
                                            cells.push_back(nm);
                                    }
                                }
                                // Don't recurse into the inner function — its
                                // body references its own scope, not ours.
                                return;
                            }
                            for (const auto& c : n->children) walkNested(c.get());
                        };
                    for (const auto& c : node->children) walkNested(c.get());
                }

                // Also, if a nested function declares a nonlocal that we (this scope) assign,
                // we must cell it even if we did not list it in our own nonlocals.
                funcCells[defIRName] = cells;
            }

            // B5: compute freeCellVars for this function (cells we read/write that live in an
            // enclosing function's cell set). This will become hidden leading parameters.
            {
                std::vector<std::string> frees;
                auto nlit = funcNonlocals.find(defIRName);
                if (nlit != funcNonlocals.end()) {
                    // For each name declared nonlocal here, the cell is provided by the nearest
                    // enclosing scope that actually owns/allocates the cell (i.e. assigns it).
                    // We record the Python name; lowering of the enclosing scope will allocate
                    // the cell and pass it down when calling us.
                    for (const auto& nm : nlit->second) {
                        if (std::find(frees.begin(), frees.end(), nm) == frees.end())
                            frees.push_back(nm);
                    }
                }
                // B5 (intermediate-scope forwarding): if this scope forwards a
                // cell to a nested closure (e.g. `def middle(): nonlocal x; def
                // inner(): ...` — middle itself doesn't assign x but inner
                // does, and middle needs to forward x's cell down to inner),
                // treat the demanded name as a free cell here too. This adds
                // a hidden cell parameter to the intermediate scope so the
                // outer (which owns the cell) can pass it down. The closure-
                // detection logic for the BUNDLE then sees the cell in the
                // bundle and forwards it at the call site.
                //
                // However, do NOT add the cell if this scope does not actually
                // read the name (i.e. it only forwards). In that case the
                // cell is sourced from this scope's own cells (or from the
                // outer scope) and the BUNDLE build for the inner function
                // will pick it up directly. Adding it here would incorrectly
                // mark the function as a closure that needs a cell passed in.
                {
                    auto demanded = collectDemandedNonlocals(node);
                    // Determine which of the demanded names this scope
                    // itself references (reads or writes via assignment).
                    // A: do not descend into nested FunctionDef/Lambda; their names
                    // are not 'used here' for deciding whether we need a free cell param.
                    std::function<void(const ASTNode*, std::unordered_set<std::string>&)> walkUsed =
                        [&](const ASTNode* n, std::unordered_set<std::string>& out) {
                            if (!n) return;
                            if (n->type == "FunctionDef" || n->type == "Lambda") {
                                return;  // do not collect names from nested scopes
                            }
                            if (n->type == "Name" && !n->id.empty()) out.insert(n->id);
                            for (const auto& c : n->children) walkUsed(c.get(), out);
                        };
                    std::unordered_set<std::string> usedHere;
                    for (const auto& c : node->children) walkUsed(c.get(), usedHere);
                    for (const auto& nm : demanded) {
                        if (usedHere.count(nm) &&
                            std::find(frees.begin(), frees.end(), nm) == frees.end()) {
                            frees.push_back(nm);
                        }
                    }
                }
                funcFreeCells[defIRName] = frees;
                // A: never treat a name we own/allocate as a free (incoming) cell for ourselves.
                // Ownership: present in funcCells here but not declared nonlocal in this scope.
                {
                    auto nlitOwn = funcNonlocals.find(defIRName);
                    std::unordered_set<std::string> nlHere;
                    if (nlitOwn != funcNonlocals.end()) {
                        for (const auto& nm : nlitOwn->second) nlHere.insert(nm);
                    }
                    auto citOwn = funcCells.find(defIRName);
                    if (citOwn != funcCells.end()) {
                        for (const auto& nm : citOwn->second) {
                            if (nlHere.count(nm) == 0) {
                                frees.erase(std::remove(frees.begin(), frees.end(), nm), frees.end());
                            }
                        }
                    }
                    funcFreeCells[defIRName] = frees;
                }
            // B5: capture *implicit* enclosing reads (no 'nonlocal' decl).
            // Only for nested functions (top-level defs have no enclosing cell scope).
            // A nested may read a name from an enclosing scope without declaring nonlocal
            // (only writes require the declaration). Add any such used name that is not
            // local/param here to frees so the nested receives the cell as a hidden param.
            bool isNestedDefForCells = !saved.empty() && saved != "__module__";
            if (isNestedDefForCells) {
                std::unordered_set<std::string> used;
                std::function<void(const ASTNode*)> walk = [&](const ASTNode* n) {
                    if (!n) return;
                    if (n->type == "FunctionDef" || n->type == "Lambda") return;
                    if (n->type == "Name" && !n->id.empty()) used.insert(n->id);
                    for (const auto& c : n->children) walk(c.get());
                };
                for (const auto& c : node->children) walk(c.get());

                std::unordered_set<std::string> localsHere;
                for (auto& a : node->args) {
                    std::string b = a;
                    if (!b.empty() && b[0] == '*') b = b.substr(1);
                    if (!b.empty() && b[0] == '*') b = b.substr(1);
                    if (!b.empty()) localsHere.insert(b);
                }
                std::function<void(const ASTNode*)> scanAsg = [&](const ASTNode* n) {
                    if (!n) return;
                    if (n->type == "FunctionDef" || n->type == "Lambda") return;
                    if (n->type == "Assign" && !n->id.empty()) localsHere.insert(n->id);
                    if (n->type == "For") {
                        // for-loop targets are locals in this scope
                        if (!n->id.empty() && n->id != "__unpack__") {
                            localsHere.insert(n->id);
                        } else if (!n->children.empty() && n->children[0]) {
                            std::function<void(const ASTNode*)> pat = [&](const ASTNode* p) {
                                if (!p) return;
                                if (p->type == "Name" && !p->id.empty()) localsHere.insert(p->id);
                                if (p->type == "Tuple" || p->type == "List") {
                                    for (auto& ch : p->children) pat(ch.get());
                                }
                            };
                            pat(n->children[0].get());
                        }
                    }
                    for (const auto& c : n->children) scanAsg(c.get());
                };
                for (const auto& c : node->children) scanAsg(c.get());

                // Also count names our nested functions need from beyond this
                // scope (transitively) — this scope must receive and forward
                // those cells even if it never reads the names itself
                // (decorator factories: deco forwards repeat's n to wrapper).
                {
                    std::unordered_set<std::string> nestedNeeds;
                    std::function<void(const ASTNode*)> findNestedTop = [&](const ASTNode* n) {
                        if (!n) return;
                        if (n->type == "FunctionDef" || n->type == "Lambda") {
                            collectTransitiveFreeReads(n, nestedNeeds);
                            return;
                        }
                        for (const auto& c : n->children) findNestedTop(c.get());
                    };
                    for (const auto& c : node->children) findNestedTop(c.get());
                    for (const auto& nm : nestedNeeds) used.insert(nm);
                }
                for (const auto& nm : used) {
                    if (localsHere.count(nm) == 0) {
                        // Never treat a nested def defined in *this* scope as a free cell
                        // from an enclosing scope. Those are bindings created here, not
                        // variables captured from above.
                        if (nestedDefNamesInThisScope.count(nm)) continue;
                        if (std::find(frees.begin(), frees.end(), nm) == frees.end())
                            frees.push_back(nm);
                    }
                }
                funcFreeCells[defIRName] = frees;
            } else {
                // Top-level defs never receive incoming cell params.
                funcFreeCells[defIRName] = {};
            }
            }

            // B5: if this nested function has free cells, synthesize hidden leading parameters
            // so the enclosing scope can pass the cells down. We prefix them to avoid clashing
            // with user parameter names and with the bare-param view used for local allocas.
            {
                auto fit = funcFreeCells.find(defIRName);
                if (fit != funcFreeCells.end() && !fit->second.empty()) {
                    // Prepend synthesized cell parameters to the IRFunction's args.
                    // Use "<pythonname>_cell" as the parameter name so that the uniform
                    // "<name>_cell" slot convention works for both owned cells (locals)
                    // and received free cells (hidden params) inside the nested function.
                    for (auto& fnr : ir.functions) if (fnr.name == defIRName) {
                        fnr.freeCellVars = fit->second;  // Python names of incoming cells (order matters)
                        std::vector<std::string> newArgs;
                        for (const auto& fc : fit->second) {
                            newArgs.push_back(fc + "_cell");
                        }
                        newArgs.insert(newArgs.end(), fnr.args.begin(), fnr.args.end());
                        fnr.args = newArgs;
                        break;
                    }
                    // Also update bareParams we already computed and re-insert function to keep
                    // the moduleIR consistent (addFunction early-returns on duplicate, so we
                    // directly mutate the existing entry). Keep funcParamNames as user view only.
                }
            }

            // B5: decide ownership of cells for this function:
            // - If a name is in *our* cellVars (we allocate it) *and* it is NOT in our nonlocal set,
            //   then we own/allocate the cell here.
            // - Names that are in our nonlocal set are received as hidden __cell_* params.
            {
                std::vector<std::string> owned;
                auto cit = funcCells.find(defIRName);
                auto nlit = funcNonlocals.find(defIRName);
                std::unordered_set<std::string> nlset;
                if (nlit != funcNonlocals.end()) {
                    for (const auto& nm : nlit->second) nlset.insert(nm);
                }
                if (cit != funcCells.end()) {
                    for (const auto& nm : cit->second) {
                        if (nlset.count(nm) == 0) {
                            // We assign it and descendants close over it => we allocate the cell.
                            owned.push_back(nm);
                        }
                    }
                }
                funcOwnedCells[defIRName] = owned;
                // Also annotate the IRFunction for codegen convenience.
                for (auto& fnr : ir.functions) if (fnr.name == defIRName) {
                    fnr.cellVars = cit != funcCells.end() ? cit->second : std::vector<std::string>{};
                    break;
                }
            }

            // B5: detect whether *this* function itself is a closure (captures any cells
            // from an outer scope). If so, mark it so that its "value" (for a def name or
            // a lambda expr) is produced as a descriptor bundle rather than a bare token.
            {
                auto fit = funcFreeCells.find(defIRName);
                if (fit != funcFreeCells.end() && !fit->second.empty()) {
                    closureFunctions.insert(defIRName);
                    closureFunctions.insert(node->id);  // so bare-name mention of python id produces bundle
                }
            }
            // Do not stomp currentFunc back to python name; keep the unique defIRName
            // for the rest of this FunctionDef (cell alloc, body lowering, return recording).
            // currentFunc is already defIRName here.
            int savedTempCounter = tempCounter;
            tempCounter = 0;
            listLiteralElemASTs.clear();
            callableTokenToSynthetic.clear();
            callableTokenTemps.clear();
            listsContainingCallableTokens.clear();
            knownFloatLists.clear();
            knownIntLists.clear();
             currentFnReturnsCallable = false;
             currentFnReturnType = "boxed";
             lastLambdaSynthetic.clear();
            // Save and clear numericLocals for this function scope
            std::unordered_set<std::string> savedNumericLocals = numericLocals;
            // Fresh try-scope state for the nested function's body — its
            // returns must not run the enclosing scope's try exits.
            std::vector<ActiveTry> savedActiveTries = activeTries; activeTries.clear();
            std::vector<size_t> savedLoopTryDepths = loopTryDepths; loopTryDepths.clear();
            numericLocals.clear();
            complexVars.clear();

            // B5: allocate owned cells (for names we assign here that inner scopes close over via nonlocal).
            // Initialize at creation time:
            // - for a parameter closed over: PyCell_New(<the param value>)  -- New INCREFs the content
            // - for a plain local: PyCell_New(0)
            // This avoids a separate PyCell_Set whose result/operand can interact badly with DECREF temps.
            {
                auto oit = funcOwnedCells.find(defIRName);
                if (oit != funcOwnedCells.end()) {
                    for (const auto& nm : oit->second) {
                        std::string cellSlot = nm + "_cell";
                        auto pit = funcParamNames.find(node->id);
                        bool isParam = false;
                        if (pit != funcParamNames.end()) {
                            for (const auto& p : pit->second) {
                                std::string bp = p;
                                if (!bp.empty() && bp[0] == '*') bp = bp.substr(1);
                                if (bp == nm) { isParam = true; break; }
                            }
                        }
                        std::string initial = isParam ? nm : "0";
                        if (!isParam) {
                            std::string z = "c" + std::to_string(tempCounter++);
                            ir.addInstruction(defIRName, "const", {"0"}, z);
                            initial = z;
                        }
                        std::string cellObj = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(defIRName, "call", {"PyCell_New", initial}, cellObj);
                        ir.addInstruction(defIRName, "assign", {cellObj}, cellSlot);
                    }
                }
            }

            // B5 (lambda cells): for each *owned* cell in this scope, if the body of any
            // nested lambda (or nested def) references that name, we must ensure the
            // lambda's synthetic will receive the cell via a hidden param. We do this
            // by walking nested Lambda nodes and adding their referenced owned names
            // into the current function's freeCellVars (so later, when lowering the
            // lambda as a nested FunctionDef, the demanded path will mark it and the
            // lambda will get the hidden cell param from *this* scope).
            {
                std::function<void(const ASTNode*)> markLambdaDemands = [&](const ASTNode* n) {
                    if (!n) return;
                    if (n->type == "Lambda") {
                        const ASTNode* body = nullptr;
                        for (const auto& c : n->children) {
                            if (c && c->type != "Default") { body = c.get(); break; }
                        }
                        if (body) {
                            auto used = collectNames(body);
                            auto oit2 = funcOwnedCells.find(defIRName);
                            if (oit2 != funcOwnedCells.end()) {
                                for (const auto& nm : oit2->second) {
                                    if (used.count(nm)) {
                                        auto& frees = funcFreeCells[defIRName];
                                        if (std::find(frees.begin(), frees.end(), nm) == frees.end())
                                            frees.push_back(nm);
                                    }
                                }
                            }
                        }
                    }
                    for (const auto& c : n->children) markLambdaDemands(c.get());
                };
                for (const auto& c : node->children) markLambdaDemands(c.get());
            }

            // B5 (closure functions): if *this* function captures any cells (freeCellVars),
            // mark it as a closure so that any "value" of it (def name mention or lambda expr)
            // is lowered as a descriptor bundle [token, cell0, cell1, ...] instead of a bare token.
            {
                auto fit = funcFreeCells.find(node->id);
                if (fit != funcFreeCells.end() && !fit->second.empty()) {
                    closureFunctions.insert(node->id);
                }
            }

            for (const auto& c : node->children) {
                if (c && (c->type == "Default" || c->type == "Decorator")) continue;
                lower(c.get());
            }
            // A5: record numericLocals in the IRFunction for codegen.
            for (auto& fnr : ir.functions) if (fnr.name == defIRName) {
                fnr.numericLocals = std::vector<std::string>(numericLocals.begin(), numericLocals.end());
                fnr.numericFloatLocals = std::vector<std::string>(numericFloatLocals.begin(), numericFloatLocals.end());
                break;
            }
            // Restore numericLocals to outer scope
            numericLocals = savedNumericLocals;
            activeTries = savedActiveTries;
            loopTryDepths = savedLoopTryDepths;
            currentFunc = saved;   // restore context for siblings (important for top-level code after defs)
            tempCounter = savedTempCounter;  // restore counter to prevent collisions with module-level temps
            lastLambdaSynthetic.clear();  // do not leak "last lambda expr" from this function to later assigns/calls in outer scope
            // B4: if the function body contained a return of a callable token, record it
            // so that later call results can be treated as tokens (for assign/unpack/call).
            if (currentFnReturnsCallable) {
                functionsThatReturnCallables.insert(defIRName);
                functionsThatReturnCallables.insert(node->id); // for any python-name lookups
            }
            currentFnReturnsCallable = false;
            // B5 (closures): if the function body contained a return of a bundle, record it
            // so callers can mark results as bundles and extract cells at use sites.
            if (currentFnReturnsBundle) {
                functionsThatReturnBundles.insert(defIRName);
                functionsThatReturnBundles.insert(node->id);
                if (!currentReturnedBundleSynthetic.empty()) {
                    functionReturnedBundleSynthetic[defIRName] = currentReturnedBundleSynthetic;
                    functionReturnedBundleSynthetic[node->id] = currentReturnedBundleSynthetic;
                }
                if (!currentReturnedBundleCaps.empty()) {
                    functionReturnedBundleCaps[defIRName] = currentReturnedBundleCaps;
                    functionReturnedBundleCaps[node->id] = currentReturnedBundleCaps;
                }
            }
            currentFnReturnsBundle = false;
            currentReturnedBundleSynthetic.clear();
            currentReturnedBundleCaps.clear();
            // Store return type on the IRFunction for flow-sensitive type analysis
            for (auto& fnr : ir.functions) {
                if (fnr.name == defIRName) {
                    fnr.returnType = currentFnReturnType.empty() ? "boxed" : currentFnReturnType;
                    break;
                }
            }
            currentFnReturnType = "boxed";
            // First-class defs: a def statement binds its name to the function
            // value in the enclosing scope (like `name = lambda ...`). Bind the
            // callable token to the name so later value references share one
            // object — making `g = f; g is f` hold. Direct calls are unaffected
            // (resolved via knownIRFunctions on the AST name). Closure functions
            // are skipped: their value references build descriptor bundles with
            // the enclosing scope's cells at each use site.
            if (!closureFunctions.count(defIRName)) {
                std::string fv = emitFuncValue(defIRName, node->id);
                ir.addInstruction(currentFunc, "assign", {fv}, node->id);
                noteType(node->id, "str");
                // Decorators, bottom-up: name = decoN(...(deco1(name))...).
                // Each application: lower the decorator expression (a Name or a
                // factory Call), then Pyc_Apply it to the current value.
                for (auto it = decorators.rbegin(); it != decorators.rend(); ++it) {
                    std::string dv = lowerExpr(*it);
                    std::string z = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, z);
                    std::string argList = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, argList);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, node->id}, "");
                    std::string decorated = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"Pyc_Apply", dv, argList}, decorated);
                    ir.addInstruction(currentFunc, "assign", {decorated}, node->id);
                }
            } else if (!decorators.empty()) {
                llvm::errs() << "pyc: warning: decorators on closure function '"
                             << node->id << "' are not supported; decorators ignored\n";
            }
            // Do not fall through to the generic FunctionDef handling below.
            return;
       } else if (node->type == "ClassDef") {
             lowerClass(node);
             return;
       } else if (node->type == "If") {
            lowerIf(node);
        } else if (node->type == "While") {
            lowerWhile(node);
        } else if (node->type == "For") {
            lowerFor(node);
        } else if (node->type == "Break") {
            // Exit try scopes entered inside the loop (pops + finallys) first.
            emitTryExits(loopTryDepths.empty() ? activeTries.size() : loopTryDepths.back());
            ir.addInstruction(currentFunc, "br", {}, loopBreakLabel);
        } else if (node->type == "Continue") {
            emitTryExits(loopTryDepths.empty() ? activeTries.size() : loopTryDepths.back());
            ir.addInstruction(currentFunc, "br", {}, loopContinueLabel);
        } else if (node->type == "Global") {
            // Declaration only — already collected in pre-scan, no IR emitted.
            return;
    } else if (node->type == "Nonlocal") {
            // Declaration only — recorded by scanFuncNonlocals during FunctionDef lowering.
            // No IR emitted here; cells + load/store rewrite happen in B5 phases.
            return;
        } else if (node->type == "Delete") {
            // del <target>, ... — each target is a child of this node.
            for (const auto& c : node->children) {
                if (!c) continue;
                lowerDelTarget(c.get());
            }
            return;
        } else if (node->type == "Assert") {
            // assert test, msg — children: [test, msg]
            if (node->children.empty()) return;
            std::string cond = lowerExpr(node->children[0].get());
            std::string failL = "assert_fail_" + std::to_string(tempCounter++);
            std::string endL = "assert_end_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "br", {cond, endL, failL});
            ir.addInstruction(currentFunc, "label", {}, failL);
            // Assert failed — raise AssertionError
            if (node->children.size() >= 2 && node->children[1]) {
                std::string msg = lowerExpr(node->children[1].get());
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_AssertFailure", msg}, "");
            } else {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_AssertFailure"}, "");
            }
            ir.addInstruction(currentFunc, "label", {}, endL);
            return;
        } else if (node->type == "Import") {
            // import sys, import math as m, import a, b, c as cc
            // B7: If the module was compiled, call __module__<name> to get the
            // module dict. Otherwise emit pyc_import_failed to report the error.
            //
            // The original module names are stored in node->id (space-
            // separated for the comma-list case); node->args holds the
            // asname-or-name (the binding target).
            std::vector<std::string> origNames;
            {
                std::stringstream ss(node->id);
                std::string tok;
                while (ss >> tok) origNames.push_back(tok);
            }
            for (size_t i = 0; i < node->args.size(); ++i) {
                const std::string& name = node->args[i];
                const std::string& orig = (i < origNames.size()) ? origNames[i] : name;
                ir.addModuleGlobal(name);
                
                if (compiledModules.count(orig) > 0) {
                    // Module was compiled — call its __module__ function
                    std::string modConst = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + orig + "\""}, modConst, "str");
                    ir.addInstruction(currentFunc, "call", {"pyc_run_module", modConst}, "");
                    
                    std::string dictLoad = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"__module__" + orig}, dictLoad);
                    
                    ir.addInstruction(currentFunc, "assign", {dictLoad}, name);
                    noteType(name, "dict");
                } else {
                    // Module not found — call pyc_import_failed and store None
                    std::string modName = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + orig + "\""}, modName, "str");
                    std::string failResult = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"pyc_import_failed", modName}, failResult);
                    
                    ir.addInstruction(currentFunc, "assign", {failResult}, name);
                }
            }
            return;
        } else if (node->type == "ImportFrom") {
            // from math import sqrt
            // from utils import *
            // B7: If the module was compiled, call __module__<mod> and look up names.
            // Otherwise emit pyc_import_failed.
            const std::string& mod = node->id;
            if (!mod.empty()) {
                if (compiledModules.count(mod) > 0) {
                    // Module was compiled
                    std::string modConst = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + mod + "\""}, modConst, "str");
                    ir.addInstruction(currentFunc, "call", {"pyc_run_module", modConst}, "");

                    std::string moduleDict = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"__module__" + mod}, moduleDict);

                    // Detect `from X import *`. The parser puts the literal "*"
                    // into node->args. We expand to the imported module's known
                    // (non-underscore) globals at lowering time, so each name
                    // becomes a real module global in the main module.
                    std::vector<std::string> exportNames;
                    bool isStar = false;
                    for (const auto& n : node->args) {
                        if (n == "*") { isStar = true; break; }
                    }
                    if (isStar) {
                        auto igit = importedModuleGlobals.find(mod);
                        if (igit != importedModuleGlobals.end()) {
                            for (const auto& nm : igit->second) {
                                if (nm.empty() || nm[0] == '_') continue;
                                exportNames.push_back(nm);
                            }
                        }
                        // If the imported module's globals weren't collected
                        // (e.g. parse failure), fall through with an empty
                        // export list — no `*` global is created.
                    } else {
                        exportNames = node->args;
                    }

                    for (const auto& name : exportNames) {
                        ir.addModuleGlobal(name);
                        std::string attrKey = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + name + "\""}, attrKey, "str");
                        std::string attrVal = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", moduleDict, attrKey}, attrVal);
                        ir.addInstruction(currentFunc, "assign", {attrVal}, name);
                    }
                } else {
                    // Module not found — call pyc_import_failed for each imported name
                    std::string modName = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + mod + "\""}, modName, "str");
                    for (const auto& name : node->args) {
                        if (name == "*") continue;  // nothing to bind for star on a missing module
                        ir.addModuleGlobal(name);
                        std::string failResult = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"pyc_import_failed", modName}, failResult);
                        ir.addInstruction(currentFunc, "assign", {failResult}, name);
                    }
                }
            }
            return;
        } else if (node->type == "AugAssign") {
            lowerAugAssign(node);
        } else if (node->type == "Assign") {
            lowerAssign(node);
        } else if (node->type == "Return") {
            lowerReturn(node);
        } else if (node->type == "Raise") {
            if (node->children.empty() || !node->children[0]) {
                // bare `raise` — re-raise the active exception
                ir.addInstruction(currentFunc, "call", {"pyc_reraise"}, "");
            } else if (node->children[0]->type == "Name" &&
                       builtinExcNames().count(node->children[0]->id)) {
                // `raise ValueError` — construct with no message
                std::string nameConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + node->children[0]->id + "\""}, nameConst, "str");
                std::string emptyMsg = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"\""}, emptyMsg, "str");
                std::string exc = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"pyc_make_exc", nameConst, emptyMsg}, exc);
                ir.addInstruction(currentFunc, "call", {"pyc_raise", exc}, "");
            } else {
                // `raise <expr>` — exception constructors (ValueError("x")) are
                // handled by lowerCall's builtin-exception special case.
                std::string exc = lowerExpr(node->children[0].get());
                ir.addInstruction(currentFunc, "call", {"pyc_raise", exc}, "");
            }
        } else if (node->type == "Try") {
            lowerTry(node);
        } else if (node->type == "With") {
            // with context_expr as var: body
            // Simplified: evaluate context_expr, call __enter__, bind result, execute body, call __exit__
            if (node->children.empty()) return;
            const ASTNode* withItem = node->children[0].get();
            if (!withItem || withItem->children.empty()) return;
            // children[0] = context_expr, children[1] = optional_vars (if any)
            std::string ctxExpr = lowerExpr(withItem->children[0].get());
            // Call __enter__ on the context manager
            // Get the __enter__ method using Pyc_GetItem (supports class dict fallback)
            std::string enterMethod = "t" + std::to_string(tempCounter++);
            std::string enterMethodToken = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"__enter__\""}, enterMethodToken, "str");
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", ctxExpr, enterMethodToken}, enterMethod);
            // Build args list: [self]
            std::string enterArgs = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", "1"}, enterArgs);
            std::string enterIdx = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, enterIdx);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", enterArgs, enterIdx, ctxExpr}, "");
            // Call __enter__(self) via Pyc_Apply
            std::string enterResult = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_Apply", enterMethod, enterArgs}, enterResult);
            // Bind to target variable if present
            if (withItem->children.size() >= 2 && withItem->children[1]) {
                std::string targetName = withItem->children[1]->id;
                ir.addInstruction(currentFunc, "assign", {enterResult}, targetName);
            }
            // Execute body
            for (size_t i = 1; i < node->children.size(); ++i) {
                if (node->children[i]) lower(node->children[i].get());
            }
            // Call __exit__ with None, None, None (simplified)
            std::string exitMethod = "t" + std::to_string(tempCounter++);
            std::string exitMethodToken = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"__exit__\""}, exitMethodToken, "str");
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", ctxExpr, exitMethodToken}, exitMethod);
            std::string exitArgs = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", "4"}, exitArgs);
            std::string exitIdx = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, exitIdx);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", exitArgs, exitIdx, ctxExpr}, "");
            std::string noneVal = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "nconst", {}, noneVal);
            for (int i = 1; i < 4; ++i) {
                std::string idx = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idx);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", exitArgs, idx, noneVal}, "");
            }
            ir.addInstruction(currentFunc, "call", {"Pyc_Apply", exitMethod, exitArgs}, "");
        } else if (node->type == "Match") {
            // match subject: cases...
            // Lower to a chain of if/elif/else.  The codegen skips non-label
            // instructions when the current block is terminated, so we must
            // place each case's condition+br AFTER a label.  Structure:
            //   <entry> → label check_1 → icmp → br(cond, body, check_2)
            //             label body_1 → body → br end
            //             label check_2 → icmp → br(cond, body, check_3)
            //             label body_2 → body → br end
            //             …
            //             label end
            if (node->children.empty()) return;
            std::string subject = lowerExpr(node->children[0].get());

            int matchEndCounter = tempCounter++;
            std::string matchEnd = "match_end_" + std::to_string(matchEndCounter);
            std::vector<int> checkLabelCounters;
            for (size_t i = 1; i < node->children.size(); ++i) {
                checkLabelCounters.push_back(tempCounter++);
            }

            for (size_t i = 1; i < node->children.size(); ++i) {
                const ASTNode* caseNode = node->children[i].get();
                if (!caseNode || caseNode->children.empty()) continue;
                const ASTNode* pattern = caseNode->children[0].get();
                if (!pattern) continue;

                bool isWildcard = (pattern->type == "MatchWildcard");
                bool isMatchAs = (pattern->type == "MatchAs");
                bool hasBinding = isMatchAs && !pattern->value.empty();

                // Emit label for this case's check point
                std::string checkLabel = "check_" + std::to_string(i) + "_" + std::to_string(checkLabelCounters[i - 1]);
                ir.addInstruction(currentFunc, "label", {}, checkLabel);

                std::string matchCond;
                if (isWildcard || isMatchAs) {
                    std::string trueConst = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "bconst", {"True"}, trueConst, "bool");
                    matchCond = trueConst;
                    if (hasBinding) {
                        ir.addInstruction(currentFunc, "assign", {subject}, pattern->value);
                    }
                } else if (pattern->type == "MatchValue") {
                    if (pattern->children.empty()) continue;
                    std::string patternVal = lowerExpr(pattern->children[0].get());
                    if (patternVal.empty()) continue;
                    std::string cmpResult = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "icmp", {"Eq", subject, patternVal}, cmpResult, "bool");
                    matchCond = cmpResult;
                } else if (pattern->type == "MatchSingleton") {
                    std::string cmpResult = "t" + std::to_string(tempCounter++);
                    if (pattern->value == "None") {
                        std::string noneConst = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "nconst", {}, noneConst);
                        ir.addInstruction(currentFunc, "icmp", {"Eq", subject, noneConst}, cmpResult, "bool");
                        matchCond = cmpResult;
                    } else {
                        std::string boolVal = pattern->value == "True" ? "True" : "False";
                        std::string boolConst = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "bconst", {boolVal}, boolConst, "bool");
                        ir.addInstruction(currentFunc, "icmp", {"Eq", subject, boolConst}, cmpResult, "bool");
                        matchCond = cmpResult;
                    }
                } else if (pattern->type == "Name") {
                    std::string targetName = pattern->id;
                    ir.addInstruction(currentFunc, "assign", {subject}, targetName);
                    for (size_t j = 1; j < caseNode->children.size(); ++j) {
                        if (caseNode->children[j]) lower(caseNode->children[j].get());
                    }
                    ir.addInstruction(currentFunc, "br", {}, matchEnd);
                    continue;
                } else {
                    matchCond = "1";
                }

                std::string caseBody = "case_body_" + std::to_string(i) + "_" + std::to_string(checkLabelCounters[i - 1]);
                std::string nextCase;
                if (i + 1 < node->children.size()) {
                    nextCase = "check_" + std::to_string(i + 1) + "_" + std::to_string(checkLabelCounters[i]);
                } else {
                    nextCase = matchEnd;
                }

                ir.addInstruction(currentFunc, "br", {matchCond, caseBody, nextCase}, "");

                ir.addInstruction(currentFunc, "label", {}, caseBody);
                size_t bodyStart = 1;
                if (caseNode->children.size() > 1 && caseNode->children[1]->type == "MatchGuard") {
                    bodyStart = 2;
                }
                for (size_t j = bodyStart; j < caseNode->children.size(); ++j) {
                    if (caseNode->children[j]) lower(caseNode->children[j].get());
                }
                ir.addInstruction(currentFunc, "br", {}, matchEnd);
            }

            ir.addInstruction(currentFunc, "label", {}, matchEnd);
        } else if (node->type == "Expr") {
            if (!node->children.empty() && node->children[0]) {
                lowerExpr(node->children[0].get());
            }
        } else if (node->type == "ListComp") {
            lowerListComp(node);
        } else if (node->type == "DictComp") {
            lowerDictComp(node);
        } else {
            // expressions or fallthrough
            lowerExpr(node);
            for (const auto& c : node->children) {
                if (c) lower(c.get());
            }
        }
    }

    std::string lowerExpr(const ASTNode* node) {
        if (!node || currentFunc.empty()) return "";
        if (node->type == "FunctionDef") return "";

        if (node->type == "Constant") {
            std::string res = "c" + std::to_string(tempCounter++);
            std::string val = node->value;
            if (node->is_bool) {
                ir.addInstruction(currentFunc, "bconst", {val}, res, "bool");
                noteType(res, "bool");
            } else if (node->is_float) {
                ir.addInstruction(currentFunc, "fconst", {val}, res, "float");
                noteType(res, "float");
            } else if (node->is_complex) {
                // Complex literals are always boxed — emit pyc_make_complex(real, imag).
                // Parse the string value like "0+1j" or "3+4j" or "0+0j"
                std::string val = node->value;
                double real = 0.0, imag = 0.0;
                // Find the 'j' position
                size_t jpos = val.find('j');
                if (jpos != std::string::npos) {
                    std::string imag_str = val.substr(0, jpos);
                    try { imag = std::stod(imag_str); } catch (...) { imag = 0.0; }
                    // Check if there's a real part (look for + or - before the imag part)
                    if (jpos > 0) {
                        // Find the separator (+ or -) between real and imag
                        for (size_t i = jpos - 1; i > 0; --i) {
                            if (val[i] == '+' || val[i] == '-') {
                                std::string real_str = val.substr(0, i);
                                try { real = std::stod(real_str); } catch (...) { real = 0.0; }
                                break;
                            }
                        }
                    }
                }
                // Emit native doubles directly (PyComplex_New expects double args)
                std::string realConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "fconst", {std::to_string(real)}, realConst, "float");
                std::string imagConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "fconst", {std::to_string(imag)}, imagConst, "float");
                std::string complexRes = "t" + std::to_string(tempCounter++);
                // Use a special IR instruction that passes native doubles
                ir.addInstruction(currentFunc, "call", {"PyComplex_New", realConst, imagConst}, complexRes);
                complexVars.insert(complexRes);
                noteType(res, "boxed");
                return complexRes;
            } else if (node->is_str) {
                // Wrap in quotes so codegen detects it as a string.
                // Embedded quotes are not escaped in this MVP.
                ir.addInstruction(currentFunc, "const", {"\"" + val + "\""}, res, "str");
                noteType(res, "str");
            } else if (node->is_none) {
                // CPython's None is a singleton represented in the runtime as a
                // null pointer. Emit a dedicated nconst op so codegen produces a
                // real PyObject* null constant (and not a PyUnicode "None").
                ir.addInstruction(currentFunc, "nconst", {}, res, "none");
                noteType(res, "none");
            } else {
                ir.addInstruction(currentFunc, "i64const", {val}, res, "i64");
                noteType(res, "i64");
            }
            return res;
        } else if (node->type == "Name") {
            // B5: if this bare name is a cell-backed name in the current function, emit
            // a PyCell_Get to obtain the value for expression use. The result is a fresh
            // PyObject* (new reference) which is safe for the expression context.
            // Use the full isCellBackedHere (checks cells/owned/free) so that pure
            // received free cells (from enclosing via implicit or nonlocal) route through
            // the cell slot instead of resolving as a bare local (which would be null).
            if (isCellBackedHere(node->id)) {
                std::string cellSlot = node->id + "_cell";
                std::string res = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyCell_Get", cellSlot}, res);
                noteType(res, "boxed");
                return res;
            }
            // B5: if this name is a closure function (one that captures free
            // cells from this scope), build a descriptor bundle so callers
            // can extract the cells and pass them down. The bundle is a
            // PyList: [token, cell0, cell1, ...] where cells are this
            // scope's owned cells that the closure reads/writes via
            // nonlocal. The synthetic name is the function's IR name.
            // Resolve via lambdaAliases so that nested defs (which are
            // registered under a unique __nesteddef_N) are looked up by
            // their IR name for cells, token, and bundle metadata.
            {
                std::string eff = node->id;
                auto ait = lambdaAliases.find(node->id);
                if (ait != lambdaAliases.end()) eff = ait->second;
                if (closureFunctions.count(eff)) {
                    std::vector<std::string> caps;
                    auto fit = funcFreeCells.find(eff);
                    if (fit != funcFreeCells.end()) {
                        for (const auto& nm : fit->second) {
                            caps.push_back(nm);
                        }
                    }
                    if (!caps.empty()) {
                        std::string zero = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"0"}, zero);
                        std::string lst = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, lst);

                    std::string tok = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + eff + "\""}, tok, "str");
                    // Do not capture the Append result. Append returns the receiver (borrowed).
                    // Capturing it creates a temp we would markOwned, leading to over-DECREF of
                    // the bundle list on function exit. The item (token/cell) is INCREFed inside Append.
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", lst, tok}, "");

                    for (const auto& nm : caps) {
                        std::string cellSlot = nm + "_cell";
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", lst, cellSlot}, "");
                    }

                        descriptorCells[lst] = caps;
                        bundleToSynthetic[lst] = eff;
                        bundleTemps.insert(lst);
                        return lst;
                    }
                }
                // First-class use of a named def in value position: produce a
                // function object wrapping its callable token so it can be
                // assigned, passed as an argument, stored in containers,
                // returned, called indirectly via Pyc_Apply, and printed as
                // <function name at ...>. Skip names shadowed by a local
                // binding (parameter or prior assignment).
                if (userDefFunctions.count(eff) && !isShadowedLocal(node->id)) {
                    std::string res = emitFuncValue(eff, node->id);
                    callableTokenToSynthetic[res] = eff;
                    callableTokenTemps.insert(res);
                    return res;
                }
            }
            // B13: Builtin exception classes as first-class values.
            // `exc = ValueError` produces a type-12 callable that constructs
            // exceptions via pyc_make_exc when invoked.
            if (builtinExcNames().count(node->id) &&
                !knownClasses.count(node->id) &&
                !isShadowedLocal(node->id)) {
                std::string excNameConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + node->id + "\""}, excNameConst, "str");
                std::string excClass = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"pyc_make_exc_class", excNameConst}, excClass);
                return excClass;
            }
            return node->id;
        } else if (node->type == "Attribute") {
            return lowerAttribute(node);
        } else if (node->type == "BinOp") {
            return lowerBinOp(node);
        } else if (node->type == "Call") {
            return lowerCall(node);
        } else if (node->type == "Compare") {
            return lowerCompare(node);
        } else if (node->type == "List" || node->type == "Tuple") {
            return lowerList(node);
        } else if (node->type == "Dict") {
            return lowerDict(node);
        } else if (node->type == "Default") {
            // Defaults are handled specially in FunctionDef lowering
            return lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        } else if (node->type == "Return") {
            return lowerReturnExpr(node);
        } else if (node->type == "JoinedStr") {
            return lowerJoinedStr(node);
        } else if (node->type == "FormattedValue") {
            return lowerFormattedValue(node);
        } else if (node->type == "BoolOp") {
            return lowerBoolOp(node);
        } else if (node->type == "UnaryOp") {
            return lowerUnaryOp(node);
        } else if (node->type == "Lambda") {
            std::string lamName = lowerLambda(node);
            // B5 (lambda as closure): if this lambda closes over any cells from the definition
            // scope, its *value* must be a descriptor bundle [token, cell0, cell1, ...] so that
            // call sites can extract the cells and pass them as leading args to the synthetic.
            {
                const ASTNode* body = nullptr;
                for (const auto& c : node->children) {
                    if (c && c->type != "Default") { body = c.get(); break; }
                }
                std::vector<std::string> caps;
                if (body) {
                    auto used = collectNames(body);
                    auto oit = funcOwnedCells.find(currentFunc);
                    if (oit != funcOwnedCells.end()) {
                        for (const auto& nm : oit->second) if (used.count(nm)) caps.push_back(nm);
                    }
                    auto fit = funcFreeCells.find(currentFunc);
                    if (fit != funcFreeCells.end()) {
                        for (const auto& nm : fit->second) if (used.count(nm)) {
                            if (std::find(caps.begin(), caps.end(), nm) == caps.end()) caps.push_back(nm);
                        }
                    }
                }
                bool hasDefaultsForLam = funcDefaultValues.count(lamName) && !funcDefaultValues[lamName].empty();
                if (!caps.empty() || hasDefaultsForLam) {
                    // Build a list: [ tokenString, cell0, cell1, ..., preboundDefault0, ... ]
                    std::string zero = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, zero);
                    std::string lst = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, lst);

                    // token (synthetic name string)
                    std::string tok = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + lamName + "\""}, tok, "str");
                    // Do not capture Append result. It returns the receiver (borrowed).
                    // Capturing would mark an alias owned and cause over-DECREF of the bundle list.
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", lst, tok}, "");

                    // cells in definition scope order (use the uniform <nm>_cell slots)
                    for (const auto& nm : caps) {
                        std::string cellSlot = nm + "_cell";
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", lst, cellSlot}, "");
                    }

                    // Prebound default values (evaluated in definition scope) for trailing defaults.
                    size_t prebound = 0;
                    auto dit = funcDefaultValues.find(lamName);
                    if (dit != funcDefaultValues.end()) {
                        for (const auto& slot : dit->second) {
                            ir.addInstruction(currentFunc, "call", {"PyList_Append", lst, slot}, "");
                            ++prebound;
                        }
                    }
                    if (prebound) bundlePreboundArgCount[lst] = prebound;

                    // Remember mapping for call-site extraction and token propagation.
                    descriptorCells[lst] = caps;
                    bundleToSynthetic[lst] = lamName;
                    bundleTemps.insert(lst);

                    // Also keep the B4 token path working for direct resolution inside this scope.
                    callableTokenToSynthetic[lst] = lamName;
                    callableTokenTemps.insert(lamName);

                    lastLambdaSynthetic = lamName;
                    closureFunctions.insert(lamName);
                    return lst;
                }
            }

            // Non-capturing path (original B4): function object wrapping the token.
            std::string res = emitFuncValue(lamName, "<lambda>");
            callableTokenToSynthetic[res] = lamName;
            callableTokenTemps.insert(res);
            lastLambdaSynthetic = lamName;
            return res;
        } else if (node->type == "Starred") {
            // In expression context (e.g. inside a list or other), lower the
            // starred value as-is (the iterable). Call-site collection is
            // handled in lowerCall.
            if (!node->children.empty()) return lowerExpr(node->children[0].get());
            return "";
        } else if (node->type == "ListComp" || node->type == "GeneratorExp") {
            // Both list comprehensions and generator expressions are
            // lowered to an eager list. CPython's genexpr is lazy, but
            // for the patterns pyc supports (str.join, list(), for-loops)
            // the result is the same — callers iterate the result once.
            return lowerListComp(node);
        } else if (node->type == "DictComp") {
            return lowerDictComp(node);
        } else if (node->type == "Subscript") {
            return lowerSubscriptGet(node);
        } else if (node->type == "IfExp") {
            return lowerIfExpr(node);
        } else if (node->type == "NamedExpr") {
            // (x := y) — evaluate y, assign to x, return y
            if (node->children.empty()) return "";
            std::string value = lowerExpr(node->children[0].get());
            if (node->args.empty()) return value;
            // Assign to the target name
            ir.addInstruction(currentFunc, "assign", {value}, node->args[0]);
            noteType(node->args[0], typeOf(value));
            return value;
        } else if (node->type == "YieldExpr") {
            // yield / yield from — emit pyc_yield_collect call
            return lowerYield(node);
        }
        return "";
    }

    std::string lowerYield(const ASTNode* node) {
        // node is a YieldExpr with optional value and is_yield_from flag
        if (!node || node->type != "YieldExpr") {
            return "";
        }
        bool is_yield_from = false;
        // Check if this is a yield from by looking at the node's args
        // (the Python C API stores is_yield_from in the node)
        if (!node->args.empty() && node->args[0] == "1") {
            is_yield_from = true;
        }
        std::string result = "t" + std::to_string(tempCounter++);
        if (is_yield_from && !node->children.empty()) {
            // yield from subgen(): directly call subgen (no generator wrapper)
            // and iterate its result list, yielding each element.
            // We emit the call directly to avoid the generator wrapper,
            // since yield from handles the iteration itself.
            // The subgen's yields go directly into the current buffer.
            std::string subgenVal;
            // Extract the function name from the call expression
            std::string funcName;
            if (!node->children[0]->children.empty() && 
                node->children[0]->children[0]->type == "Name") {
                funcName = node->children[0]->children[0]->id;
            }
            // Emit direct call to subgen (no wrapper)
            std::string callRes = "t" + std::to_string(tempCounter++);
            std::vector<std::string> callOps;
            callOps.push_back(funcName);
            ir.addInstruction(currentFunc, "call", callOps, callRes);
            subgenVal = callRes;
            // Store subgen result in a slot to keep it alive through the loop
            std::string subgenSlot = "__yfrom_subgen_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {subgenVal}, subgenSlot);
            subgenVal = subgenSlot;
            // Use boxed index for comparison and iteration
            std::string idxVar = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxVar);
            std::string loopLabel = "yfrom_loop_" + std::to_string(tempCounter);
            std::string bodyLabel = "yfrom_body_" + std::to_string(tempCounter);
            std::string exitLabel = "yfrom_exit_" + std::to_string(tempCounter);
            tempCounter++;
            ir.addInstruction(currentFunc, "label", {}, loopLabel);
            // len = PyList_SizeBoxed(subgenVal) returns boxed int
            std::string lenRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", subgenVal}, lenRes);
            std::string lenSlot = "__sl_yfrom_" + std::to_string(tempCounter);
            ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlot);
            std::string cmpRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", idxVar, lenSlot}, cmpRes);
            ir.addInstruction(currentFunc, "br", {cmpRes, bodyLabel, exitLabel});
            ir.addInstruction(currentFunc, "label", {}, bodyLabel);
            std::string elemRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", subgenVal, idxVar}, elemRes);
            std::string yld = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"pyc_yield_collect", elemRes}, yld);
            std::string oneRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, oneRes);
            std::string nextIdx = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "add", {idxVar, oneRes}, nextIdx);
            ir.addInstruction(currentFunc, "assign", {nextIdx}, idxVar);
            ir.addInstruction(currentFunc, "br", {}, loopLabel);
            ir.addInstruction(currentFunc, "label", {}, exitLabel);
            return yld;
        }
        // regular yield
        if (!node->children.empty()) {
            std::string val = lowerExpr(node->children[0].get());
            ir.addInstruction(currentFunc, "call", {"pyc_yield_collect", val}, result);
        } else {
            ir.addInstruction(currentFunc, "call", {"pyc_yield_collect"}, result);
        }
        return result;
    }

    // A6: Generate specialized function variants based on call-site type info.
    // For each function that is called with all-proven-numeric arguments at all
    // call sites (with consistent arg count matching declared params), create
    // a variant that takes i64/double directly instead of PyObject*.
    void generateSpecializedVariants() {
        for (auto& kv : callSiteTypes) {
            const std::string& funcName = kv.first;
            const std::vector<std::vector<std::string>>& allSigs = kv.second;
            if (allSigs.empty()) continue;

            // Find the original function
            IRFunction* origFunc = nullptr;
            for (auto& f : ir.functions) {
                if (f.name == funcName) { origFunc = &f; break; }
            }
            if (!origFunc) continue;

            size_t declaredArgCount = origFunc->args.size();
            if (declaredArgCount == 0) continue;

            // Check that all call sites have the same arg count as declared params.
            // This ensures defaults are fully supplied at every call site.
            size_t observedArgCount = allSigs[0].size();
            if (observedArgCount != declaredArgCount) continue;
            for (const auto& sig : allSigs) {
                if (sig.size() != declaredArgCount) {
                    // Mixed arg counts — can't generate a single consistent variant.
                    continue;
                }
            }

            // Check if all args across ALL call sites are proven numeric (int or float).
            // For specialization, every call site must use numeric types at every position.
            bool allNumeric = true;
            std::string sig; // "i" for int, "f" for float
            for (size_t i = 0; i < declaredArgCount; ++i) {
                // Collect all types seen at position i across all call sites.
                std::unordered_set<std::string> typesAtPos;
                for (const auto& s : allSigs) {
                    if (i < s.size()) typesAtPos.insert(s[i]);
                }
                if (typesAtPos.count("int") > 0 || typesAtPos.count("i64") > 0) {
                    sig += "i";
                } else if (typesAtPos.count("float") > 0) {
                    sig += "f";
                } else {
                    allNumeric = false;
                    break;
                }
            }
            if (!allNumeric || sig.empty()) continue;

            // Check if we already have this variant
            std::string variantName = "__specialized_" + funcName + "_" + sig;
            bool alreadyExists = false;
            for (const auto& f : ir.functions) {
                if (f.name == variantName) { alreadyExists = true; break; }
            }
            if (alreadyExists) continue;

            // Create the variant
            IRFunction variant;
            variant.name = variantName;

            // Build parameter list: cells (if any) + original param names
            // (codegen detects native types from variant name prefix)
            for (const auto& cell : origFunc->freeCellVars) {
                variant.args.push_back(cell + "_cell");
            }
            for (size_t i = 0; i < origFunc->args.size(); ++i) {
                variant.args.push_back(origFunc->args[i]);
            }
            variant.paramNames = origFunc->paramNames;
            variant.defaultGlobals = origFunc->defaultGlobals;
            variant.cellVars = origFunc->cellVars;
            variant.freeCellVars = origFunc->freeCellVars;

            // Copy instructions — variants have the same body as original.
            // Codegen uses the variant name to allocate native param slots.
            variant.body = origFunc->body;

            ir.functions.push_back(variant);
        }
    }

    // Type stability tracking: Infer and propagate container element types.
    // Once a type is inferred from a source (literal, container, param default),
    // track it through all assign/unpack operations. A variable's type is stable
    // if it consistently receives compatible values throughout its lifetime.
    void inferContainerElementTypes() {
        // ============== Phase 1: Module-level type registry ==============
        // Track temps from knownFloatLists/knownIntLists and build a map from
        // module global names to their container types.
        
        // Map: global name → container element type per index
        std::unordered_map<std::string, std::unordered_map<size_t, std::string>> globalElementTypes;
        // Map: temp → container type ("float_list", "int_list", "boxed")
        std::unordered_map<std::string, std::string> tempContainerType;
        
        // Track known float/int list temps
        std::unordered_set<std::string> knownFloatTemps(knownFloatLists.begin(), knownFloatLists.end());
        std::unordered_set<std::string> knownIntTemps(knownIntLists.begin(), knownIntLists.end());
        
        // Module-level name → temp mapping (from getglobal → result)
        std::unordered_map<std::string, std::string> globalToTemp;
        
        for (auto& fn : ir.functions) {
            if (fn.name != currentFunc) continue; // module scope only
            
            for (auto& inst : fn.body) {
                if (inst.op == "getglobal" && !inst.operands.empty() && !inst.result.empty()) {
                    globalToTemp[inst.operands[0].name] = inst.result;
                }
            }
        }
        
        // For temps created by lowerList with all float/int elements, mark them
        for (const auto& temp : knownFloatTemps) {
            tempContainerType[temp] = "float_list";
            for (size_t i = 0; i <= 20; i++) {
                globalElementTypes[temp][i] = "float";
            }
        }
        for (const auto& temp : knownIntTemps) {
            tempContainerType[temp] = "int_list";
            for (size_t i = 0; i <= 20; i++) {
                globalElementTypes[temp][i] = "int";
            }
        }
        // Also propagate per-index element types from listElementTypes (mixed-type containers)
        for (auto& fnx : ir.functions) {
            for (const auto& [cname, elemTypes] : fnx.listElementTypes) {
                if (!elemTypes.empty()) {
                    for (size_t i = 0; i < elemTypes.size() && i <= 20; i++) {
                        const auto& et = elemTypes[i];
                        if (et == "float" || et == "float_list" || et == "list_float") {
                            globalElementTypes[cname][i] = "float";
                        } else if (et == "int" || et == "int_list" || et == "list_int") {
                            globalElementTypes[cname][i] = "int";
                        }
                    }
                }
            }
        }
        
        // ============== Phase 2: Module globals ==============
        // For each module global, determine its container type by checking
        // what value was assigned to it. Module globals are assigned by:
        // 1. Direct list/tuple/dict literals (via lowerList/lowerDict)
        // 2. Function calls that return containers (list(), combinations(), etc.)
        
        // Check typeOf for module globals - if typeOf("POSITION") == "list_float",
        // then POSITION holds a float list
        std::unordered_map<std::string, std::string> globalToValueType;
        for (const auto& gname : ir.moduleGlobals) {
            std::string vt = typeOf(gname);
            globalToValueType[gname] = vt;
            if (vt == "list_float") {
                for (size_t i = 0; i <= 20; i++) {
                    globalElementTypes[gname][i] = "float";
                }
                tempContainerType[gname] = "float_list";
            } else if (vt == "list_int" || vt == "int" || vt == "i64" || vt == "bool") {
                for (size_t i = 0; i <= 20; i++) {
                    globalElementTypes[gname][i] = "int";
                }
                tempContainerType[gname] = "int_list";
            } else if (vt == "list" || vt == "dict" || vt == "") {
                // Generic container - check if assigned temp has element type info
                auto ttit = globalToTemp.find(gname);
                if (ttit != globalToTemp.end()) {
                    auto temp = ttit->second;
                    // Propagate from knownFloat/int temps
                    if (tempContainerType.count(temp) && tempContainerType[temp] == "float_list") {
                        tempContainerType[gname] = "float_list";
                        for (size_t i = 0; i <= 20; i++) globalElementTypes[gname][i] = "float";
                    } else if (tempContainerType.count(temp) && tempContainerType[temp] == "int_list") {
                        tempContainerType[gname] = "int_list";
                        for (size_t i = 0; i <= 20; i++) globalElementTypes[gname][i] = "int";
                    } else {
                        // Check listElementTypes for per-index element types
                        for (auto& fnx : ir.functions) {
                            auto lit = fnx.listElementTypes.find(temp);
                            if (lit != fnx.listElementTypes.end() && !lit->second.empty()) {
                                for (size_t i = 0; i < lit->second.size() && i <= 20; i++) {
                                    const auto& et = lit->second[i];
                                    if (et == "float" || et == "float_list" || et == "list_float") {
                                        globalElementTypes[gname][i] = "float";
                                    }
                                    if (et == "int" || et == "int_list" || et == "list_int") {
                                        globalElementTypes[gname][i] = "int";
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        
        // ============== Phase 3: Propagate type info to all functions ==============
        // For each function, populate containerElementTypes from the module-level registry.
        // Also handle function parameter defaults.
        
        for (auto& fn : ir.functions) {
            // Propagate module global types to all functions
            for (const auto& [gname, elemMap] : globalElementTypes) {
                fn.subscriptElementTypes[gname] = elemMap;
            }
            
            // Check parameter defaults
            for (size_t i = 0; i < fn.defaultGlobals.size() && i < fn.args.size(); ++i) {
                const auto& paramName = fn.args[i];
                std::string defaultTemp = fn.defaultGlobals[i];
                
                // Find what type the default resolves to
                std::string defaultElemType = "boxed";
                
                // Check if default matches a global name directly
                if (globalToValueType.count(defaultTemp)) {
                    std::string vt = globalToValueType[defaultTemp];
                    if (vt == "list_float") {
                        defaultElemType = "float_list";
                    } else if (vt == "list_int") {
                        defaultElemType = "int_list";
                    }
                }
                
                // Check if default resolves via globalToTemp map
                auto gtit = globalToTemp.find(defaultTemp);
                if (gtit != globalToTemp.end()) {
                    auto cit = tempContainerType.find(gtit->second);
                    if (cit != tempContainerType.end()) {
                        defaultElemType = cit->second;
                    }
                }
                
                // Also check direct global name
                auto gvit = globalToValueType.find(defaultTemp);
                if (gvit != globalToValueType.end()) {
                    if (gvit->second == "list_float" && (defaultElemType == "boxed")) {
                        defaultElemType = "float_list";
                    } else if (gvit->second == "list_int" && (defaultElemType == "boxed")) {
                        defaultElemType = "int_list";
                    }
                }
                
                // Propagate to this function
                if (defaultElemType == "float_list") {
                    for (size_t idx = 0; idx <= 20; idx++) {
                        fn.subscriptElementTypes[paramName][idx] = "float";
                    }
                    fn.containerElementTypes[paramName][0] = "float_list";
                } else if (defaultElemType == "int_list") {
                    for (size_t idx = 0; idx <= 20; idx++) {
                        fn.subscriptElementTypes[paramName][idx] = "int";
                    }
                    fn.containerElementTypes[paramName][0] = "int_list";
                } else {
                    // Generic container - record that subscript returns boxed
                    fn.containerElementTypes[paramName][0] = "boxed";
                }
            }
        }
    }

    // Infer per-index element types for temps created by subscript operations.
    // Walks the IR to find PyList_GetItem / Pyc_Subscript calls and propagates
    // element type info from source containers to result temps.
    void inferListElementTypes() {
        // Phase 1: Collect element type info from module-level globals
        std::unordered_map<std::string, std::unordered_map<size_t, std::string>> globalElemTypes;
        for (const auto& gname : ir.moduleGlobals) {
            std::string vt = typeOf(gname);
            if (vt == "list_float") {
                for (size_t i = 0; i <= 20; i++) globalElemTypes[gname][i] = "float";
            } else if (vt == "list_int" || vt == "int" || vt == "i64" || vt == "bool") {
                for (size_t i = 0; i <= 20; i++) globalElemTypes[gname][i] = "int";
            }
            for (auto& fnx : ir.functions) {
                if (fnx.name != currentFunc) continue;
                auto cit = fnx.listElementTypes.find(gname);
                if (cit != fnx.listElementTypes.end()) {
                    globalElemTypes[gname] = std::unordered_map<size_t, std::string>();
                    for (size_t i = 0; i < cit->second.size(); i++) {
                        globalElemTypes[gname][i] = cit->second[i];
                    }
                }
            }
        }
        
        // Helper: resolve a name (or temp chain) to its element types for a given index
        auto getElemTypesForIdx = [&](const std::string& name, size_t idx, 
                                       const std::unordered_map<std::string, std::pair<std::string, size_t>>& tSrc,
                                       std::unordered_map<size_t, std::string>& result) -> bool {
            std::string curr = name;
            for (int d = 0; d < 100; d++) {
                // Check fn.listElementTypes[curr]
                for (auto& fnx : ir.functions) {
                    auto lit = fnx.listElementTypes.find(curr);
                    if (lit != fnx.listElementTypes.end() && !lit->second.empty()) {
                        if (idx < lit->second.size()) {
                            std::string et = lit->second[idx];
                            if (et == "float" || et == "float_list" || et == "list_float") { 
                                for (size_t i = 0; i <= 20; i++) result[i] = "float"; return true; 
                            } else if (et == "int" || et == "int_list" || et == "list_int") { 
                                for (size_t i = 0; i <= 20; i++) result[i] = "int"; return true; 
                            }
                        }
                    }
                    // Also check fn.subscriptElementTypes[curr] — populated by lowerList for mixed-type containers
                    auto sit = fnx.subscriptElementTypes.find(curr);
                    if (sit != fnx.subscriptElementTypes.end()) {
                        for (const auto& [ikey, et] : sit->second) {
                            if (et == "float" || et == "float_list" || et == "list_float") { 
                                for (size_t i = 0; i <= 20; i++) result[i] = "float"; return true; 
                            } else if (et == "int" || et == "int_list" || et == "list_int") { 
                                for (size_t i = 0; i <= 20; i++) result[i] = "int"; return true; 
                            }
                            break;
                        }
                    }
                    // Also check fn.containerElementTypes[curr]
                    auto cit = fnx.containerElementTypes.find(curr);
                    if (cit != fnx.containerElementTypes.end()) {
                        for (const auto& [ikey, ctypes] : cit->second) {
                            if (ctypes == "float_list") { for (size_t i = 0; i <= 20; i++) result[i] = "float"; return true; }
                            if (ctypes == "int_list") { for (size_t i = 0; i <= 20; i++) result[i] = "int"; return true; }
                            break;
                        }
                    }
                }
                // Check globalElemTypes (only for non-temp names)
                if (curr.rfind("t", 0) != 0) {
                    auto git = globalElemTypes.find(curr);
                    if (git != globalElemTypes.end()) {
                        auto iit = git->second.find(idx);
                        if (iit != git->second.end()) {
                            std::string et = iit->second;
                            if (et == "float" || et == "float_list") { for (size_t i = 0; i <= 20; i++) result[i] = "float"; return true; }
                            else if (et == "int") { for (size_t i = 0; i <= 20; i++) result[i] = "int"; return true; }
                        }
                    }
                }
                // Walk the temp chain
                auto src = tSrc.find(curr);
                if (src != tSrc.end()) { curr = src->second.first; }
                else break;
            }
            return false;
        };

        for (auto& fn : ir.functions) {
            // Track: temp name → (container name, literal index) used to create it
            std::unordered_map<std::string, std::pair<std::string, size_t>> tempSource;
            // Map: temp name → per-index element types
            std::unordered_map<std::string, std::unordered_map<size_t, std::string>> tempElemTypes;

            for (auto& inst : fn.body) {
                if (inst.op != "call") continue;
                const std::string& callee = inst.operands.empty() ? "" : inst.operands[0].name;
                if (inst.result.empty()) continue;
                const std::string& res = inst.result;

                // Track Pyc_Subscript calls for temp source resolution
                if (callee == "Pyc_Subscript" && inst.operands.size() >= 2) {
                    const std::string& objName = inst.operands[0].name;
                    const std::string& idxName = inst.operands[1].name;
                    size_t litIdx = 0;
                    bool hasLitIdx = false;
                    try { litIdx = std::stoull(idxName); hasLitIdx = true; } catch (...) { hasLitIdx = false; }
                    tempSource[res] = {objName, litIdx};
                }
                // Also track PyList_GetItemObj results for temp chain resolution
                if (callee == "PyList_GetItemObj" && inst.operands.size() >= 2) {
                    std::string obj = (fn.name == currentFunc) ? inst.operands[0].name : inst.operands[1].name;
                    std::string idx = (fn.name == currentFunc) ? inst.operands[1].name : inst.operands[2].name;
                    size_t litIdx = 0;
                    bool hasLitIdx = false;
                    try { litIdx = std::stoull(idx); hasLitIdx = true; } catch (...) { hasLitIdx = false; }
                    tempSource[res] = {obj, litIdx};
                }

                // Track PyList_New*Boxed results (comprehensions) as element type sources
                if (callee == "PyList_NewIntBoxed" && inst.operands.size() >= 1) {
                    std::unordered_map<size_t, std::string> intTypes;
                    for (size_t i = 0; i <= 20; i++) intTypes[i] = "int";
                    tempElemTypes[res] = intTypes;
                    fn.subscriptElementTypes[res] = intTypes;
                }
                if (callee == "PyList_NewFloatBoxed" && inst.operands.size() >= 1) {
                    std::unordered_map<size_t, std::string> floatTypes;
                    for (size_t i = 0; i <= 20; i++) floatTypes[i] = "float";
                    tempElemTypes[res] = floatTypes;
                    fn.subscriptElementTypes[res] = floatTypes;
                }

                // PyList_GetItemObj(container, index) → element type inference
                if (callee == "PyList_GetItemObj" && inst.operands.size() >= 2) {
                    std::string objName = (fn.name == currentFunc) ? inst.operands[0].name : inst.operands[1].name;
                    std::string idxName = (fn.name == currentFunc) ? inst.operands[1].name : "";
                    
                    size_t litIdx = 0;
                    bool hasLitIdx = false;
                    if (!idxName.empty()) {
                        try { litIdx = std::stoull(idxName); hasLitIdx = true; } catch (...) { hasLitIdx = false; }
                    }
                    
                    std::unordered_map<size_t, std::string> resElemTypes;
                    
                    // Always first check subscriptElementTypes/containerElementTypes directly for the container name
                    // This handles the case where the container is a global or has been propagated via assigns
                    bool foundDirect = false;
                    for (auto& fnx : ir.functions) {
                        auto sit = fnx.subscriptElementTypes.find(objName);
                        if (sit != fnx.subscriptElementTypes.end() && !sit->second.empty()) {
                            for (const auto& [ikey, et] : sit->second) {
                                if (et == "float" || et == "float_list" || et == "list_float") { 
                                    for (size_t i = 0; i <= 20; i++) resElemTypes[i] = "float"; foundDirect = true; 
                                } else if (et == "int" || et == "int_list" || et == "list_int") { 
                                    for (size_t i = 0; i <= 20; i++) resElemTypes[i] = "int"; foundDirect = true; 
                                }
                                break;
                            }
                            if (foundDirect) break;
                        }
                        auto cit = fnx.containerElementTypes.find(objName);
                        if (cit != fnx.containerElementTypes.end() && !cit->second.empty()) {
                            for (const auto& [ikey, ctypes] : cit->second) {
                                if (ctypes == "float_list") { for (size_t i = 0; i <= 20; i++) resElemTypes[i] = "float"; foundDirect = true; }
                                if (ctypes == "int_list") { for (size_t i = 0; i <= 20; i++) resElemTypes[i] = "int"; foundDirect = true; }
                                break;
                            }
                            if (foundDirect) break;
                        }
                        if ((sit != fnx.subscriptElementTypes.end() && !sit->second.empty()) || 
                            (cit != fnx.containerElementTypes.end() && !cit->second.empty())) break;
                    }
                    
                    // Try: walk temp chain to find original global, then get its element types
                    if (!foundDirect && getElemTypesForIdx(objName, hasLitIdx ? litIdx : 0, tempSource, resElemTypes)) {
                        // success
                    } else if (hasLitIdx && resElemTypes.empty()) {
                        // Try direct lookup in fn.listElementTypes[container]
                        for (auto& fnx : ir.functions) {
                            auto lit = fnx.listElementTypes.find(objName);
                            if (lit != fnx.listElementTypes.end() && !lit->second.empty() && litIdx < lit->second.size()) {
                                std::string et = lit->second[litIdx];
                                if (et == "float" || et == "float_list" || et == "list_float") { 
                                    for (size_t i = 0; i <= 20; i++) resElemTypes[i] = "float"; break;
                                } else if (et == "int" || et == "int_list" || et == "list_int") { 
                                    for (size_t i = 0; i <= 20; i++) resElemTypes[i] = "int"; break;
                                }
                            }
                            break;
                        }
                    } else if (resElemTypes.empty()) {
                        // Try fn.subscriptElementTypes or containerElementTypes
                        for (auto& fnx : ir.functions) {
                            auto cit = fnx.containerElementTypes.find(objName);
                            if (cit != fnx.containerElementTypes.end()) {
                                if (hasLitIdx) {
                                    auto iit = cit->second.find(litIdx);
                                    if (iit != cit->second.end()) {
                                        if ((iit->second == "float_list")) for (size_t i = 0; i <= 20; i++) resElemTypes[i] = "float";
                                        else if ((iit->second == "int_list")) for (size_t i = 0; i <= 20; i++) resElemTypes[i] = "int";
                                    }
                                }
                            }
                            auto sit = fnx.subscriptElementTypes.find(objName);
                            if (sit != fnx.subscriptElementTypes.end()) {
                                for (const auto& [ikey, et] : sit->second) {
                                    if (et == "float" || et == "float_list" || et == "list_float") {
                                        for (size_t idx2 = 0; idx2 <= 20; idx2++) resElemTypes[idx2] = "float"; break;
                                    } else if (et == "int" || et == "int_list" || et == "list_int") {
                                        for (size_t idx2 = 0; idx2 <= 20; idx2++) resElemTypes[idx2] = "int"; break;
                                    }
                                    break;
                                }
                            }
                            break;
                        }
                    }

                    if (!resElemTypes.empty()) {
                        tempElemTypes[res] = resElemTypes;
                        fn.subscriptElementTypes[res] = resElemTypes;
                    }
                }

                // Assign: target = source — propagate element types
                if (inst.op == "assign" && !inst.operands.empty() && !res.empty()) {
                    auto src = tempElemTypes.find(inst.operands[0].name);
                    if (src != tempElemTypes.end()) {
                        tempElemTypes[res] = src->second;
                        fn.subscriptElementTypes[res] = src->second;
                    }
                }
            }
        }
    }

    // Generate per-param type info from call-site analysis.
    // For each function that is called with numeric types at all positions,
    // record the dominant type ("int" or "float") for each param slot.
    // This enables native param slot allocation even when args have defaults.
    void generateParamTypeAnalysis() {
        for (auto& kv : callSiteTypes) {
            const std::string& funcName = kv.first;
            const auto& allSigs = kv.second;
            if (allSigs.empty()) continue;

            // Find the original function
            IRFunction* func = nullptr;
            for (auto& f : ir.functions) {
                if (f.name == funcName) { func = &f; break; }
            }
            if (!func) continue;

            size_t declaredArgCount = func->args.size();
            if (declaredArgCount == 0) continue;

            // For each declared param, find the dominant type across all call sites.
            // A param is "dominant" if ALL call sites agree on the same numeric type.
            for (size_t pi = 0; pi < declaredArgCount; ++pi) {
                std::string dominant = "";
                bool consistent = true;
                bool allPositionsFilled = true;

                for (const auto& sig : allSigs) {
                    if (pi >= sig.size()) {
                        allPositionsFilled = false;
                        break;
                    }
                    const auto& t = sig[pi];
                    if (t == "i64") {
                        if (dominant == "" || dominant == "i64") {
                            dominant = t;
                        } else {
                            consistent = false; break;
                        }
                    } else if (t == "int") {
                        if (dominant == "" || dominant == "int") {
                            dominant = t;
                        } else {
                            consistent = false; break;
                        }
                    } else if (t == "float") {
                        if (dominant == "" || dominant == "float") {
                            dominant = t;
                        } else {
                            consistent = false; break;
                        }
                    }
                    // Any other type (boxed, list, etc.) means we can't track
                }

                if (!consistent || !allPositionsFilled) continue;

                if (dominant == "int" || dominant == "i64") {
                    func->paramTypes.push_back("int");
                } else if (dominant == "float") {
                    func->paramTypes.push_back("float");
                } else {
                    func->paramTypes.push_back("");  // unknown or non-numeric
                }
            }
            // Pad remaining params (in case some weren't filled)
            while ((size_t)func->paramTypes.size() < declaredArgCount) {
                func->paramTypes.push_back("");
            }
        }
    }

  private:
     ModuleIR& ir;
     // B7: set of module names that were successfully compiled (used to decide
     // whether import lowering emits __module__<name> or pyc_import_failed).
     std::unordered_set<std::string> compiledModules;
     // B7: map from imported module name to its exported (non-underscore) globals
     // at the time the main module is lowered. Used to statically expand
     // `from X import *` instead of trying to look up the literal key "*".
     std::unordered_map<std::string, std::vector<std::string>> importedModuleGlobals;
    std::string currentFunc;
    std::string currentClass;
    int tempCounter = 0;
    // Current innermost loop labels — updated by lowerFor/lowerWhile so
    // break/continue target the right blocks even with nested loops.
    std::string loopContinueLabel;
    std::string loopBreakLabel;
    std::unordered_map<std::string, int> funcDefaultCount;
    std::unordered_map<std::string, std::vector<std::string>> funcDefaultValues;
    std::unordered_map<std::string, std::vector<std::string>> funcParamNames;
    std::unordered_map<std::string, std::string> valueTypes;
     // A2.1: names proven to stay numeric (int/i64/float) for their live range.
     // These get native i64/double slots instead of boxed PyObject*.
     std::unordered_set<std::string> numericLocals;
    // Native float locals - proven to stay float through computation chains
    std::unordered_set<std::string> numericFloatLocals;
     // B16: names proven to be complex numbers (type 13).
     std::unordered_set<std::string> complexVars;
     // Map from user-level name to synthetic lambda function name for call resolution.
    std::unordered_map<std::string, std::string> lambdaAliases;
    // Track list/tuple literals assigned to names (intra-function) so that
    // *args at call sites can statically expand to the right number of
    // operands on the emitted 'call' IR when the length is known.
    // We store the raw AST element nodes and re-lower at the use site to
    // emit the element values (avoids temp lifetime issues across statements).
    std::unordered_map<std::string, std::vector<ASTNode*>> listLiteralElemASTs;
    // Names of IR functions we have registered (for deciding static vs dynamic
    // call lowering in B4/B8 indirect callable support).
    std::unordered_set<std::string> knownIRFunctions;
    // User-defined functions only (defs + nested defs by IR name) — excludes the
    // special builtin shims that share knownIRFunctions. Used to decide when a
    // bare Name in value position should produce a callable token.
    std::unordered_set<std::string> userDefFunctions;
    // Set of class names for class instantiation support.
    std::unordered_set<std::string> knownClasses;
    // Map from class name to __init__ parameter names (comma-separated).
    std::unordered_map<std::string, std::string> classInitParams;
    // Names that have been assigned (or unpacked into) values that may be
    // callable tokens at runtime (lambdas, results of calls that return lambdas,
    // elements of containers holding lambdas, copies of such names, etc.).
    // Used to decide whether a bare-name callee should load its runtime value
    // as the token for Pyc_Apply (B4 completeness for returned/aliased lambdas).
    std::unordered_set<std::string> namesThatMayHoldCallableTokens;
    // B5: bare names whose runtime value is a descriptor bundle for a capturing lambda/closure.
    std::unordered_set<std::string> namesThatMayHoldBundles;
    // Temps (consts or results) whose runtime value is a callable token string.
    std::unordered_set<std::string> callableTokenTemps;
    // B6: temps that hold super() proxy objects
    std::unordered_set<std::string> superProxyTemps;
    // B6: map from class name to its list of base class names (for multiple inheritance)
    std::unordered_map<std::string, std::vector<std::string>> classBases;
    // User functions (defs or synthetic lambdas) that contain a return of a
    // callable token value. Calls to them have their result temp marked so
    // that subsequent assigns/unpacks/calls can propagate the token nature (B4).
    std::unordered_set<std::string> functionsThatReturnCallables;
    // List (or tuple) temps from lowerList whose element(s) are callable token
    // temps. Used to mark subscript results and unpack targets as potential tokens.
    std::unordered_set<std::string> listsContainingCallableTokens;
    std::unordered_set<std::string> listsContainingBundles;
    std::unordered_set<std::string> knownFloatLists;
    std::unordered_set<std::string> knownIntLists;
    std::unordered_set<std::string> namesThatMayHoldListsWithBundles;
    // During lowering of a FunctionDef/lambda body, set true if any ret (or
    // implicit lambda body) produces a tracked callable token. At end of the
    // function we record the function name in functionsThatReturnCallables.
    bool currentFnReturnsCallable = false;
    // During lowering of a FunctionDef/lambda body, this accumulates the return type.
    // If multiple different types are returned, it becomes "boxed" at the end.
    std::string currentFnReturnType = "boxed";
    // Map from IR result temp of a string constant (emitted as the "value" of
    // a lambda expression) to the synthetic IR function name it refers to.
    // This enables treating a lambda "value" (string token) as a callable target
    // when it appears as a callee expression (B4 progress on lambda as value).
     std::unordered_map<std::string, std::string> callableTokenToSynthetic; // temp -> synthetic name
     // S4: map from module-level dict name → common value type of all values.
     // Populated when lowering a dict literal with string keys where all values
     // have the same type. Used by .values()/.keys()/.items() to inherit types.
     std::unordered_map<std::string, std::string> dictValueTypes;
     // S4: map from temp → element type when that temp is a known typed container.
     // Complements typeOf(): if typeOf(temp)="list" but we know it came from
     // sorted(list_float_arg), we record that it's a list_of_float_list.
     std::unordered_map<std::string, std::string> tempContainerElementTypes;

      // B6: helper to get the first base class of a given class
      std::string getFirstBase(const std::string& className) {
          auto it = classBases.find(className);
          if (it != classBases.end() && !it->second.empty()) {
              return it->second[0];
          }
          return "";
      }
      // B6: helper to get all base classes of a given class
      std::vector<std::string> getAllBases(const std::string& className) {
          auto it = classBases.find(className);
          return (it != classBases.end()) ? it->second : std::vector<std::string>();
      }
      // B6b: C3 linearization for MRO (Method Resolution Order)
      std::unordered_map<std::string, std::vector<std::string>> classMRO;
      std::vector<std::string> computeMRO(const std::string& className) {
          // Get bases for this class
          std::vector<std::string> bases = getAllBases(className);
          if (bases.empty()) {
              // Store the trivial MRO too — otherwise the class dict gets no
              // __mro__ and later lookups default-construct an empty entry.
              classMRO[className] = {className};
              return {className};
          }
          // C3 linearization algorithm
          // L[C] = C + merge(L[B1], L[B2], ..., [B1, B2, ...])
          std::unordered_map<std::string, std::vector<std::string>> linearizations;
          // Compute linearizations for all bases first
          for (const auto& base : bases) {
              if (linearizations.find(base) == linearizations.end()) {
                  if (classMRO.count(base)) {
                      linearizations[base] = classMRO[base];
                  } else {
                      linearizations[base] = computeMRO(base);
                      classMRO[base] = linearizations[base];
                  }
              }
          }
          // Build merge list: L[B1], L[B2], ..., [B1, B2, ...]
          std::vector<std::vector<std::string>> mergeList;
          for (const auto& base : bases) {
              if (linearizations.count(base)) {
                  mergeList.push_back(linearizations[base]);
              }
          }
          mergeList.push_back(bases);
          // Perform merge
          std::vector<std::string> mro;
          mro.push_back(className);
          while (!mergeList.empty()) {
              bool found = false;
              for (size_t i = 0; i < mergeList.size(); ++i) {
                  if (mergeList[i].empty()) continue;
                  // Copy, not reference: the removal loop below erases list
                  // heads, which would shift what a reference points at and
                  // corrupt the comparisons for the remaining lists.
                  const std::string candidate = mergeList[i][0];
                  // Check if candidate is in the tail of any other list
                  bool bad = false;
                  for (size_t j = 0; j < mergeList.size(); ++j) {
                      if (i == j || mergeList[j].empty()) continue;
                      for (size_t k = 1; k < mergeList[j].size(); ++k) {
                          if (mergeList[j][k] == candidate) {
                              bad = true;
                              break;
                          }
                      }
                      if (bad) break;
                  }
                  if (!bad) {
                      // Add to MRO and remove from all lists
                      mro.push_back(candidate);
                      for (size_t j = 0; j < mergeList.size(); ++j) {
                          if (!mergeList[j].empty() && mergeList[j][0] == candidate) {
                              mergeList[j].erase(mergeList[j].begin());
                          }
                      }
                      found = true;
                      break;
                  }
              }
              if (!found) {
                  // Merge failed - this shouldn't happen with valid Python classes
                  // Fall back to first-base-wins
                  break;
              }
          }
          classMRO[className] = mro;
          return mro;
      }
      std::string getNextClassInMRO(const std::string& className, const std::string& currentClass) {
          // Find the next class in MRO after the current class
          const auto& mro = classMRO[className];
          for (size_t i = 0; i < mro.size(); ++i) {
              if (mro[i] == currentClass && i + 1 < mro.size()) {
                  return mro[i + 1];
              }
          }
          // Fall back to first base
          return getFirstBase(className);
      }


    // A6: Call-site type tracking for monomorphization.
    // Maps funcName -> list of observed type signatures (each signature is a vector of arg types).
    // Used after lowering to generate specialized variants for functions called with
    // consistent numeric argument types.
    std::unordered_map<std::string, std::vector<std::vector<std::string>>> callSiteTypes;
    // Last synthetic name produced by lowerLambda (used by assign of a lambda
    // to capture the alias after we started emitting a boxed string value for
    // the lambda expression).
    std::string lastLambdaSynthetic;

    // B5 (nonlocal/cells): per-function list of names declared 'nonlocal' inside that function.
    // These names must be backed by cells (heap objects allocated in an enclosing scope)
    // rather than ordinary locals or module globals. The map is populated during FunctionDef
    // lowering via scanFuncNonlocals. Full cell allocation + hidden-param passing + load/store
    // rewrite happens in subsequent B5 increments.
    std::unordered_map<std::string, std::vector<std::string>> funcNonlocals;

    // B5: names that actually use cell storage for this function (union of nonlocals here
    // and names we assign here that descendants access via nonlocal).
    std::unordered_map<std::string, std::vector<std::string>> funcCells;

    // B5: for a nested function, the Python-level cell names it needs from an enclosing scope.
    // These become synthesized hidden leading parameters (cells) when lowering the nested func.
    std::unordered_map<std::string, std::vector<std::string>> funcFreeCells;

    // B5: names for which *this* function allocates the cell (owns the binding for closed-over descendants).
    // Distinct from funcFreeCells (which are received via hidden params because this function declared nonlocal).
    std::unordered_map<std::string, std::vector<std::string>> funcOwnedCells;

    // B5: set of synthetic names (defs or lambdas) whose lowered "value" must carry cells
    // (a descriptor bundle) rather than a bare string token. When such a value flows to a
    // call site we will extract the cells from the bundle and pass them as leading args.
    std::unordered_set<std::string> closureFunctions;

    // B5: for a descriptor bundle temp (value of a capturing lambda/closure), the ordered
    // Python-level cell names it carries. Used at call sites to splice the right cells.
    std::unordered_map<std::string, std::vector<std::string>> descriptorCells;

    // B5: descriptor bundle temp -> synthetic IR name (so call sites can resolve the real target).
    std::unordered_map<std::string, std::string> bundleToSynthetic;

    // B5: temps that are known descriptor bundles (for propagation through assign/return/etc.).
    std::unordered_set<std::string> bundleTemps;
    // B5/B4: for a bundle temp, how many trailing default values (prebound args) follow the cells.
    std::unordered_map<std::string, size_t> bundlePreboundArgCount;

    // B5: functions that return descriptor bundles (capturing lambdas returned from makers etc.).
    std::unordered_set<std::string> functionsThatReturnBundles;
    // Generator functions: contain yield expressions. Calls to them are
    // wrapped with clear→call→get_buffer to materialize the yielded values.
    std::unordered_set<std::string> generatorFunctions;

    // Helper: recursively check if an AST node contains a YieldExpr.
    bool containsYield(const ASTNode* node) const {
        if (!node) return false;
        if (node->type == "YieldExpr") return true;
        for (const auto& c : node->children) {
            if (containsYield(c.get())) return true;
        }
        return false;
    }

    // Pre-scan all function bodies to detect generators.
    void scanForGenerators(const ASTNode* node) {
        if (!node) return;
        if (node->type == "FunctionDef" || node->type == "Lambda") {
            std::string fnName;
            if (node->type == "FunctionDef" && !node->id.empty()) {
                fnName = node->id;
            } else if (node->type == "Lambda") {
                // Lambdas get synthetic names; check if any child is a FunctionDef
                // that we've already processed. For now, skip lambda detection.
            }
            if (!fnName.empty()) {
                for (const auto& c : node->children) {
                    if (containsYield(c.get())) {
                        generatorFunctions.insert(fnName);
                        break;
                    }
                }
            }
        }
        for (const auto& c : node->children) {
            scanForGenerators(c.get());
        }
    }
    std::unordered_map<std::string, std::string> functionReturnedBundleSynthetic;
    std::unordered_map<std::string, std::vector<std::string>> functionReturnedBundleCaps;
    std::unordered_map<std::string, std::vector<std::string>> lambdaDefaultTemps;

    // Unique IR names for nested FunctionDefs to avoid collisions on source names
    // (e.g. two 'def inner()' in different makers). Top-level defs keep their source id.
    int nestedFuncCounter = 0;
    // enclosingIRName -> (python def name -> unique IR name)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> enclosingToNestedDef;

    // Per-FunctionDef state for returns of bundles (to record at end of the def).
    bool currentFnReturnsBundle = false;
    std::string currentReturnedBundleSynthetic;
    std::vector<std::string> currentReturnedBundleCaps;

    // B5 helper (member, callable during lowering of exprs/stmts in a FunctionDef body):
    // Returns true if 'nm' must be treated as cell-backed while lowering the *current* function.
    // We consult the three analysis maps so that:
    //  - owners (present in funcOwnedCells for this scope) go through cells,
    //  - direct nonlocals (present in funcNonlocals) go through cells,
    //  - forwarders (present in funcFreeCells) go through cells.
    // Using a single predicate here prevents the owner from doing a plain local assign for
    // a name it owns as a cell, which was the root cause of "cell mutation visible inside
    // callee but stale value read later in owner".
    bool isCellBackedHere(const std::string& nm) const {
        auto cit = funcCells.find(currentFunc);
        if (cit != funcCells.end()) {
            for (const auto& v : cit->second) if (v == nm) return true;
        }
        auto oit = funcOwnedCells.find(currentFunc);
        if (oit != funcOwnedCells.end()) {
            for (const auto& v : oit->second) if (v == nm) return true;
        }
        auto fit = funcFreeCells.find(currentFunc);
        if (fit != funcFreeCells.end()) {
            for (const auto& v : fit->second) if (v == nm) return true;
        }
        return false;
    }

    // Builtin exception class names — calls to these construct a structured
    // exception object (pyc_make_exc) instead of going through normal call
    // resolution.
    static const std::unordered_set<std::string>& builtinExcNames() {
        static const std::unordered_set<std::string> names = {
            "BaseException", "Exception", "ArithmeticError", "ZeroDivisionError",
            "OverflowError", "FloatingPointError", "LookupError", "IndexError",
            "KeyError", "ValueError", "TypeError", "RuntimeError", "StopIteration",
            "AttributeError", "NameError", "UnboundLocalError", "NotImplementedError",
            "OSError", "IOError", "FileNotFoundError", "PermissionError",
            "AssertionError", "SyntaxError", "IndentationError"
        };
        return names;
    }

    // Emit a function-object value: pyc_make_func(token, displayName).
    // The token resolves through the callable registry in Pyc_Apply; the
    // display name is what repr shows (<function displayName at ...>).
    std::string emitFuncValue(const std::string& irName, const std::string& displayName) {
        // Dedicated temp namespace: emitFuncValue also runs right after a
        // FunctionDef restores tempCounter, and consuming shared-counter
        // numbers there collides with temp-name-keyed compile-time maps
        // (bundleTemps / callableTokenToSynthetic) populated during the
        // function body's lowering.
        int n = fvCounter++;
        std::string tok = "cfv" + std::to_string(n);
        ir.addInstruction(currentFunc, "const", {"\"" + irName + "\""}, tok, "str");
        std::string disp = "cfv" + std::to_string(n) + "d";
        ir.addInstruction(currentFunc, "const", {"\"" + displayName + "\""}, disp, "str");
        std::string res = "tfv" + std::to_string(n);
        ir.addInstruction(currentFunc, "call", {"pyc_make_func", tok, disp}, res);
        return res;
    }
    int fvCounter = 0;

    // True when 'name' is bound locally in the current scope (parameter or a
    // name assigned so far), i.e. it must resolve as a variable even if a
    // user def of the same name exists.
    bool isShadowedLocal(const std::string& name) const {
        if (valueTypes.count(name)) return true;
        auto pit = funcParamNames.find(currentFunc);
        if (pit != funcParamNames.end()) {
            for (const auto& p : pit->second) {
                std::string b = p;
                while (!b.empty() && b[0] == '*') b = b.substr(1);
                if (b == name) return true;
            }
        }
        return false;
    }

    void noteType(const std::string& name, const std::string& type) {
        if (!name.empty() && !type.empty()) valueTypes[name] = type;
        // A2.1: promote to native numeric local if type is proven numeric (int/i64/bool first)
        if (type == "int" || type == "i64" || type == "bool") {
            numericLocals.insert(name);
        }
        // A6: track float provenance for native float computation chains
        if (type == "float") {
            numericFloatLocals.insert(name);
        }
    }

    // Helper: check if a name is a global variable in the current function.
    bool isGlobalHere(const std::string& name) const {
        for (auto& fnr : ir.functions) {
            if (fnr.name == currentFunc) {
                for (const auto& g : fnr.globalVars) {
                    if (g == name) return true;
                }
                return false;
            }
        }
        return false;
    }

    std::string typeOf(const std::string& name) const {
        auto it = valueTypes.find(name);
        std::string t = it == valueTypes.end() ? "boxed" : it->second;
        if (t == "i64") return "int";
        return t;
    }

    // A2.1: mark a name as no longer eligible for native numeric storage
    // (e.g. assigned a string, list, or unknown value).
    void killNumericLocal(const std::string& name) {
        numericLocals.erase(name);
        numericFloatLocals.erase(name);
        // Don't overwrite the type here - the type was already set correctly by noteType
        // if (!name.empty()) valueTypes[name] = "boxed";
    }

    std::string numericResultType(const std::string& op,
                                    const std::string& left,
                                    const std::string& right) const {
        std::string lt = typeOf(left);
        std::string rt = typeOf(right);
        if (op == "truediv") return "float";
        if (op == "pow") {
            auto isNum = [](const std::string& t){ return t=="int" || t=="bool" || t=="float" || t=="i64"; };
            if (isNum(lt) && isNum(rt)) {
                if (lt == "float" || rt == "float") return "float";
                return "boxed";
            }
            return "boxed";
        }
        auto isNum = [](const std::string& t){ return t=="int" || t=="bool" || t=="float" || t=="i64"; };
        if (isNum(lt) && isNum(rt)) {
            return (lt == "float" || rt == "float") ? "float" : "int";
        }
        return "boxed";
    }

    // A4: Detect the element type of a list comprehension expression from
    // its AST node, without lowering.  Returns "int", "float", or "boxed".
    // Handles the common cases: constants, names (via valueTypes), binary
    // ops, unary ops, and calls to numeric-producing builtins.
    std::string detectCompElementType(const ASTNode* node) const {
        if (!node) return "boxed";
        if (node->type == "Constant") {
            if (node->is_bool || !node->is_float && !node->is_str && !node->is_none)
                return "int";
            if (node->is_float) return "float";
            return "boxed";
        }
        if (node->type == "Name") {
            // We can't look up valueTypes here because the loop variable
            // hasn't been lowered yet.  Fall through to "boxed" for names.
            return "boxed";
        }
        if (node->type == "BinOp") {
            std::string op = node->id; // "+", "-", "*", etc.
            std::string lt = detectCompElementType(node->children[0].get());
            std::string rt = detectCompElementType(node->children.size() > 1 ? node->children[1].get() : nullptr);
            auto isNum = [](const std::string& t){ return t=="int" || t=="bool" || t=="float"; };
            if (!isNum(lt) || !isNum(rt)) return "boxed";
            if (op == "truediv" || op == "/") return "float";
            if (lt == "float" || rt == "float") return "float";
            return "int";
        }
        if (node->type == "Subscript") {
            // Try to infer element type from the subscript expression.
            // This handles patterns like bodies[i][j] in list comprehensions.
            if (node->children.size() >= 2) {
                const ASTNode* objNode = node->children[0].get();
                const ASTNode* idxNode = node->children[1].get();
                // Check if objNode is a nested subscript - recurse to infer type
                if (objNode && objNode->type == "Subscript") {
                    std::string innerType = detectCompElementType(objNode);
                    if (innerType == "float" || innerType == "int") {
                        return innerType;
                    }
                }
                // Simple subscript with constant index - conservative: treat as float
                // This handles patterns like [x, y, z] where x = container[k]
                if (objNode && objNode->type == "Name" && idxNode && idxNode->type == "Constant") {
                    return "float";
                }
            }
            return "boxed";
        }
        if (node->type == "UnaryOp") {
            // Unary minus/plus on a numeric operand produces the same type
            std::string st = detectCompElementType(node->children[0].get());
            if (st == "float") return "float";
            if (st == "int" || st == "bool") return "int";
            return "boxed";
        }
        if (node->type == "Call") {
            // Calls to int(), float(), len(), abs(), sum(), etc. produce numeric
            std::string fn = "";
            if (!node->children.empty() && node->children[0]->type == "Name")
                fn = node->children[0]->id;
            if (fn == "int" || fn == "len" || fn == "abs" || fn == "sum" ||
                fn == "min" || fn == "max" || fn == "any" || fn == "all")
                return "int";
            if (fn == "float") return "float";
            // sorted() returns list → boxed
            if (fn == "sorted" || fn == "list" || fn == "reversed" || fn == "enumerate" || fn == "zip")
                return "boxed";
            return "boxed";
        }
        if (node->type == "List" || node->type == "Tuple") return "boxed";
        if (node->type == "Dict") return "boxed";
        if (node->type == "Compare") return "boxed";  // bool, but not commonly used as list element
        if (node->type == "JoinedStr" || node->type == "FormattedValue") return "boxed";
        if (node->type == "BoolOp") return "boxed";  // returns actual value, could be anything
        if (node->type == "Attribute") return "boxed";
        // Comprehension nested inside comprehension
        if (node->type == "ListComp") {
            // Check the inner element type
            if (node->children.size() >= 2)
                return detectCompElementType(node->children[0].get());
            return "boxed";
        }
        return "boxed";
    }

    void mergeBranchTypes(const std::unordered_map<std::string, std::string>& before,
                           const std::unordered_map<std::string, std::string>& thenTypes,
                           const std::unordered_map<std::string, std::string>& elseTypes) {
        std::unordered_set<std::string> names;
        for (const auto& kv : before) names.insert(kv.first);
        for (const auto& kv : thenTypes) names.insert(kv.first);
        for (const auto& kv : elseTypes) names.insert(kv.first);

        std::unordered_map<std::string, std::string> merged = before;
        for (const auto& name : names) {
            auto bit = before.find(name);
            std::string incoming = bit == before.end() ? "boxed" : bit->second;

            auto tit = thenTypes.find(name);
            auto eit = elseTypes.find(name);
            std::string thenType = tit == thenTypes.end() ? incoming : tit->second;
            std::string elseType = eit == elseTypes.end() ? incoming : eit->second;

            merged[name] = (thenType == elseType) ? thenType : "boxed";
        }
        valueTypes = std::move(merged);
    }

    // Conservative loop back-edge widening: if a variable's type at the end
    // of the body differs from its type on entry to the loop head, widen to
    // "boxed" so subsequent iterations (and code after the loop) do not
    // assume a type that is not stable across all iterations.
    void widenLoopTypes(const std::unordered_map<std::string, std::string>& entryTypes) {
        for (auto& kv : valueTypes) {
            auto eit = entryTypes.find(kv.first);
            if (eit != entryTypes.end() && eit->second != kv.second) {
                kv.second = "boxed";
            }
        }
    }

    // Recursively collect all names from `global` statements in the subtree.
    void collectGlobalDecls(const ASTNode* node) {
        if (!node) return;
        if (node->type == "Global") {
            for (const auto& name : node->args) ir.addModuleGlobal(name);
        }
        for (const auto& c : node->children) collectGlobalDecls(c.get());
    }

    void collectModuleBindings(const ASTNode* moduleNode) {
        if (!moduleNode || moduleNode->type != "Module") return;
        for (const auto& c : moduleNode->children) {
            if (!c) continue;
            if (c->type == "Assign") {
                if (!c->args.empty()) {
                    for (const auto& name : c->args) ir.addModuleGlobal(name);
                } else if (c->id == "__unpack__") {
                    // For unpacking (e.g., x, y = DATA), extract all target names
                    if (c->children.size() >= 1 && c->children[0]) {
                        collectTargetNames(c->children[0].get());
                    }
                } else if (!c->id.empty() && c->id != "__subscript__") {
                    ir.addModuleGlobal(c->id);
                }
            } else if (c->type == "FunctionDef") {
                ir.addModuleGlobal(c->id);
            }
        }
    }

    void collectTargetNames(const ASTNode* node) {
        if (!node) return;
        if (node->type == "Name" && !node->id.empty()) {
            ir.addModuleGlobal(node->id);
        } else if (node->type == "Tuple" || node->type == "List") {
            // Recurse into children unconditionally (even if id.empty())
            // Handles nested tuples/lists like ((a, b), c) = DATA
            for (const auto& c : node->children) {
                collectTargetNames(c.get());
            }
        }
    }

    // Collect names from `global` statements that are direct descendants of a FunctionDef.
    std::vector<std::string> scanFuncGlobals(const ASTNode* funcNode) {
        std::vector<std::string> result;
        for (const auto& c : funcNode->children) {
            if (c && c->type == "Global") {
                for (const auto& name : c->args) result.push_back(name);
            }
        }
        return result;
    }

    // B5: Collect names from `nonlocal` statements that are direct descendants of a FunctionDef.
    // These names must be resolved via cells (heap objects allocated in an enclosing scope),
    // not as ordinary locals or module globals.
    std::vector<std::string> scanFuncNonlocals(const ASTNode* funcNode) {
        std::vector<std::string> result;
        for (const auto& c : funcNode->children) {
            if (c && c->type == "Nonlocal") {
                for (const auto& name : c->args) result.push_back(name);
            }
        }
        return result;
    }

    // B5: collect names assigned (simple targets) inside a FunctionDef subtree.
    // Used to decide which assigned names must become cells because nested scopes
    // declare them nonlocal.
    std::vector<std::string> scanAssignedNames(const ASTNode* funcNode) {
        std::vector<std::string> result;
        std::function<void(const ASTNode*)> walk = [&](const ASTNode* n) {
            if (!n) return;
            if (n->type == "Assign") {
                if (!n->args.empty()) {
                    for (const auto& nm : n->args) {
                        if (!nm.empty() && nm != "__subscript__" && nm != "__unpack__")
                            result.push_back(nm);
                    }
                } else if (!n->id.empty() && n->id != "__subscript__" && n->id != "__unpack__") {
                    result.push_back(n->id);
                }
            } else if (n->type == "AugAssign") {
                if (!n->id.empty() && n->id != "__subscript__")
                    result.push_back(n->id);
            } else if (n->type == "For") {
                if (!n->id.empty() && n->id != "__unpack__")
                    result.push_back(n->id);
            }
            for (const auto& c : n->children) walk(c.get());
        };
        for (const auto& c : funcNode->children) walk(c.get());
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    // B5: recursively collect *all* names declared nonlocal anywhere in the subtree
    // rooted at funcNode (including funcNode itself and all descendant FunctionDefs).
    // This gives the full set of names that must be backed by cells for any scope
    // that can reach those declarations via nesting. Used for correct forwarding
    // through intermediate scopes that neither assign nor declare the name.
    // Names a function subtree reads but does not bind at the reading scope
    // (or below): its own free reads plus, transitively, the free reads of
    // its nested functions that this scope does not bind either. Used so an
    // intermediate scope forwards cells its inner closures need even when it
    // never references the name itself (decorator factories: repeat(n) ->
    // deco -> wrapper reads n; deco must carry n through).
    void collectTransitiveFreeReads(const ASTNode* fn, std::unordered_set<std::string>& out) {
        if (!fn) return;
        std::unordered_set<std::string> used;
        std::function<void(const ASTNode*)> walkOwn = [&](const ASTNode* n) {
            if (!n) return;
            if (n->type == "FunctionDef" || n->type == "Lambda") return;
            if (n->type == "Name" && !n->id.empty()) used.insert(n->id);
            for (const auto& c : n->children) walkOwn(c.get());
        };
        for (const auto& c : fn->children) walkOwn(c.get());
        std::unordered_set<std::string> localsHere;
        for (const auto& a : fn->args) {
            std::string b = a;
            while (!b.empty() && b[0] == '*') b = b.substr(1);
            if (!b.empty()) localsHere.insert(b);
        }
        std::function<void(const ASTNode*)> scanAsg = [&](const ASTNode* n) {
            if (!n) return;
            if (n->type == "FunctionDef" || n->type == "Lambda") {
                if (!n->id.empty()) localsHere.insert(n->id);  // def binds its name here
                return;
            }
            if (n->type == "Assign" && !n->id.empty()) localsHere.insert(n->id);
            for (const auto& c : n->children) scanAsg(c.get());
        };
        for (const auto& c : fn->children) scanAsg(c.get());
        std::unordered_set<std::string> sub;
        std::function<void(const ASTNode*)> findNested = [&](const ASTNode* n) {
            if (!n) return;
            if (n->type == "FunctionDef" || n->type == "Lambda") {
                collectTransitiveFreeReads(n, sub);
                return;
            }
            for (const auto& c : n->children) findNested(c.get());
        };
        for (const auto& c : fn->children) findNested(c.get());
        for (const auto& nm : used) if (!localsHere.count(nm)) out.insert(nm);
        for (const auto& nm : sub) if (!localsHere.count(nm)) out.insert(nm);
    }

    std::vector<std::string> collectDemandedNonlocals(const ASTNode* funcNode) {
        std::vector<std::string> result;
        std::function<void(const ASTNode*)> walk = [&](const ASTNode* n) {
            if (!n) return;
            if (n->type == "Nonlocal") {
                for (const auto& nm : n->args) {
                    if (!nm.empty()) result.push_back(nm);
                }
            }
            for (const auto& c : n->children) walk(c.get());
        };
        walk(funcNode);
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    // B5: collect bare Name ids referenced anywhere in the subtree.
    // Used for lambdas (and future nested scopes) to discover which names from
    // the definition scope they close over so we can force those names to cells.
    std::unordered_set<std::string> collectNames(const ASTNode* node) {
        std::unordered_set<std::string> out;
        std::function<void(const ASTNode*)> w = [&](const ASTNode* n) {
            if (!n) return;
            if (n->type == "Name" && !n->id.empty()) out.insert(n->id);
            for (const auto& c : n->children) w(c.get());
        };
        w(node);
        return out;
    }

    std::string lowerBinOp(const ASTNode* node) {
        std::string op = node->op.empty() ? "add" : node->op;
        if (op == "Add") op = "add";
        else if (op == "Sub") op = "sub";
        else if (op == "Mult") op = "mul";
        else if (op == "FloorDiv") op = "div";
        else if (op == "Div") op = "truediv";
        else if (op == "Mod") op = "mod";
        else if (op == "Pow") op = "pow";
        else if (op == "LShift") op = "lshift";
        else if (op == "RShift") op = "rshift";
        else if (op == "BitOr") op = "bitor";
        else if (op == "BitAnd") op = "bitand";
        else if (op == "BitXor") op = "bitxor";
        // A8: String formatting with % operator
        // When left operand is a string constant and op is Mod, emit a call
        // to PyString_Format(fmt, args) instead of numeric mod.
        if (op == "mod" && node->children.size() >= 2) {
            const ASTNode* leftNode = node->children[0].get();
            if (leftNode && leftNode->type == "Constant" && leftNode->is_str) {
                std::string left = lowerExpr(leftNode);
                std::string right = lowerExpr(node->children[1].get());
                std::string res = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyString_Format", left, right}, res);
                noteType(res, "str");
                return res;
            }
        }
        if (op == "pow" && node->children.size() > 1 && node->children[1]) {
            const ASTNode* rc = node->children[1].get();
            if (rc->type == "Constant" && !rc->is_float && !rc->is_str && !rc->is_none && !rc->is_bool) {
                char* eend = nullptr;
                errno = 0;
                long expv = std::strtol(rc->value.c_str(), &eend, 10);
                (void)eend; (void)errno;
                if (expv >= 0 && expv <= 8) {
                    std::string left = lowerExpr(node->children[0].get());
                    if (expv == 0) {
                        std::string one = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"1"}, one, "int");
                        noteType(one, "int");
                        return one;
                    }
                    std::string cur = left;
                    // Check if left is complex (boxed type from complex literal or complex op)
                    bool isComplex = (typeOf(left) == "boxed");
                    for (long k = 1; k < expv; ++k) {
                        std::string t = "t" + std::to_string(tempCounter++);
                        if (isComplex) {
                            ir.addInstruction(currentFunc, "call", {"PyComplex_Mul", cur, left}, t);
                            noteType(t, "boxed");
                        } else {
                            std::string rt = numericResultType("mul", cur, left);
                            ir.addInstruction(currentFunc, "mul", {cur, left}, t, rt);
                            noteType(t, rt);
                        }
                        cur = t;
                    }
                    return cur;
                }
            }
        }
        std::string left = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string right = lowerExpr(node->children.size() > 1 ? node->children[1].get() : nullptr);
        // B16: Complex arithmetic — if both operands are complex, emit complex calls
        if (op == "add" || op == "sub" || op == "mul" || op == "truediv") {
            std::string funcName;
            if (op == "add") funcName = "PyComplex_Add";
            else if (op == "sub") funcName = "PyComplex_Sub";
            else if (op == "mul") funcName = "PyComplex_Mul";
            else if (op == "truediv") funcName = "PyComplex_Div";
            if (!funcName.empty() && complexVars.count(left) > 0 && complexVars.count(right) > 0) {
                std::string res = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {funcName, left, right}, res);
                complexVars.insert(res);
                noteType(res, "boxed");
                return res;
            }
        }
        // B16: Complex pow — if op is pow and operands are complex, emit PyComplex_Pow
        if (op == "pow") {
            if (complexVars.count(left) > 0 && complexVars.count(right) > 0) {
                std::string res = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyComplex_Pow", left, right}, res);
                complexVars.insert(res);
                noteType(res, "boxed");
                return res;
            }
        }
        std::string res = "t" + std::to_string(tempCounter++);
        std::string resultType = numericResultType(op, left, right);
        ir.addInstruction(currentFunc, op, {left, right}, res, resultType);
        noteType(res, resultType);
        return res;
    }

    // Emit IR (in current context) that, given a list value containing the
    // full effective positional arguments for 'targetFunc', unpacks according
    // to the target's parameter signature (fixed params before any *vararg,
    // plus a collected tail list for the * slot if present) and emits a
    // 'call' instruction to the target with the correct static number of
    // operands. The result of the call is placed in 'resultTemp' (or a fresh
    // temp if empty). This is used by __va wrappers for dynamic *args calls.
    void emitForwardCallFromList(const std::string& targetFunc, const std::string& listVal, const std::string& resultTemp) {
        auto pit = funcParamNames.find(targetFunc);
        size_t fixed = 0;
        bool hasVar = false;
        if (pit != funcParamNames.end()) {
            const auto& ps = pit->second;
            for (size_t j = 0; j < ps.size(); ++j) {
                if (!ps[j].empty() && ps[j][0] == '*') { hasVar = true; break; }
                ++fixed;
            }
        }
        std::vector<std::string> fwd;
        for (size_t k = 0; k < fixed; ++k) {
            std::string ck = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(k)}, ck);
            std::string el = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", listVal, ck}, el);
            fwd.push_back(el);
        }
        std::string rest;
        if (hasVar) {
            // Collect [fixed .. n) into a fresh list for the * slot.
            std::string ln = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", listVal}, ln);
            std::string lnSlot = "__sl_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {ln}, lnSlot);
            std::string startC = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(fixed)}, startC);
            std::string zero = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, zero);
            rest = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, rest);
            std::string jv = "s" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {startC}, jv);
            int sc = tempCounter++;
            std::string slp = "vf_lp_" + std::to_string(sc);
            std::string sbd = "vf_bd_" + std::to_string(sc);
            std::string sex = "vf_ex_" + std::to_string(sc);
            ir.addInstruction(currentFunc, "br", {}, slp);
            ir.addInstruction(currentFunc, "label", {}, slp);
            std::string cm = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", jv, lnSlot}, cm);
            ir.addInstruction(currentFunc, "br", {cm, sbd, sex});
            ir.addInstruction(currentFunc, "label", {}, sbd);
            std::string el = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", listVal, jv}, el);
            std::string d = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_Append", rest, el}, d);
            std::string one = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, one);
            std::string nj = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "add", {jv, one}, nj);
            ir.addInstruction(currentFunc, "assign", {nj}, jv);
            ir.addInstruction(currentFunc, "br", {}, slp);
            ir.addInstruction(currentFunc, "label", {}, sex);
        }
        if (hasVar) fwd.push_back(rest);
        std::string callRes = resultTemp.empty() ? ("t" + std::to_string(tempCounter++)) : resultTemp;
        std::vector<std::string> cops = {targetFunc};
        cops.insert(cops.end(), fwd.begin(), fwd.end());
        ir.addInstruction(currentFunc, "call", cops, callRes);
    }

    void ensureVaWrapper(const std::string& target) {
        std::string wrapper = "__va_" + target;
        for (const auto& f : ir.functions) {
            if (f.name == wrapper) return;
        }
        ir.addFunction(wrapper, {"va"});
        funcParamNames[wrapper] = {"va"};
        for (auto& fnr : ir.functions) if (fnr.name == wrapper) { fnr.paramNames = {"va"}; break; }

        std::string savedFunc = currentFunc;
        int savedTemp = tempCounter;
        currentFunc = wrapper;
        tempCounter = 0;

        std::string vaParam = "va";
        std::string callRes = "t" + std::to_string(tempCounter++);
        emitForwardCallFromList(target, vaParam, callRes);
        ir.addInstruction(wrapper, "ret", {callRes});

        currentFunc = savedFunc;
        tempCounter = savedTemp;
    }

    std::string lowerCall(const ASTNode* node) {
        // Method call: obj.method(args) — func is an Attribute node
         if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Attribute") {
            return lowerMethodCall(node);
        }
        // super() call — returns a proxy that looks up methods on the parent class
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" && node->children[0]->id == "super") {
            std::string superProxy = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Super"}, superProxy);
            superProxyTemps.insert(superProxy);
            return superProxy;
        }
        // Builtin exception constructor: ValueError("msg"), KeyError(k), ...
        // Produces a structured exception object via pyc_make_exc. A local
        // binding with the same name (or a user class) takes precedence.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            builtinExcNames().count(node->children[0]->id) &&
            !knownClasses.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::string nameConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + node->children[0]->id + "\""}, nameConst, "str");
            std::string msg;
            for (size_t i = 1; i < node->children.size(); ++i) {
                if (node->children[i] && node->children[i]->type != "Keyword") {
                    msg = lowerExpr(node->children[i].get());
                    break;
                }
            }
            if (msg.empty()) {
                msg = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"\""}, msg, "str");
            }
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"pyc_make_exc", nameConst, msg}, res);
            return res;
        }
    // Class instantiation: ClassName(args) — create instance dict and call __init__
        std::string funcName;
        if (!node->children.empty() && node->children[0] && node->children[0]->type == "Name") {
            funcName = node->children[0]->id;
            auto classIt = knownClasses.find(funcName);
            if (classIt != knownClasses.end()) {
                std::string instanceDict = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyDict_New"}, instanceDict);
                // Store class reference on instance for method lookup
                std::string classKeyConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"__class__\""}, classKeyConst, "str");
                ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", instanceDict, classKeyConst, funcName}, "class_set");
                // Build __init__ function with correct parameters
                std::string initName = funcName + "__init__";
                std::vector<std::string> initParams;
                auto pit = classInitParams.find(funcName);
                if (pit != classInitParams.end() && !pit->second.empty()) {
                    std::string params = pit->second;
                    std::stringstream ss(params);
                    std::string param;
                    while (std::getline(ss, param, ',')) {
                        initParams.push_back(param);
                    }
                } else {
                    // B6: Check base classes for __init__ parameters
                    // Find the first base class that has __init__ defined
                    for (const auto& base : node->args) {
                        if (base.empty() || base == "(complex base)") continue;
                        auto basePit = classInitParams.find(base);
                        if (basePit != classInitParams.end() && !basePit->second.empty()) {
                            std::string params = basePit->second;
                            std::stringstream ss(params);
                            std::string param;
                            while (std::getline(ss, param, ',')) {
                                initParams.push_back(param);
                            }
                            break;
                        }
                    }
                    if (initParams.empty()) {
                        initParams.push_back("self");
                    }
                }
                ir.addFunction(initName, initParams);
                knownIRFunctions.insert(initName);
                // Collect user-provided positional args (skip the class name child[0]).
                std::vector<std::string> userArgs;
                for (size_t i = 1; i < node->children.size(); ++i) {
                    if (node->children[i] && node->children[i]->type != "Keyword") {
                        userArgs.push_back(lowerExpr(node->children[i].get()));
                    }
                }
                // Pad with defaults for trailing params that lack user args.
                // Defaults for the class's __init__ were registered (in the
                // FunctionDef lowering of `__init__`) as module globals named
                // __default___init__<i> in the order they appear. The total
                // number of params (incl. self) minus userArgs minus 1 (self)
                // gives the number of trailing defaults to inject.
                size_t totalParams = initParams.size();
                size_t provided = userArgs.size();
                size_t trailing = (totalParams > provided + 1) ? (totalParams - provided - 1) : 0;
                // Look up the defaults registered for the underlying __init__.
                // funcDefaultValues stores them in order; the *last* N are the
                // trailing defaults (consistent with Python: defaults apply to
                // the last N parameters).
                auto dit = funcDefaultValues.find("__init__");
                std::vector<std::string> defaults;
                if (dit != funcDefaultValues.end()) defaults = dit->second;
                // Build args list: self + user args + injected defaults.
                std::vector<std::string> callArgs;
                callArgs.push_back(initName);
                callArgs.push_back(instanceDict);
                for (const auto& a : userArgs) callArgs.push_back(a);
                for (size_t i = 0; i < trailing && i < defaults.size(); ++i) {
                    size_t di = defaults.size() - trailing + i;
                    if (di < defaults.size()) callArgs.push_back(defaults[di]);
                }
                std::string initCallRes = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", callArgs, initCallRes);
                return instanceDict;
            }
        }
        if (!node->children.empty() && node->children[0] && node->children[0]->type == "Name") {
            funcName = node->children[0]->id;
        } else {
            funcName = "";
        }
        // If the callee is a literal lambda expression, lower it first (this
        // registers the synthetic nested function and any defaults). The
        // returned name is the IR function to call. We bypass the "value" path
        // for direct (lambda)(args) so we don't treat the token as an arg.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Lambda") {
            funcName = lowerLambda(node->children[0].get());
        } else {
            std::string rawName = (node->children.empty() || !node->children[0]) ? "" : node->children[0]->id;
            funcName = rawName;
            // Resolve lambda aliases for assigned lambdas: "f = lambda ...; f(...)"
            auto ait = lambdaAliases.find(rawName);
            if (ait != lambdaAliases.end()) funcName = ait->second;

            // B4: if the callee expression lowers to a "callable token" (string const
            // holding a synthetic name, e.g. from a previous lambda expr, assignment,
            // or container element that was a lambda), use the synthetic as the target.
            // This allows lambdas used as values to be called when they appear as the
            // callee expression.
            // Skip for plain Names already resolved to a known direct IR function:
            // lowering those would just emit a dead callable-token const per call site.
            if (!node->children.empty() && node->children[0] &&
                !(node->children[0]->type == "Name" && knownIRFunctions.count(funcName))) {
                std::string calleeVal = lowerExpr(node->children[0].get());
                auto tit = callableTokenToSynthetic.find(calleeVal);
                if (tit != callableTokenToSynthetic.end()) {
                    funcName = tit->second;
                }
            }
        }

        // Compute lowered callee value early (needed for indirect detection before processing *).
        std::string calleeValEarly;
        if (!node->children.empty() && node->children[0] &&
            !(node->children[0]->type == "Name" && knownIRFunctions.count(funcName))) {
            calleeValEarly = lowerExpr(node->children[0].get());
        }
        // Re-check token map (in case the early lower produced the const temp for a lambda value).
        if (!calleeValEarly.empty()) {
            auto tit = callableTokenToSynthetic.find(calleeValEarly);
            if (tit != callableTokenToSynthetic.end()) {
                funcName = tit->second;
            }
        }

        bool isDirectNameEarly = (!node->children.empty() && node->children[0] && node->children[0]->type == "Name");
        auto knownIt0 = knownIRFunctions.find(funcName);
        bool knownDirect0 = (knownIt0 != knownIRFunctions.end());

        bool useDynamicApply = false;
        std::string tokenTempForApply;
        // Names we have special lowering/rewrites for in lowerCall (print, len, range, min/max,
        // sum, sorted, any/all, isinstance, int/float/abs/str, list, enumerate, zip, etc.).
        // These must never be turned into dynamic Pyc_Apply(token) calls; they must go through
        // their direct special paths (and have their args collected into argRes normally).
        static const std::unordered_set<std::string> specialBuiltinNames = {
            "print", "len", "range", "min", "max", "sum", "sorted", "any", "all", "isinstance",
            "int", "float", "abs", "str", "list", "enumerate", "zip",
            "bool", "type", "id", "repr", "hex", "oct", "bin", "ord", "chr", "round"
        };

        if (!knownDirect0) {
            if (isDirectNameEarly) {
                std::string theName = node->children[0] ? node->children[0]->id : "";
                // Names that must never be turned into a dynamic Pyc_Apply(token) call.
                // These have dedicated fast/special lowering paths in lowerCall and must
                // collect args normally into argRes.
                static const std::unordered_set<std::string> neverDynamic = {
                    "print","len","range","min","max","sum","sorted","any","all","isinstance",
                    "int","float","complex","abs","str","list","enumerate","zip","bool","type","id",
                    "repr","hex","oct","bin","ord","chr","round","open"
                };
                if (!theName.empty() && neverDynamic.count(theName) == 0) {
                    // B4 complete: any bare name that is not a known direct IR function *and*
                    // is not one of our special builtin shims is treated as a carrier of a
                    // callable token at runtime. We route the call via Pyc_Apply, passing the
                    // runtime value of that name as the token string. This makes "f = lambda ...; f()",
                    // "add5 = make_adder(5); add5(7)", "fns[0](x)", "make_adder(10)(20)",
                    // parameters holding lambdas, etc. all work uniformly.
                    // Regular user "def" calls stay direct (their names are pre-populated in
                    // knownIRFunctions). Special builtins keep their fast/special paths.
                    useDynamicApply = true;
                    if (isCellBackedHere(theName)) {
                        // Cell-backed callee (closure free variable holding a
                        // callable, e.g. a decorator wrapper's captured fn):
                        // the bare name has no direct slot here — fetch the
                        // cell content and apply that.
                        std::string cellVal = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyCell_Get", theName + "_cell"}, cellVal);
                        tokenTempForApply = cellVal;
                    } else {
                        tokenTempForApply = theName;
                    }
                }
            } else if (!calleeValEarly.empty()) {
                // Non-plain-name callee expression (subscript, attribute, result of a call
                // that returns a lambda, etc.) -- use its lowered value as the token for
                // the dynamic Pyc_Apply path.
                useDynamicApply = true;
                tokenTempForApply = calleeValEarly;
            }
        }
        bool isIndirectCallee = useDynamicApply;

        // B4: if the *lowered value* of the callee expression is a tracked callable token temp
        // (from a lambda expr, or a call result we marked because the callee function returns
        // lambdas, or subscript from a list we marked, etc.), force the dynamic path and use
        // that value as the token for Pyc_Apply. This covers direct expression cases like
        // "make_adder(10)(20)" where the callee is the result temp of the inner call.
        if (!isIndirectCallee && !calleeValEarly.empty() &&
            (callableTokenTemps.count(calleeValEarly) || callableTokenToSynthetic.count(calleeValEarly))) {
            useDynamicApply = true;
            tokenTempForApply = calleeValEarly;
            isIndirectCallee = true;
        }

        // For indirect callees (lambdas-as-values via tokens), we build the argument
        // list for Pyc_Apply directly here so that Starred dynamic * can splice into it
        // without routing through a __va wrapper (which requires a static target name).
        std::string indirectArgListTemp; // if non-empty, this list is passed to Pyc_Apply
        bool buildingIndirectArgs = false;
        if (isIndirectCallee) {
            std::string z = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, z);
            indirectArgListTemp = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, indirectArgListTemp);
            buildingIndirectArgs = true;
        }

        std::vector<std::string> argRes;
        std::vector<std::pair<std::string, std::string>> kwArgs; // (name, value)
        std::vector<std::string> kwargDicts; // dicts from **kwargs unpacking
        bool hadRuntimeStar = false; // true if this call used * with a non-literal (dynamic splice via __va wrapper)

        for (size_t i = 1; i < node->children.size(); ++i) {
            if (!node->children[i]) continue;
            if (node->children[i]->type == "Keyword") {
                if (!node->children[i]->children.empty()) {
                    std::string val = lowerExpr(node->children[i]->children[0].get());
                    // **kwargs unpacking: keyword with empty name means unpack dict as kwargs
                    if (node->children[i]->id.empty()) {
                        kwargDicts.push_back(val);
                    } else {
                        kwArgs.emplace_back(node->children[i]->id, val);
                    }
                }
            } else if (node->children[i]->type == "Starred" &&
                        !node->children[i]->children.empty()) {
                // *args at call site:
                // 1) If the starred source is a tracked list/tuple literal name
                //    (from a prior Assign of List/Tuple in this scope), statically
                //    expand its elements as separate operands. Exact arity, normal
                //    default/keyword/callee-* handling applies afterward.
                // 2) If the child of Starred is itself a direct List or Tuple
                //    literal expression, also statically expand (very common for
                //    func(*[1,2,3]) etc.). This avoids the runtime splice + wrapper
                //    for the common literal case.
                // 3) Otherwise (dynamic / name not tracked), do a runtime splice
                //    into a collected list and route the call via a generated
                //    __va_<target> wrapper (see ensureVaWrapper + emitForwardCallFromList).
                std::string starSrc = node->children[i]->children[0] ? node->children[i]->children[0]->id : std::string();
                auto litIt = listLiteralElemASTs.find(starSrc);
                const ASTNode* starChild = node->children[i]->children[0].get();
                if (litIt != listLiteralElemASTs.end()) {
                    for (auto* elemAst : litIt->second) {
                        argRes.push_back(lowerExpr(elemAst));
                    }
                } else if (starChild && (starChild->type == "List" || starChild->type == "Tuple")) {
                    // Direct literal in * position: static expand.
                    for (auto& ch : starChild->children) {
                        argRes.push_back(lowerExpr(ch.get()));
                    }
                } else {
                    // Dynamic case: runtime splice.
                    hadRuntimeStar = true;
                    std::string lst = lowerExpr(starChild);
                    if (buildingIndirectArgs) {
                        // Indirect callee (lambda-as-value via token, possibly passed as param or in a container).
                        // Flush any fixed prefix collected before this * into the indirect list,
                        // then splice the starred list's contents into the same indirect list.
                        for (auto& p : argRes) {
                            if (!p.empty()) {
                                std::string d = "t" + std::to_string(tempCounter++);
                                ir.addInstruction(currentFunc, "call", {"PyList_Append", indirectArgListTemp, p}, d);
                            }
                        }
                        std::string ln = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", lst}, ln);
                        std::string lnSlotI = "__sl_" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "assign", {ln}, lnSlotI);
                        std::string jv = "s" + std::to_string(tempCounter++);
                        std::string j0 = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"0"}, j0);
                        ir.addInstruction(currentFunc, "assign", {j0}, jv);
                        int sc = tempCounter++;
                        std::string slp = "istar_lp_" + std::to_string(sc);
                        std::string sbd = "istar_bd_" + std::to_string(sc);
                        std::string sex = "istar_ex_" + std::to_string(sc);
                        ir.addInstruction(currentFunc, "br", {}, slp);
                        ir.addInstruction(currentFunc, "label", {}, slp);
                        std::string cm = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "icmp", {"Lt", jv, lnSlotI}, cm);
                        ir.addInstruction(currentFunc, "br", {cm, sbd, sex});
                        ir.addInstruction(currentFunc, "label", {}, sbd);
                        std::string el = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", lst, jv}, el);
                        std::string dmy = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", indirectArgListTemp, el}, dmy);
                        std::string one = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"1"}, one);
                        std::string nj = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "add", {jv, one}, nj);
                        ir.addInstruction(currentFunc, "assign", {nj}, jv);
                        ir.addInstruction(currentFunc, "br", {}, slp);
                        ir.addInstruction(currentFunc, "label", {}, sex);
                        // Nothing is pushed to argRes; the indirect Pyc_Apply path will use indirectArgListTemp.
                    } else {
                        // Direct target: original dynamic * path using a __va_<target> wrapper.
                        std::string ln = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", lst}, ln);
                        std::string lnSlotD = "__sl_" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "assign", {ln}, lnSlotD);
                        std::string jv = "s" + std::to_string(tempCounter++);
                        std::string j0 = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"0"}, j0);
                        ir.addInstruction(currentFunc, "assign", {j0}, jv);
                        int sc = tempCounter++;
                        std::string slp = "star_lp_" + std::to_string(sc);
                        std::string sbd = "star_bd_" + std::to_string(sc);
                        std::string sex = "star_ex_" + std::to_string(sc);
                        // Seed va list with fixed prefix so far (positionals before the *)
                        std::string va = "t" + std::to_string(tempCounter++);
                        std::string pn = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {std::to_string(argRes.size())}, pn);
                        ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", pn}, va);
                        for (auto& p : argRes) {
                            if (!p.empty()) {
                                std::string d = "t" + std::to_string(tempCounter++);
                                ir.addInstruction(currentFunc, "call", {"PyList_Append", va, p}, d);
                            }
                        }
                        ir.addInstruction(currentFunc, "br", {}, slp);
                        ir.addInstruction(currentFunc, "label", {}, slp);
                        std::string cm = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "icmp", {"Lt", jv, lnSlotD}, cm);
                        ir.addInstruction(currentFunc, "br", {cm, sbd, sex});
                        ir.addInstruction(currentFunc, "label", {}, sbd);
                        std::string el = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", lst, jv}, el);
                        std::string dmy = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", va, el}, dmy);
                        std::string one = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"1"}, one);
                        std::string nj = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "add", {jv, one}, nj);
                        ir.addInstruction(currentFunc, "assign", {nj}, jv);
                        ir.addInstruction(currentFunc, "br", {}, slp);
                        ir.addInstruction(currentFunc, "label", {}, sex);
                        // Route this call through the __va wrapper for the target.
                        ensureVaWrapper(funcName);
                        funcName = "__va_" + funcName;
                        argRes.clear();
                        argRes.push_back(va);
                    }
                }
            } else {
                if (buildingIndirectArgs) {
                    std::string v = lowerExpr(node->children[i].get());
                    std::string d = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", indirectArgListTemp, v}, d);
                } else {
                    argRes.push_back(lowerExpr(node->children[i].get()));
                }
            }
        }
        // Callee-side *args collection (skip for runtime * call sites; the __va wrapper
        // already forwards the correct fixed+tail shape for the target).
        if (!hadRuntimeStar) {
            auto pit = funcParamNames.find(funcName);
            if (pit != funcParamNames.end()) {
                const auto& params = pit->second;
                size_t vidx = (size_t)-1;
                for (size_t j = 0; j < params.size(); ++j) {
                    if (!params[j].empty() && params[j][0] == '*') { vidx = j; break; }
                }
                if (vidx != (size_t)-1) {
                    size_t fixed = vidx;
                    std::vector<std::string> tail;
                    while (argRes.size() > fixed) {
                        tail.push_back(argRes.back());
                        argRes.pop_back();
                    }
                    std::reverse(tail.begin(), tail.end());
                    std::string collected;
                    // Always start empty and append; pre-sizing + append would leave
                    // initial null slots (visible as None) and double the length.
                    std::string z = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, z);
                    collected = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, collected);
                    for (auto& t : tail) {
                        std::string d = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", collected, t}, d);
                    }
                    if (argRes.size() < fixed) argRes.resize(fixed, "");
                    argRes.push_back(collected);
                }
            }
        }

        // Save the count of pure positional args BEFORE the kwarg-mixing code
        // below possibly appends kwarg values to argRes. The print fast-path
        // needs the positional-only count to build the right list size.
        const size_t posArgCount = argRes.size();

        // Handle keyword arguments by mapping to parameter positions
        if (!kwArgs.empty() || !kwargDicts.empty()) {
            auto pit = funcParamNames.find(funcName);
            if (pit != funcParamNames.end()) {
                const auto& params = pit->second;
                // First, apply regular keyword arguments
                for (auto& kw : kwArgs) {
                    for (size_t j = 0; j < params.size(); ++j) {
                        if (params[j] == kw.first) {
                            if (argRes.size() <= j) argRes.resize(j + 1);
                            argRes[j] = kw.second;
                            break;
                        }
                    }
                }
                // Then, expand **kwargs dicts using a runtime helper
                for (auto& dictVal : kwargDicts) {
                    // Call Pyc_ExpandKwargs(dict, param1, param2, ...) -> list of args
                    std::string expandResult = "t" + std::to_string(tempCounter++);
                    std::vector<std::string> expandArgs;
                    expandArgs.push_back(dictVal);
                    for (const auto& p : params) {
                        std::string paramConst = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + p + "\""}, paramConst, "str");
                        expandArgs.push_back(paramConst);
                    }
                    std::vector<std::string> callOperands;
                    callOperands.push_back("Pyc_ExpandKwargs");
                    for (const auto& a : expandArgs) callOperands.push_back(a);
                    ir.addInstruction(currentFunc, "call", callOperands, expandResult);
                    // Now unpack the result list into argRes
                    // The result list has len(params) elements, in order
                    for (size_t j = 0; j < params.size(); ++j) {
                        std::string idx = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {std::to_string(j)}, idx);
                        std::string elem = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", expandResult, idx}, elem);
                        if (argRes.size() <= j) argRes.resize(j + 1);
                        argRes[j] = elem;
                    }
                }
            } else {
                // Fallback: append keyword values
                for (auto& kw : kwArgs) argRes.push_back(kw.second);
                for (auto& dictVal : kwargDicts) argRes.push_back(dictVal);
            }
        }

        // *args collection at the call site: splice the iterable's elements
        // as additional positional args. We lower the starred expression
        // to a list value, then emit a tiny runtime-assisted unpack using
        // the existing list machinery (size + loop of GetItem) right here
        // so the callee receives true extra positional arguments.
        // If a Starred node appears in the original children we rewrite
        // argRes by expanding it inline before default injection.
        // Simpler approach used below: detect Starred in the AST children
        // of the Call and splice using list iteration at lowering time.
        // Default-arg injection (skip for runtime * call sites routed via __va).
        // Defaults in the AST correspond only to the regular (non-* / non-**)
        // positional-or-keyword parameters, as the suffix of those regular params.
        // We must compute slots relative to the regular prefix, not the full
        // params list (which may contain *vararg / **kwarg markers).
        if (!hadRuntimeStar) {
            auto dit = funcDefaultValues.find(funcName);
            auto pit = funcParamNames.find(funcName);
            if (dit != funcDefaultValues.end() && pit != funcParamNames.end()) {
                const auto& params = pit->second;
                const auto& defaults = dit->second;
                size_t ndefaults = defaults.size();
                if (ndefaults > 0) {
                    // Find first * marker (vararg); regular params are before it.
                    size_t first_star = params.size();
                    for (size_t j = 0; j < params.size(); ++j) {
                        if (!params[j].empty() && params[j][0] == '*') {
                            first_star = j; break;
                        }
                    }
                    size_t nregular = first_star;
                    // After call-site * handling and callee-side collection,
                    // argRes may contain entries for regular params + a collected
                    // list for a * slot (at logical position first_star).
                    // Ensure we have slots for the regular params.
                    if (argRes.size() < nregular) argRes.resize(nregular, "");
                    // Fill defaults into the suffix of the regular section.
                    for (size_t i = 0; i < ndefaults; ++i) {
                        size_t reg_idx = nregular - ndefaults + i;
                        if (reg_idx < argRes.size() && argRes[reg_idx].empty()) {
                            argRes[reg_idx] = defaults[i];
                        }
                    }
                }
                // Strip any trailing empty slots (shouldn't happen but be safe).
                while (!argRes.empty() && argRes.back().empty()) argRes.pop_back();
            } else if (argRes.empty() && kwArgs.empty()) {
                // No param info at all — fall back to using all defaults.
                auto it = funcDefaultValues.find(funcName);
                if (it != funcDefaultValues.end()) {
                    argRes = it->second;
                }
            }
            // 0-supplied direct call to a known function that has defaults:
            // pad trailing defaults so the callee receives them even with 0 user args.
            if (!hadRuntimeStar) {
                auto dit = funcDefaultValues.find(funcName);
                auto pit = funcParamNames.find(funcName);
                if (dit != funcDefaultValues.end() && pit != funcParamNames.end()) {
                    const auto& params = pit->second;
                    const auto& defaults = dit->second;
                    size_t ndefaults = defaults.size();
                    if (ndefaults > 0) {
                        size_t first_star = params.size();
                        for (size_t j = 0; j < params.size(); ++j) {
                            if (!params[j].empty() && params[j][0] == '*') { first_star = j; break; }
                        }
                        size_t nregular = first_star;
                        if (argRes.size() < nregular) argRes.resize(nregular, "");
                        for (size_t i = 0; i < ndefaults; ++i) {
                            size_t reg_idx = nregular - ndefaults + i;
                            if (reg_idx < argRes.size() && argRes[reg_idx].empty()) {
                                argRes[reg_idx] = defaults[i];
                            }
                        }
                        while (!argRes.empty() && argRes.back().empty()) argRes.pop_back();
                    }
                }
            }
        }

        // print() with no positional args and no kwargs → bare newline.
        if (funcName == "print" && argRes.empty() && kwArgs.empty()) {
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_PrintNewline"}, res);
            return res;
        }

        // print(a, b, c, ... [, sep=X, end=Y]) — build a Python list of args
        // and call pyc_print(list, sep, end). The runtime handles joining and
        // the final newline/end suffix. This honors sep= and end= correctly.
        if (funcName == "print") {
            // Use posArgCount to avoid including kwarg values that may have
            // been appended to argRes by the kwarg-mapping code above.
            const size_t n = posArgCount;
            // Build the args list. We use the boxed list runtime so the args
            // are reference-counted like any other list.
            std::string sizeConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
            std::string argList = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sizeConst}, argList);
            for (size_t i = 0; i < n; ++i) {
                std::string idx = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idx);
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idx, argRes[i]}, dummy);
            }
            // Resolve sep/end from kwArgs. Default to null (runtime uses " " and "\n").
            std::string sepVal;  // empty == null arg → use runtime default
            std::string endVal;
            bool sepGiven = false, endGiven = false;
            for (const auto& kv : kwArgs) {
                if (kv.first == "sep")      { sepVal = kv.second; sepGiven = true; }
                else if (kv.first == "end") { endVal = kv.second; endGiven = true; }
                // other kwargs (file, flush) are ignored — compiler doesn't support them yet
            }
            // Emit a real nconst (null) when not given. Use distinct temps to avoid clashes.
            std::string sepArg;
            if (sepGiven) {
                sepArg = sepVal;
            } else {
                sepArg = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "nconst", {}, sepArg, "none");
            }
            std::string endArg;
            if (endGiven) {
                endArg = endVal;
            } else {
                endArg = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "nconst", {}, endArg, "none");
            }
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"pyc_print", argList, sepArg, endArg}, res);
            return res;
        }

        // len(obj) → PyBuiltin_Len(obj)
        if (funcName == "len") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Len", arg}, res);
            noteType(res, "i64");
            return res;
        }

        // open(path, mode) — returns a fake "file" dict with __enter__,
        // __exit__, and write methods. The actual file is opened by
        // PyBuiltin_Open (which stores the FILE* in a synthetic file
        // struct accessible via the PyObject pointer). The returned
        // dict is annotated as "dict" so the with-statement and
        // method-call dispatch find the entries.
        if (funcName == "open") {
            std::string path = argRes.size() > 0 ? argRes[0] : "";
            std::string mode = argRes.size() > 1 ? argRes[1] : "";
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Open", path, mode}, res);
            noteType(res, "dict");
            return res;
        }

        // min/max — fold pairwise; single list arg uses list variant
        if (funcName == "min" || funcName == "max") {
            std::string fn2  = (funcName == "min") ? "PyBuiltin_Min2"    : "PyBuiltin_Max2";
            std::string fnLst = (funcName == "min") ? "PyBuiltin_MinList" : "PyBuiltin_MaxList";
            if (argRes.size() == 1) {
                std::string res = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {fnLst, argRes[0]}, res);
                noteType(res, "boxed");
                return res;
            }
            std::string acc = argRes[0];
            for (size_t i = 1; i < argRes.size(); ++i) {
                std::string res2 = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {fn2, acc, argRes[i]}, res2);
                noteType(res2, "boxed");
                acc = res2;
            }
            noteType(acc, "boxed");
            return acc;
        }
        // list(x) → PyBuiltin_List(x)
        if (funcName == "list") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            if (argRes.empty()) {
                std::string sc = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, sc);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sc}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", arg}, res);
            }
            // S3/S4: Propagate element type from argument to result for container ops
            std::string argType = typeOf(arg);
            std::string listElemType = "boxed";
            // Direct typed list args
            if (argType == "list_int") {
                listElemType = "int";
                noteType(res, "list_int");
            } else if (argType == "list_float") {
                listElemType = "float";
                noteType(res, "list_float");
            } 
            // S4: values()-typed list (from dict.valueTypes)
            else if (argType == "list_values_typed" && tempContainerElementTypes.count(arg)) {
                listElemType = tempContainerElementTypes[arg];
                noteType(res, "list");
            }
            else {
                noteType(res, "list");
            }
            return res;
        }
        // reversed(seq) → PyBuiltin_Reversed(seq)
        if (funcName == "reversed") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Reversed", arg}, res);
            noteType(res, "list");
            // S3: Propagate element type
            std::string argType = typeOf(arg);
            if (argType == "list_int") noteType(res, "list_int");
            else if (argType == "list_float") noteType(res, "list_float");
            return res;
        }
        // enumerate(iterable) → PyBuiltin_Enumerate(iterable)
        if (funcName == "enumerate") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Enumerate", arg}, res);
            noteType(res, "list");
            // S3: enumerate returns tuples, always boxed
            (void)typeOf(arg);
            return res;
        }
        // zip(a, b) → PyBuiltin_Zip2(a, b)
        if (funcName == "zip" && argRes.size() >= 2) {
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Zip2", argRes[0], argRes[1]}, res);
            noteType(res, "list");
            // S3: zip returns tuples, always boxed
            return res;
        }

        // sum(iterable) → PyBuiltin_Sum
        if (funcName == "sum") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Sum", arg}, res);
            noteType(res, "boxed");
            return res;
        }
        // cmp_to_key(cmp) → PyBuiltin_CmpToKey(cmp)
        // Returns a dict token that sorted recognizes for the fast-path.
        if (funcName == "cmp_to_key" && argRes.size() >= 1) {
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_CmpToKey", argRes[0]}, res);
            noteType(res, "dict");
            return res;
        }
        // sorted(iterable) → PyBuiltin_Sorted(iterable, null)
        // sorted(iterable, key=fn) → PyBuiltin_Sorted(iterable, fn)
        // sorted(iterable, key=cmp_to_key(cmp)) → PyBuiltin_SortedWithCmp(iterable, cmp)
        //   The last form is a special case: instead of producing a key
        //   function that returns K pairs, we pass the comparator directly
        //   to a separate runtime entry point that sorts the items via
        //   the comparator.
        if (funcName == "sorted") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string argType = typeOf(arg);
            std::string res = "t" + std::to_string(tempCounter++);
            // Find the key argument (positional or keyword).
            std::string keyName;
            for (const auto& kv : kwArgs) {
                if (kv.first == "key") { keyName = kv.second; break; }
            }
            if (keyName.empty() && argRes.size() >= 2) keyName = argRes[1];
            // Detect the cmp_to_key(cmp) pattern: a Call to cmp_to_key
            // (direct, not via an alias) with one positional arg. The
            // call may be a positional arg or a keyword arg's value.
            std::string cmpArg;
            std::function<void(const ASTNode*)> findCmpToKey = [&](const ASTNode* c) {
                if (!c) return;
                if (c->type == "Keyword" && c->children.size() == 1) {
                    findCmpToKey(c->children[0].get());
                    return;
                }
                if (c->type == "Call" && !c->children.empty() && c->children[0] &&
                    c->children[0]->type == "Name" && c->children[0]->id == "cmp_to_key" &&
                    c->children.size() >= 2) {
                    // The comparator is the (lowered) value of the
                    // first positional arg of cmp_to_key(...).
                    cmpArg = lowerExpr(c->children[1].get());
                    return;
                }
            };
            for (size_t i = 1; i < node->children.size(); ++i) {
                findCmpToKey(node->children[i].get());
                if (!cmpArg.empty()) break;
            }
            if (!cmpArg.empty()) {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_SortedWithCmp", arg, cmpArg}, res);
            } else {
                std::string keyArg = keyName.empty() ? "" : keyName;
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Sorted", arg, keyArg}, res);
            }
            noteType(res, "list");
            // S3: Propagate element type from argument to sorted result
            if (argType == "list_int") noteType(res, "list_int");
            else if (argType == "list_float") noteType(res, "list_float");
            return res;
        }
        // any(iterable) → PyBuiltin_Any (bool result)
        if (funcName == "any") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Any", arg}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // all(iterable) → PyBuiltin_All (bool result)
        if (funcName == "all") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_All", arg}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // isinstance(obj, classinfo) → Pyc_IsInstance
        if (funcName == "isinstance" && argRes.size() >= 2) {
            // If classinfo is a known type name, pass a numeric typecode
            // (0=int, 4=float, 3=str, 1=list, 2=dict, 5=bool, 6=NoneType)
            // so the runtime can dispatch without a real type system.
            int typecode = -1;
            // children[0] = func (isinstance), children[1] = obj,
            // children[2] = classinfo
            if (node->children.size() >= 3 && node->children[2]) {
                std::string childType = node->children[2]->type;
                std::string childVal = node->children[2]->value;
                std::string childId  = node->children[2]->id;
                if (childType == "Name" || childType == "Constant") {
                    const std::string& n = childType == "Name" ? childId : childVal;
                    if      (n == "int")  typecode = 0;
                    else if (n == "float") typecode = 4;
                    else if (n == "str")  typecode = 3;
                    else if (n == "list") typecode = 1;
                    else if (n == "dict") typecode = 2;
                    else if (n == "bool") typecode = 5;
                    else if (n == "NoneType" || n == "type") typecode = 6;
                } else if (childType == "Call" && node->children[2]->children.size() >= 2) {
                    // type(None) → typecode 6
                    const auto* func = node->children[2]->children[0].get();
                    const auto* arg0 = node->children[2]->children[1].get();
                    if (func && func->type == "Name" && func->id == "type" &&
                        arg0 && arg0->type == "Constant" && arg0->value == "None") {
                        typecode = 6;
                    }
                }
            }
            std::string res = "t" + std::to_string(tempCounter++);
            if (typecode >= 0) {
                std::string tc = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(typecode)}, tc, "int");
                ir.addInstruction(currentFunc, "call", {"Pyc_IsInstance", argRes[0], tc}, res, "bool");
            } else {
                ir.addInstruction(currentFunc, "call", {"Pyc_IsInstance", argRes[0], argRes[1]}, res, "bool");
            }
            noteType(res, "bool");
            return res;
        }

        // int(x) or int(x, base) → PyBuiltin_Int / PyBuiltin_IntBase
        if (funcName == "int") {
            std::string res = "t" + std::to_string(tempCounter++);
            if (argRes.size() >= 2) {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_IntBase", argRes[0], argRes[1]}, res, "int");
            } else {
                std::string arg = argRes.empty() ? "" : argRes[0];
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Int", arg}, res, "int");
            }
            noteType(res, "int");
            return res;
        }
        // float(x) → PyBuiltin_Float(x)
        if (funcName == "float") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Float", arg}, res, "float");
            noteType(res, "float");
            return res;
        }
        // complex(x) or complex(x, y) → PyBuiltin_Complex(x, y)
        if (funcName == "complex") {
            std::string res = "t" + std::to_string(tempCounter++);
            std::string arg1 = argRes.empty() ? "" : argRes[0];
            std::string arg2 = argRes.size() >= 2 ? argRes[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Complex", arg1, arg2}, res, "boxed");
            complexVars.insert(res);
            noteType(res, "boxed");
            return res;
        }
        // abs(x) → PyBuiltin_Abs(x) or PyComplex_Abs(x) for complex
        if (funcName == "abs") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            // Check if arg is complex (type 13, boxed)
            // Complex values are always boxed, so we check if the arg was produced by complex literal lowering
            // For now, use a heuristic: if resultType would be "boxed" and we can't determine it's int/float,
            // check if it might be complex. Actually, we need to track complex types.
            // Simple approach: always use PyComplex_Abs if the arg is boxed — the runtime will type-check.
            // Better: check if arg was produced by complex literal (has a specific temp pattern or annotation)
            // For now, emit PyComplex_Abs for boxed args — runtime handles type checking
            std::string resultType = typeOf(arg);
            if (resultType == "boxed") {
                // Could be complex — try PyComplex_Abs first
                ir.addInstruction(currentFunc, "call", {"PyComplex_Abs", arg}, res);
                noteType(res, "float");  // abs(complex) returns float
                return res;
            }
            if (resultType != "int" && resultType != "float" && resultType != "bool") {
                resultType = "boxed";
            }
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Abs", arg}, res, resultType);
            noteType(res, resultType);
            return res;
        }

        // ord(c) → PyBuiltin_Ord(c)
        if (funcName == "ord") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Ord", arg}, res, "int");
            noteType(res, "int");
            return res;
        }
        // chr(i) → PyBuiltin_Chr(i)
        if (funcName == "chr") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Chr", arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // bool(x) → PyBuiltin_Bool(x)
        if (funcName == "bool") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Bool", arg}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // type(x) → PyBuiltin_Type(x)  (returns a string like "<class 'int'>")
        if (funcName == "type") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Type", arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // hex/oct/bin(x) — string with 0x/0o/0b prefix
        if (funcName == "hex" || funcName == "oct" || funcName == "bin") {
            std::string helper = (funcName == "hex") ? "PyBuiltin_Hex"
                                : (funcName == "oct") ? "PyBuiltin_Oct"
                                : "PyBuiltin_Bin";
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {helper, arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // id(obj) → PyBuiltin_Id(obj)
        if (funcName == "id") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Id", arg}, res, "int");
            noteType(res, "int");
            return res;
        }
        // divmod(a, b) → PyBuiltin_Divmod(a, b) — returns a 2-element list
        if (funcName == "divmod") {
            std::string a = argRes.size() > 0 ? argRes[0] : "";
            std::string b = argRes.size() > 1 ? argRes[1] : "";
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Divmod", a, b}, res, "list");
            noteType(res, "list");
            return res;
        }
        // repr(obj) → PyBuiltin_Repr(obj) (returns a string)
        if (funcName == "repr") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Repr", arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // round(x [, n]) → PyBuiltin_Round(x, n_or_null)
        if (funcName == "round") {
            std::string x = argRes.empty() ? "" : argRes[0];
            std::string n = argRes.size() > 1 ? argRes[1] : "";
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Round", x, n}, res);
            noteType(res, "boxed");
            return res;
        }
        // pow(base, exp) → PyBuiltin_Pow(base, exp)
        if (funcName == "pow") {
            std::string a = argRes.size() > 0 ? argRes[0] : "";
            std::string b = argRes.size() > 1 ? argRes[1] : "";
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Pow", a, b}, res);
            noteType(res, "boxed");
            return res;
        }

        // str(obj) → PyStr_FromAny(obj)
        if (funcName == "str") {
            if (argRes.empty()) {
                std::string res = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"\""}, res);
                noteType(res, "str");
                return res;
            }
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyStr_FromAny", argRes[0]}, res);
            noteType(res, "str");
            return res;
        }

        // Normalize range(stop), range(start,stop), range(start,stop,step)
        // → always call PyBuiltin_Range(start, stop, step) with 3 PyObject* args
        if (funcName == "range") {
            std::string startRes, stopRes, stepRes;
            if (argRes.size() == 1) {
                startRes = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, startRes);
                stopRes  = argRes[0];
                stepRes  = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"1"}, stepRes);
            } else if (argRes.size() == 2) {
                startRes = argRes[0];
                stopRes  = argRes[1];
                stepRes  = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"1"}, stepRes);
            } else if (argRes.size() >= 3) {
                startRes = argRes[0];
                stopRes  = argRes[1];
                stepRes  = argRes[2];
            } else {
                // range() with no args → empty list
                startRes = stopRes = stepRes = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, startRes);
            }
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call",
                              {"PyBuiltin_Range", startRes, stopRes, stepRes}, res);
            noteType(res, "range_object");
            return res;
        }

        // For runtime *args (dynamic splice), we have already switched funcName
        // to the __va_<target> wrapper and argRes contains exactly the collected
        // list as a single operand. Emit the call to the wrapper directly.
        // B4/B8: decide whether to use direct call or dynamic dispatch via Pyc_Apply
        // (for lambdas-as-values, parameters holding tokens, subscripts producing tokens, etc.)
        // (useDynamicApply / tokenTempForApply declared earlier for indirect-callee detection)

        // Lower the callee expression to its value (important for Subscript, Name that holds a token, etc.)
        std::string calleeVal;
        if (!node->children.empty() && node->children[0]) {
            calleeVal = lowerExpr(node->children[0].get());
        }

        if (!isIndirectCallee) {
            // If the lowered callee value is a known callable token (string const from lambda expr), use it.
            auto tit = callableTokenToSynthetic.find(calleeVal);
            if (tit != callableTokenToSynthetic.end()) {
                funcName = tit->second;
                tokenTempForApply = calleeVal;
                useDynamicApply = false;
            }
        }

        // B5: if the callee value is a closure descriptor bundle, switch to
        // dynamic apply so we can extract the cells from the bundle and
        // prepend them to user args. The bundle is [token, cell0, cell1, ...].
        if (!calleeVal.empty() && bundleTemps.count(calleeVal)) {
            useDynamicApply = true;
            // The token is bundle[0]; we'll Pyc_GetItem it at the call site.
            tokenTempForApply.clear();
            auto bit = bundleToSynthetic.find(calleeVal);
            if (bit != bundleToSynthetic.end()) funcName = bit->second;
            isIndirectCallee = true;
        }

        bool isDirectName = (!node->children.empty() && node->children[0] && node->children[0]->type == "Name");
        auto knownIt = knownIRFunctions.find(funcName);
        bool knownDirect = (knownIt != knownIRFunctions.end());

        if (!useDynamicApply) {
            if (!isIndirectCallee) {
                auto tit2 = callableTokenToSynthetic.find(calleeVal);
                if (tit2 != callableTokenToSynthetic.end()) {
                    funcName = tit2->second;
                } else if (isDirectName && !knownDirect) {
                    // B4 complete: a bare name that is not a known direct IR function is a dynamic
                    // token carrier if we tracked it as holding a callable (via assign/unpack/return
                    // from a function that returns a lambda, subscript from a token list, etc.),
                    // *or* if it is a parameter of the current function (the token flows in via the arg).
                    // All other bare names stay on the direct path (normal user defs, forward refs, etc.).
                    bool isParamOfCurrent = false;
                    auto pit = funcParamNames.find(currentFunc);
                    if (pit != funcParamNames.end()) {
                        for (const auto& p : pit->second) {
                            std::string pn = p;
                            if (!pn.empty() && pn[0] == '*') pn = pn.substr(1);
                            if (pn == funcName) { isParamOfCurrent = true; break; }
                        }
                    }
                    if (isParamOfCurrent || namesThatMayHoldCallableTokens.count(funcName)) {
                        useDynamicApply = true;
                        tokenTempForApply = funcName;
                    }
                } else if (!knownDirect && !calleeVal.empty() && !isDirectName) {
                    // Non-plain-name callee expression (subscript, etc.) not known direct → dynamic with its value as token.
                    useDynamicApply = true;
                    tokenTempForApply = calleeVal;
                }
            }
        }

        // Seed the late decision from the early indirect detection (done before * processing
        // so that dynamic * under an indirect callee splices into the indirect list instead of
        // creating a __va for a param name).
        if (isIndirectCallee) {
            useDynamicApply = true;
            if (tokenTempForApply.empty() && !calleeValEarly.empty()) {
                tokenTempForApply = calleeValEarly;
            }
        }

        if (useDynamicApply) {
            // B5: detect closure-bundle callees — the callee value is a
            // descriptor list [token, cell0, cell1, ...]. Extract the
            // token (calleeVal[0]) and prepend the cells to argRes.
            std::string bundleCallee;
            // A+B fix: if the callee is a bare name (or value) that carries a bundle
            // (e.g. c1 = make_counter(); ... c1()), force bundle treatment here so that
            // we extract the real token and prepend the cells. Without this, we would
            // pass the bundle list (or the bare name) as the Pyc_Apply token.
            if (bundleCallee.empty()) {
                std::string bsrc;
                if (!calleeVal.empty() && (bundleTemps.count(calleeVal) || namesThatMayHoldBundles.count(calleeVal))) bsrc = calleeVal;
                else if (!funcName.empty() && (bundleTemps.count(funcName) || namesThatMayHoldBundles.count(funcName))) bsrc = funcName;
                if (!bsrc.empty()) {
                    std::string bc = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {bsrc}, bc);
                    bundleCallee = bc;
                    bundleTemps.insert(bc);
                    auto sit = bundleToSynthetic.find(bsrc);
                    if (sit != bundleToSynthetic.end()) bundleToSynthetic[bc] = sit->second;
                    auto dit = descriptorCells.find(bsrc);
                    if (dit != descriptorCells.end()) descriptorCells[bc] = dit->second;
                    tokenTempForApply.clear();
                }
            }
            if (!calleeVal.empty() && bundleTemps.count(calleeVal)) {
                // Use a fresh temp to hold the callee (avoids clashes with
                // existing slot names like "c1" or "c2" that the user code
                // may have assigned earlier — otherwise getOrLoad may pick
                // up the wrong value for the same name).
                std::string bc = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "assign", {calleeVal}, bc);
                bundleCallee = bc;
                bundleTemps.insert(bc);
                // Copy the metadata.
                auto sit = bundleToSynthetic.find(calleeVal);
                if (sit != bundleToSynthetic.end()) bundleToSynthetic[bc] = sit->second;
                auto dit = descriptorCells.find(calleeVal);
                if (dit != descriptorCells.end()) descriptorCells[bc] = dit->second;
            }
            // Build the argument list for Pyc_Apply. For indirect callees (including those with
            // dynamic *), we may have built a flat user-arg list (with * contents spliced) into
            // indirectArgListTemp during arg processing. Prefer that when present.
            std::string argList;
            // For a bundle callee, prepend the cells (indices 1..n) to the user-arg list.
            // For a non-bundle indirect callee, just build the user-arg list.
            if (!indirectArgListTemp.empty() && bundleCallee.empty()) {
                argList = indirectArgListTemp;
            } else {
                // Build a fresh arg list, prepending bundle cells (if any) followed by user args.
                std::string z = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, z);
                argList = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, argList);
                if (!bundleCallee.empty()) {
                    auto dit = descriptorCells.find(bundleCallee);
                    if (dit != descriptorCells.end()) {
                        int k = 0;
                        for (const auto& nm : dit->second) {
                            // bundle[1+k] is the k-th cell (a PyCell* PyObject).
                            std::string ic = "c" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "const", {std::to_string(1 + k)}, ic);
                            std::string cellObj = "t" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", bundleCallee, ic}, cellObj);
                            std::string d = "t" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, cellObj}, d);
                            ++k;
                        }
                    }
                }
                if (!indirectArgListTemp.empty()) {
                    // Splice the indirect user-arg list contents into our argList.
                    // (For dynamic * under a non-bundle indirect callee, the
                    // indirectArgListTemp holds the user args; we appended bundle
                    // cells above, so just iterate the list and append each item.)
                    std::string sz = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", indirectArgListTemp}, sz);
                    std::string idx = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, idx);
                    std::string szc = "__ipc_sz_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {sz}, szc);
                    std::string jc = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, jc);
                    std::string jslot = "__ipc_j_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {jc}, jslot);
                    int sc = tempCounter++;
                    std::string lp = "__ipc_lp_" + std::to_string(sc);
                    std::string bd = "__ipc_bd_" + std::to_string(sc);
                    std::string ex = "__ipc_ex_" + std::to_string(sc);
                    ir.addInstruction(currentFunc, "br", {}, lp);
                    ir.addInstruction(currentFunc, "label", {}, lp);
                    std::string cm = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "icmp", {"Lt", jslot, szc}, cm);
                    ir.addInstruction(currentFunc, "br", {cm, bd, ex});
                    ir.addInstruction(currentFunc, "label", {}, bd);
                    std::string el = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", indirectArgListTemp, jslot}, el);
                    std::string d = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, el}, d);
                    std::string oneC = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"1"}, oneC);
                    std::string newJ = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "add", {jslot, oneC}, newJ, "int");
                    ir.addInstruction(currentFunc, "assign", {newJ}, jslot);
                    ir.addInstruction(currentFunc, "br", {}, lp);
                    ir.addInstruction(currentFunc, "label", {}, ex);
                } else {
                    for (auto& v : argRes) {
                        std::string d = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, v}, d);
                    }
                }
            }
            std::string tok = tokenTempForApply;
            // Bundle callee: extract the token from bundle[0] (a string).
            if (!bundleCallee.empty()) {
                std::string zc = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, zc);
                tok = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", bundleCallee, zc}, tok);
            } else if (tok.empty() && !funcName.empty()) {
                tok = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + funcName + "\""}, tok, "str");
            }
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_Apply", tok, argList}, res);
            // The Pyc_GetItem on the bundle returns a borrowed ref to a
            // string token (the function name). Pyc_Apply does not
            // INCREF/DECREF the token, but it does keep a reference via
            // its g_callableRegistry. We can safely DECREF the borrowed
            // ref after the call.
            // B4: the result of an indirect call via Pyc_Apply may itself be a callable token
            // (e.g. a function that returns another lambda). Conservatively mark the result
            // temp; if it is later assigned or used as a callee we will treat it as a token.
            // (We cannot know the dynamic callee here, so we mark the call result as "may hold token"
            // for names and also insert it into callableTokenTemps so bare-name callees after
            // "x = some_call_that_returns_lambda(); x(...)" work.)
            callableTokenTemps.insert(res);
            return res;
        }

        // Normal direct call path (B5: may need to pass hidden cell objects for free nonlocals).
        // Also handle descriptor bundles at the callee expression (capturing lambdas-as-values).
        {
            std::vector<std::string> finalOps;
            // If the callee value is a descriptor bundle, splice its cells first, then user args.
            auto bit = bundleTemps.find(calleeVal);
            if (bit != bundleTemps.end()) {
                auto dit = descriptorCells.find(calleeVal);
                if (dit != descriptorCells.end()) {
                    int k = 0;
                    for (const auto& nm : dit->second) {
                        std::string ic = "c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {std::to_string(1 + k)}, ic);
                        std::string cellObj = "t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", calleeVal, ic}, cellObj);
                        finalOps.push_back(cellObj);
                        ++k;
                    }
                }
                // Resolve the real target synthetic for the call.
                auto sit = bundleToSynthetic.find(calleeVal);
                if (sit != bundleToSynthetic.end()) {
                    funcName = sit->second;
                }
            } else {
                // Existing free-cell path for direct named callees (defs that close over cells).
                auto fit = funcFreeCells.find(funcName);
                if (fit != funcFreeCells.end() && !fit->second.empty()) {
                    for (const auto& fc : fit->second) {
                        finalOps.push_back(fc + "_cell");
                    }
                }
            }
            finalOps.insert(finalOps.begin(), funcName);
            finalOps.insert(finalOps.end(), argRes.begin(), argRes.end());
            // A6: track call-site argument types for monomorphization.
            // Only track for direct calls to known functions (not builtins with special paths).
            if (knownIRFunctions.count(funcName) && !argRes.empty()) {
                std::vector<std::string> sig;
                for (size_t i = 0; i < argRes.size(); ++i) {
                    std::string t = typeOf(argRes[i]);
                    if (t == "i64") t = "int";
                    sig.push_back(t);
                }
                callSiteTypes[funcName].push_back(sig);
            }
            // Generator call: wrap with clear→call→get_buffer to materialize yields.
            bool isGenCall = generatorFunctions.count(funcName) > 0;
            if (isGenCall) {
                ir.addInstruction(currentFunc, "call", {"pyc_clear_yield_buffer"}, "");
            }
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", finalOps, res);
            if (isGenCall) {
                std::string genRes = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"pyc_get_yield_buffer"}, genRes);
                res = genRes;
            }
            // B4: if the callee is a function we have recorded as returning a callable token
            // (e.g. a Python function whose body does "return lambda ..."), mark the call result
            // so subsequent assign/unpack/call through that result can propagate tokens.
            if (functionsThatReturnCallables.count(funcName)) {
                callableTokenTemps.insert(res);
            }
            // B5: if the callee is known to return a bundle, mark the result accordingly
            // so later bare-name callees and assign targets can extract cells.
            if (functionsThatReturnBundles.count(funcName)) {
                bundleTemps.insert(res);
                auto sit = functionReturnedBundleSynthetic.find(funcName);
                if (sit != functionReturnedBundleSynthetic.end()) bundleToSynthetic[res] = sit->second;
                auto cit = functionReturnedBundleCaps.find(funcName);
                if (cit != functionReturnedBundleCaps.end()) descriptorCells[res] = cit->second;
            }
            return res;
        }
    }

    std::string lowerLambda(const ASTNode* node) {
        // Treat lambda as a nested function with a synthetic unique name.
        // The C++ return value is the synthetic IR function name (used for
        // direct call resolution via knownIRFunctions and lambdaAliases).
        // The *expression value* produced for the Lambda node (in lowerExpr)
        // is a string constant (the "callable token") holding the synthetic name.
        // This token can be assigned to names, passed as an argument, stored in
        // lists/dicts, returned from functions, and later used as a callee
        // expression. Calls through such tokens are routed via Pyc_Apply + the
        // generated __apply__<name> adapter (registered at module startup).
        // This completes the B4 "lambdas as values" model (string-token based,
        // with full support for *args in the lambda and dynamic * at the call site).
        // Full first-class function objects (with identity, __call__, cells for
        // closure over mutable variables, etc.) are out of scope for B4.
        static int lamCount = 0;
        std::string lamName = "__lambda_" + std::to_string(lamCount++);

        // Clean * / ** markers for the actual IR parameter names.
        std::vector<std::string> cleaned;
        for (auto& a : node->args) {
            if (!a.empty() && a[0]=='*') cleaned.push_back(a.substr(1));
            else cleaned.push_back(a);
        }
        ir.addFunction(lamName, cleaned);
        funcParamNames[lamName] = node->args;
        for (auto& fnr : ir.functions) if (fnr.name == lamName) { fnr.paramNames = node->args; break; }
        ir.setFunctionGlobals(lamName, ir.moduleGlobals);
        knownIRFunctions.insert(lamName);
        lastLambdaSynthetic = lamName;

        std::string savedFunc = currentFunc;

        // Handle default arguments for the lambda (mirror FunctionDef).
        // Defaults are evaluated in the definition context (saved), and stored
        // into module globals so the call-site default injection can find them.
        std::vector<std::string> defaults;
        size_t defaultIndex = 0;
        for (const auto& c : node->children) {
            if (c && c->type == "Default") {
                std::string defVal = lowerExpr(c.get());
                std::string slot = "__default_" + lamName + "_" + std::to_string(defaultIndex++);
                ir.addModuleGlobal(slot);
                ir.addInstruction(savedFunc, "assign", {defVal}, slot);
                defaults.push_back(slot);
            }
        }
        if (!defaults.empty()) {
            funcDefaultCount[lamName] = defaults.size();
            funcDefaultValues[lamName] = defaults;
        }
        for (auto& fnr : ir.functions) if (fnr.name == lamName) { fnr.defaultGlobals = defaults; break; }
        // Also record the temp names (in the definition scope) for later propagation to lists.
        if (!defaults.empty()) lambdaDefaultTemps[lamName] = defaults;

        // Capture the outer temp counter *after* any default exprs (which intentionally
        // allocate in the definition context), but *before* we stomp it for the lambda body.
        int savedTemp = tempCounter;

        currentFunc = lamName;
        tempCounter = 0;

        // Body is the first (and only) non-Default child. Lower it as the
        // implicit return expression for the lambda.
        bool emittedRet = false;
        for (const auto& c : node->children) {
            if (c && c->type == "Default") continue;
            if (c) {
                std::string bodyVal = lowerExpr(c.get());
                // B4: lambdas must return a PyObject* (ABI + callers expect it).
                // If the body produced a native unboxed numeric (A2/A3), box it.
                std::string rt = typeOf(bodyVal);
                if (rt == "i64") {
                    std::string bx = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(lamName, "box_i64", {bodyVal}, bx, "int");
                    bodyVal = bx;
                } else if (rt == "float") {
                    std::string bx = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(lamName, "box_f64", {bodyVal}, bx, "float");
                    bodyVal = bx;
                }
                ir.addInstruction(lamName, "ret", {bodyVal});
                emittedRet = true;
                break;  // lambda has exactly one body expression
            }
        }
        if (!emittedRet) {
            std::string z = "c" + std::to_string(tempCounter++);
            ir.addInstruction(lamName, "const", {"0"}, z);
            ir.addInstruction(lamName, "ret", {z});
        }

        currentFunc = savedFunc;
        tempCounter = savedTemp;

            // For direct/alias paths we return the synthetic name (lowerAssign records
            // it into lambdaAliases when the target is a Name). For value use (passing,
            // storing, indirect call) the actual expression value produced by the
            // Lambda node in lowerExpr is the string token const; that token is what
            // gets boxed, returned from functions, put into lists, etc.
            // The synthetic is also registered in knownIRFunctions so adapters and
            // direct lowering know about it.
            return lamName;
        }

    std::string lowerCompare(const ASTNode* node) {
        if (node->children.empty()) return "";
        // Evaluate all operands exactly once: children[0]=left, children[1..n]=comparators
        std::vector<std::string> operands;
        for (const auto& c : node->children)
            operands.push_back(lowerExpr(c.get()));

        const auto& ops = node->args;   // all op names, populated by parser
        if (ops.empty()) return "";

        // Helper: emit one pairwise comparison, return result name
        auto emitPair = [&](const std::string& opName,
                             const std::string& lhs, const std::string& rhs) {
            std::string r = "t" + std::to_string(tempCounter++);
            if (opName == "In") {
                ir.addInstruction(currentFunc, "call", {"Pyc_Contains", rhs, lhs}, r);
            } else if (opName == "NotIn") {
                std::string c2 = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_Contains", rhs, lhs}, c2);
                ir.addInstruction(currentFunc, "call", {"PyObject_Not", c2}, r);
            } else if (opName == "Is") {
                // identity: compare PyObject* pointers directly, not values
                ir.addInstruction(currentFunc, "ptricmp", {"Eq", lhs, rhs}, r);
            } else if (opName == "IsNot") {
                ir.addInstruction(currentFunc, "ptricmp", {"NotEq", lhs, rhs}, r);
            } else {
                ir.addInstruction(currentFunc, "icmp", {opName, lhs, rhs}, r);
            }
            return r;
        };

        // Single comparison — common fast path
        if (ops.size() == 1 && operands.size() >= 2)
            return emitPair(ops[0], operands[0], operands[1]);

        // Chained: (a op0 b) and (b op1 c) and ... — short-circuit like BoolOp
        int bc = tempCounter++;
        std::string resultVar = "chain_r_"   + std::to_string(bc);
        std::string endLabel  = "chain_end_" + std::to_string(bc);

        std::string first = emitPair(ops[0], operands[0], operands[1]);
        ir.addInstruction(currentFunc, "assign", {first}, resultVar);

        for (size_t i = 1; i < ops.size() && i + 1 < operands.size(); ++i) {
            std::string rhsL = "chain_rhs_" + std::to_string(bc) + "_" + std::to_string(i);
            std::string truth = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyObject_TruthBoxed", resultVar}, truth);
            ir.addInstruction(currentFunc, "br", {truth, rhsL, endLabel});
            ir.addInstruction(currentFunc, "label", {}, rhsL);
            std::string pairRes = emitPair(ops[i], operands[i], operands[i + 1]);
            ir.addInstruction(currentFunc, "assign", {pairRes}, resultVar);
        }
        ir.addInstruction(currentFunc, "label", {}, endLabel);
        return resultVar;
    }

    void lowerIf(const ASTNode* node) {
        int c = tempCounter++;
        std::string thenL = "if_then_" + std::to_string(c);
        std::string elseL = "if_else_" + std::to_string(c);
        std::string endL  = "if_end_"  + std::to_string(c);

        std::string cond = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        ir.addInstruction(currentFunc, "br", {cond, thenL, elseL});

        auto beforeTypes = valueTypes;

        ir.addInstruction(currentFunc, "label", {}, thenL);

        // node->value = number of then-body statements (set in parser)
        size_t bodyCount = node->value.empty() ? 0 : (size_t)std::stoi(node->value);
        size_t n = node->children.size();
        for (size_t i = 1; i <= bodyCount && i < n; ++i)
            lower(node->children[i].get());
        auto thenTypes = valueTypes;

        ir.addInstruction(currentFunc, "br", {}, endL);
        ir.addInstruction(currentFunc, "label", {}, elseL);

        valueTypes = beforeTypes;
        for (size_t i = 1 + bodyCount; i < n; ++i)
            lower(node->children[i].get());
        auto elseTypes = valueTypes;

        ir.addInstruction(currentFunc, "label", {}, endL);
        mergeBranchTypes(beforeTypes, thenTypes, elseTypes);
    }

    void lowerWhile(const ASTNode* node) {
        int c = tempCounter++;
        std::string loopL = "while_loop_" + std::to_string(c);
        std::string bodyL = "while_body_" + std::to_string(c);
        std::string exitL = "while_exit_" + std::to_string(c);

        std::string savedCont = loopContinueLabel, savedBreak = loopBreakLabel;
        loopTryDepths.push_back(activeTries.size());
        loopContinueLabel = loopL;
        loopBreakLabel    = exitL;

        ir.addInstruction(currentFunc, "label", {}, loopL);
        auto loopEntryTypes = valueTypes;
        std::string cond = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        ir.addInstruction(currentFunc, "br", {cond, bodyL, exitL});
        ir.addInstruction(currentFunc, "label", {}, bodyL);
        for (size_t i = 1; i < node->children.size(); ++i)
            lower(node->children[i].get());
        widenLoopTypes(loopEntryTypes);
        ir.addInstruction(currentFunc, "br", {}, loopL);
        ir.addInstruction(currentFunc, "label", {}, exitL);

        loopTryDepths.pop_back();
        loopContinueLabel = savedCont;
        loopBreakLabel    = savedBreak;
    }

    void lowerFor(const ASTNode* node) {
        // For AST layout (from buildAST):
        //   node->id        = target variable name  (e.g. "i")
        //   children[0]     = iter expression, or tuple/list target pattern
        //   children[1]     = iter expression when children[0] is a pattern
        if (node->children.empty()) return;
        size_t iterIndex = (node->id == "__unpack__" &&
                            node->children[0] &&
                            (node->children[0]->type == "Tuple" || node->children[0]->type == "List"))
                               ? 1 : 0;
        if (node->children.size() <= iterIndex) return;
        if (node->id != "__unpack__" && isNativeRangeCandidate(node->children[iterIndex].get())) {
            lowerRangeFor(node, iterIndex);
            return;
        }
        std::string listVal = lowerExpr(node->children[iterIndex].get());  // iter
        // Propagate element type from the iter to the loop variable. For
        // instance, a list of Match objects (from re.finditer) makes the
        // loop var of type "match", so the .group() method dispatches
        // correctly.
        std::string iterElemType = "boxed";
        if (typeOf(listVal) == "match_list") iterElemType = "match";
        // Materialise the iterator as a list. For list iterables this is
        // a no-op; for dicts (iterate keys), strings (iterate characters),
        // and other sequences it converts to a list of elements. We can't
        // know the static type at lowering time, so always go through
        // PyBuiltin_List which is cheap for lists and correct otherwise.
        std::string listRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", listVal}, listRes);
        if (iterElemType != "boxed") noteType(listRes, "match_list");
        listVal = listRes;
        // Store iterator in a slot so owned refs (e.g. sorted() result) are freed
        // at scope exit instead of leaking when emitDecRefIfOwnedSameBlock is blocked
        // inside the loop body (different block from the iterator definition).
        std::string iterSlot = "__iter_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "assign", {listVal}, iterSlot);
        listVal = iterSlot;

        // Boxed length: PyList_SizeBoxed returns PyObject*(int)
        std::string lenRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", listVal}, lenRes);
        // Store sentinel in a slot so null-init + DECREF-on-reassign + slot-exit-cleanup
        // handle it automatically (avoids leak when this code is inside an outer loop).
        std::string lenSlot = "__sl_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlot);

        // Use a fresh temp for the initial 0 to avoid name collision with the alloca.
        std::string idxVar  = node->id + "__idx";     // alloca variable name
        std::string idxInit = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, idxInit);
        ir.addInstruction(currentFunc, "assign", {idxInit}, idxVar);

        std::string loopLabel = "for_loop_" + std::to_string(tempCounter);
        std::string bodyLabel = "for_body_" + std::to_string(tempCounter);
        std::string exitLabel = "for_exit_" + std::to_string(tempCounter);
        tempCounter++;

        std::string savedCont = loopContinueLabel, savedBreak = loopBreakLabel;
        loopTryDepths.push_back(activeTries.size());
        loopContinueLabel = loopLabel;
        loopBreakLabel    = exitLabel;

        ir.addInstruction(currentFunc, "label", {}, loopLabel);
        auto loopEntryTypes = valueTypes;
        std::string cmpRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "icmp", {"Lt", idxVar, lenSlot}, cmpRes);
        ir.addInstruction(currentFunc, "br", {cmpRes, bodyLabel, exitLabel});

        ir.addInstruction(currentFunc, "label", {}, bodyLabel);
        std::string itemRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", listVal, idxVar}, itemRes);
        if (node->id == "__unpack__") {
            if (iterIndex == 1) {
                lowerUnpackTarget(node->children[0].get(), itemRes);
            } else {
                for (size_t j = 0; j < node->args.size(); ++j) {
                    std::string ic = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {std::to_string(j)}, ic);
                    std::string elem = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", itemRes, ic}, elem);
                    ir.addInstruction(currentFunc, "assign", {elem}, node->args[j]);
                }
            }
        } else {
            ir.addInstruction(currentFunc, "assign", {itemRes}, node->id);
            if (iterElemType != "boxed") noteType(node->id, iterElemType);
        }

        // B5: if the iteration target is cell-backed in this scope, write through the cell
        // instead of (or after) the plain assign above. For the simple "for v in ..." form,
        // node->id holds the target name. We already did a plain assign; if it is cell-backed
        // here, follow up with a PyCell_Set so the shared cell sees the iteration value.
        if (node->id != "__unpack__") {
            auto cit = funcCells.find(currentFunc);
            bool isCell = false;
            if (cit != funcCells.end()) {
                for (const auto& cv : cit->second) { if (cv == node->id) { isCell = true; break; } }
            }
            if (isCell) {
                std::string cellSlot = node->id + "_cell";
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyCell_Set", cellSlot, node->id}, dummy);
            }
        }

        for (size_t i = iterIndex + 1; i < node->children.size(); ++i)
            lower(node->children[i].get());

        // idxVar = idxVar + 1
        std::string oneRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"1"}, oneRes);
        std::string nextIdx = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "add", {idxVar, oneRes}, nextIdx);
        ir.addInstruction(currentFunc, "assign", {nextIdx}, idxVar);

        widenLoopTypes(loopEntryTypes);
        ir.addInstruction(currentFunc, "br", {}, loopLabel);
        ir.addInstruction(currentFunc, "label", {}, exitLabel);

        loopTryDepths.pop_back();
        loopContinueLabel = savedCont;
        loopBreakLabel    = savedBreak;
    }

    bool isRangeCall(const ASTNode* node) const {
        return node && node->type == "Call" &&
               !node->children.empty() && node->children[0] &&
               node->children[0]->id == "range";
    }

    bool isNativeRangeCandidate(const ASTNode* node) const {
        if (!isRangeCall(node)) return false;
        size_t argc = node->children.size() > 0 ? node->children.size() - 1 : 0;
        if (argc < 3) return true;
        return constantStepSign(node->children[3].get()) != 0;
    }

    int constantStepSign(const ASTNode* node) const {
        if (!node) return 1;
        if (node->type == "Constant") {
            try {
                long v = std::stol(node->value);
                if (v > 0) return 1;
                if (v < 0) return -1;
            } catch (...) {
            }
        }
        if (node->type == "UnaryOp" && node->op == "USub" &&
            !node->children.empty() && node->children[0] &&
            node->children[0]->type == "Constant") {
            try {
                long v = std::stol(node->children[0]->value);
                if (v > 0) return -1;
            } catch (...) {
            }
        }
        return 0;
    }

    bool constantI64Value(const ASTNode* node, long& out) const {
        if (!node) return false;
        if (node->type == "Constant") {
            try {
                out = std::stol(node->value);
                return true;
            } catch (...) {
                return false;
            }
        }
        if (node->type == "UnaryOp" && node->op == "USub" &&
            !node->children.empty() && node->children[0] &&
            node->children[0]->type == "Constant") {
            try {
                out = -std::stol(node->children[0]->value);
                return true;
            } catch (...) {
                return false;
            }
        }
        return false;
    }

    std::string lowerRangeI64Arg(const ASTNode* arg) {
        long constVal = 0;
        if (constantI64Value(arg, constVal)) {
            std::string nativeConst = "i" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "i64const", {std::to_string(constVal)}, nativeConst, "i64");
            noteType(nativeConst, "i64");
            return nativeConst;
        }
        std::string boxed = lowerExpr(arg);
        std::string native = "i" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "i64_from_box", {boxed}, native, "i64");
        noteType(native, "i64");
        return native;
    }

    void lowerRangeFor(const ASTNode* node, size_t iterIndex) {
        const ASTNode* call = node->children[iterIndex].get();
        size_t argc = call->children.size() > 0 ? call->children.size() - 1 : 0;

        std::string startRes;
        std::string stopRes;
        std::string stepRes;
        int stepSign = 1;

        if (argc == 1) {
            startRes = "i" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "i64const", {"0"}, startRes, "i64");
            noteType(startRes, "i64");
            stopRes = lowerRangeI64Arg(call->children[1].get());
            stepRes = "i" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "i64const", {"1"}, stepRes, "i64");
            noteType(stepRes, "i64");
        } else if (argc >= 2) {
            startRes = lowerRangeI64Arg(call->children[1].get());
            stopRes = lowerRangeI64Arg(call->children[2].get());
            if (argc >= 3) {
                stepSign = constantStepSign(call->children[3].get());
                stepRes = lowerRangeI64Arg(call->children[3].get());
            } else {
                stepRes = "i" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "i64const", {"1"}, stepRes, "i64");
                noteType(stepRes, "i64");
            }
        } else {
            startRes = "i" + std::to_string(tempCounter++);
            stopRes = "i" + std::to_string(tempCounter++);
            stepRes = "i" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "i64const", {"0"}, startRes, "i64");
            ir.addInstruction(currentFunc, "i64const", {"0"}, stopRes, "i64");
            ir.addInstruction(currentFunc, "i64const", {"1"}, stepRes, "i64");
            noteType(startRes, "i64");
            noteType(stopRes, "i64");
            noteType(stepRes, "i64");
        }

        int c = tempCounter++;
        std::string idxVar = node->id + "__range_idx_" + std::to_string(c);
        std::string loopLabel = "range_loop_" + std::to_string(c);
        std::string bodyLabel = "range_body_" + std::to_string(c);
        std::string incrLabel = "range_incr_" + std::to_string(c);
        std::string exitLabel = "range_exit_" + std::to_string(c);

        ir.addInstruction(currentFunc, "i64assign", {startRes}, idxVar, "i64");
        noteType(idxVar, "i64");

        std::string savedCont = loopContinueLabel, savedBreak = loopBreakLabel;
        loopTryDepths.push_back(activeTries.size());
        loopContinueLabel = incrLabel;
        loopBreakLabel = exitLabel;

        auto loopEntryTypes = valueTypes;
        ir.addInstruction(currentFunc, "label", {}, loopLabel);
        std::string cmpRes = "i" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "i64icmp", {stepSign < 0 ? "Gt" : "Lt", idxVar, stopRes}, cmpRes, "bool");
        ir.addInstruction(currentFunc, "br", {cmpRes, bodyLabel, exitLabel});

        ir.addInstruction(currentFunc, "label", {}, bodyLabel);
        // Unbox the visible loop variable as native i64 inside the range region.
        // Uses of the name in numeric contexts will load the i64 directly.
        // Contexts that need a PyObject* (calls, containers, print, return, etc.)
        // will box on demand at the use site.
        ir.addInstruction(currentFunc, "i64assign", {idxVar}, node->id, "i64");
        noteType(node->id, "i64");
        for (size_t i = iterIndex + 1; i < node->children.size(); ++i)
            lower(node->children[i].get());

        ir.addInstruction(currentFunc, "label", {}, incrLabel);
        std::string nextIdx = "i" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "i64add", {idxVar, stepRes}, nextIdx, "i64");
        noteType(nextIdx, "i64");
        ir.addInstruction(currentFunc, "i64assign", {nextIdx}, idxVar, "i64");
        noteType(idxVar, "i64");
        widenLoopTypes(loopEntryTypes);
        ir.addInstruction(currentFunc, "br", {}, loopLabel);
        ir.addInstruction(currentFunc, "label", {}, exitLabel);

        loopTryDepths.pop_back();
        loopContinueLabel = savedCont;
        loopBreakLabel = savedBreak;
    }

    std::vector<std::string> lowerElements(const ASTNode* node) {
        std::vector<std::string> elems;
        if (!node) return elems;
        for (const auto& c : node->children) elems.push_back(lowerExpr(c.get()));
        return elems;
    }

    std::string lowerList(const ASTNode* node) {
        auto elems = lowerElements(node);
        size_t n = elems.size();
            bool allInt = n > 0;
            bool allFloat = n > 0;
            std::vector<std::string> elemTypeList;
            for (const auto& e : elems) {
                std::string t = typeOf(e);
                if (t != "int" && t != "i64" && t != "bool") allInt = false;
                if (t != "float") allFloat = false;
                elemTypeList.push_back(t);
            }
        std::string listRes = "t" + std::to_string(tempCounter++);
        if (allInt) {
            std::string sizeConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
            ir.addInstruction(currentFunc, "call", {"PyList_NewIntBoxed", sizeConst}, listRes);
            noteType(listRes, "list_int");
            knownIntLists.insert(listRes);
        } else if (allFloat) {
            std::string sizeConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
            ir.addInstruction(currentFunc, "call", {"PyList_NewFloatBoxed", sizeConst}, listRes);
            noteType(listRes, "list_float");
            knownFloatLists.insert(listRes);
        } else {
            std::string sizeConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sizeConst}, listRes);
            noteType(listRes, "list");
            for (auto& fn : ir.functions) {
                if (fn.name == currentFunc) {
                    std::unordered_map<size_t, std::string> idxMap;
                    for (size_t i = 0; i < n; ++i) {
                        idxMap[i] = elemTypeList[i];
                    }
                    fn.subscriptElementTypes[listRes] = idxMap;
                    fn.listElementTypes[listRes] = elemTypeList;
                    break;
                }
            }
        }

        bool containsTok = false;
        for (size_t i = 0; i < n; ++i) {
            std::string idxConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idxConst);
            if (allInt && i < n) {
                std::string elemType = typeOf(elems[i]);
                bool elemIsInt = (elemType == "int" || elemType == "i64" || elemType == "bool");
                if (elemIsInt) {
                    ir.addInstruction(currentFunc, "call", {"PyList_SetItemInt64", listRes, idxConst, elems[i]}, "");
                } else {
                    ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", listRes, idxConst, elems[i]}, "");
                }
            } else if (allFloat && i < n) {
                std::string elemType = typeOf(elems[i]);
                bool elemIsFloat = (elemType == "float");
                if (elemIsFloat) {
                    ir.addInstruction(currentFunc, "call", {"PyList_SetItemDouble", listRes, idxConst, elems[i]}, "");
                } else {
                    ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", listRes, idxConst, elems[i]}, "");
                }
            } else {
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", listRes, idxConst, elems[i]}, "");
            }
            if (!elems[i].empty() && (callableTokenTemps.count(elems[i]) || callableTokenToSynthetic.count(elems[i]))) {
                containsTok = true;
            }
        }
        if (containsTok) {
            listsContainingCallableTokens.insert(listRes);
        }
        bool containsBundle = false;
        for (size_t i = 0; i < n; ++i) {
            if (!elems[i].empty() && (bundleTemps.count(elems[i]) || bundleToSynthetic.count(elems[i]))) {
                containsBundle = true; break;
            }
        }
        if (containsBundle) listsContainingBundles.insert(listRes);
        return listRes;
    }

     std::string lowerDict(const ASTNode* node) {
         std::string dictRes = "t" + std::to_string(tempCounter++);
         ir.addInstruction(currentFunc, "call", {"PyDict_New"}, dictRes);
         noteType(dictRes, "dict");

         // S4: Track dict value types for constant-key dict literals.
         // If all values have the same type, record it for .values() propagation.
         std::string commonValType = "";
         bool allStringKeys = true;
         for (size_t i = 0; i + 1 < node->children.size(); i += 2) {
             // Check key is a string constant
             const ASTNode* keyNode = node->children[i].get();
             if (!keyNode || keyNode->type != "Constant" || keyNode->is_str) {
                 allStringKeys = false;
             }
         }

         if (allStringKeys) {
             // Gather value types
             for (size_t i = 1; i + 1 < node->children.size(); i += 2) {
                 std::string val = lowerExpr(node->children[i].get());
                 std::string valType = typeOf(val);
                 if (commonValType.empty()) {
                     commonValType = valType;
                 } else if (commonValType != valType) {
                     commonValType = "boxed";
                     break;
                 }
             }
             if (commonValType == "boxed") commonValType = "";
             if (!commonValType.empty()) {
                 // Store value type for this dict temp
                 tempContainerElementTypes[dictRes] = commonValType;
             }
             // Emit the dict items (re-lower values since we already lowered above)
             for (size_t i = 0; i + 1 < node->children.size(); i += 2) {
                 std::string key = lowerExpr(node->children[i].get());
                 std::string val = lowerExpr(node->children[i+1].get());
                 ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", dictRes, key, val}, "");
             }
         } else {
             for (size_t i = 0; i + 1 < node->children.size(); i += 2) {
                 std::string key = lowerExpr(node->children[i].get());
                 std::string val = lowerExpr(node->children[i+1].get());
                 ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", dictRes, key, val}, "");
             }
         }
         return dictRes;
     }

    std::string lowerAttribute(const ASTNode* node) {
        std::string obj = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string res = "t" + std::to_string(tempCounter++);
        std::string attrNameConst = "c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"\"" + node->id + "\""}, attrNameConst, "str");
        ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, attrNameConst}, res);
        // Annotate the result as "dict" (we can't distinguish dict vs other
        // container, but for method dispatch on sys.module-style lookups
        // this is a useful hint — see lowerMethodCall's dict-method path).
        noteType(res, "dict");
        return res;
    }

    void lowerAssign(const ASTNode* node) {
        // Multi-target: a = b = val — args holds all target names
        if (!node->args.empty()) {
            std::string val = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
            std::string vt = typeOf(val);
            for (const auto& name : node->args) {
                bool isGlob = isGlobalHere(name);
                if (!isGlob && (numericLocals.count(name) || (vt == "int" || vt == "i64" || vt == "bool"))) {
                    ir.addInstruction(currentFunc, "i64assign", {val}, name, "i64");
                    numericLocals.insert(name);
                    noteType(name, "i64");
                } else {
                    ir.addInstruction(currentFunc, "assign", {val}, name);
                    noteType(name, vt);
                }
                if (vt != "int" && vt != "i64" && vt != "bool") {
                    killNumericLocal(name);
                }
                // B4: if the assigned value is (or carries) a callable token, mark the target name.
                if (!val.empty() && (callableTokenTemps.count(val) || callableTokenToSynthetic.count(val))) {
                    namesThatMayHoldCallableTokens.insert(name);
                }
            }
            return;
        }
        // Track list/tuple literals for *args static expansion within the function.
            if (node->id != "__subscript__" &&
            !node->children.empty() && node->children[0] &&
            (node->children[0]->type == "List" || node->children[0]->type == "Tuple")) {
            listLiteralElemASTs[node->id] = {};
            for (auto& ch : node->children[0]->children) listLiteralElemASTs[node->id].push_back(ch.get());
            // B4: we conservatively mark the list name here too; lowerList will do the
            // precise marking of listsContainingCallableTokens when it sees token elements.
        }
        if (node->id == "__attr_assign__") {
            // Attribute assignment: self.x = value — store in instance dict
            if (node->children.size() < 2) return;
            const ASTNode* attrTarget = node->children[0].get();  // Attribute node
            std::string obj = lowerExpr(attrTarget->children.size() > 0 ? attrTarget->children[0].get() : nullptr);
            std::string attrName = attrTarget->id;  // attribute name (e.g., "x")
            std::string val = lowerExpr(node->children[1].get());
            std::string attrConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + attrName + "\""}, attrConst, "str");
            std::string dummy = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", obj, attrConst, val}, dummy);
            return;
        }
        if (node->id == "__subscript__") {
            if (node->children.size() < 2) return;
            const ASTNode* sub = node->children[0].get();   // Subscript node
            std::string obj = lowerExpr(sub->children.size() > 0 ? sub->children[0].get() : nullptr);
            const ASTNode* idxnode = (sub->children.size() > 1 ? sub->children[1].get() : nullptr);
            std::string val = lowerExpr(node->children[1].get());
            if (idxnode && idxnode->type == "Slice") {
                std::string start = lowerExpr(idxnode->children.size() > 0 ? idxnode->children[0].get() : nullptr);
                std::string stop  = lowerExpr(idxnode->children.size() > 1 ? idxnode->children[1].get() : nullptr);
                std::string step  = (idxnode->children.size() > 2 && idxnode->children[2])
                                       ? lowerExpr(idxnode->children[2].get()) : "";
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_SetSlice", obj, start, stop, step, val}, dummy);
                return;
            }
            std::string idx = lowerExpr(idxnode);
            // A4/A7: use native set for proven homogeneous lists with native values.
            // A7: extend float/int provenance to generic lists via Auto helpers.
            std::string objType = typeOf(obj);
            std::string valType = typeOf(val);
            std::string idxType = typeOf(idx);
            bool isIntList = (objType == "list_int");
            bool isFloatList = (objType == "list_float");
            bool valIsInt = (valType == "int" || valType == "i64" || valType == "bool");
            bool valIsFloat = (valType == "float");
            std::string dummy;
            if (isIntList && valIsInt) {
                dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemInt64", obj, idx, val}, dummy);
            } else if (isFloatList && valIsFloat) {
                dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemDouble", obj, idx, val}, dummy);
            } else if (valIsFloat) {
                dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemDoubleAuto", obj, idx, val}, dummy);
            } else if (valIsInt) {
                dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemInt64Auto", obj, idx, val}, dummy);
            } else {
                dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", obj, idx, val}, dummy);
            }
            return;
        }
        if (node->id == "__unpack__") {
            if (node->children.size() < 2) return;
            const ASTNode* tupleTgt = node->children[0].get();  // Tuple/List of Name nodes
            std::string rhs = lowerExpr(node->children[1].get());
            lowerUnpackTarget(tupleTgt, rhs);
            return;
        }
        if (!node->children.empty() && node->children[0]) {
            std::string val = lowerExpr(node->children[0].get());
            // B5: if the target is cell-backed *in this function* (we own or receive the cell here),
            // emit PyCell_Set instead of a plain assign. A name that is only a nonlocal target in a
            // nested scope should not be routed through a cell at this level.
            if (isCellBackedHere(node->id)) {
                std::string cellSlot = node->id + "_cell";
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyCell_Set", cellSlot, val}, dummy);
                noteType(node->id, typeOf(val));
                killNumericLocal(node->id);  // A2.1: unboxed numeric locals are not cell-backed yet
                // B4 token propagation (cells are still names for B4 purposes).
                if (!val.empty() && (callableTokenTemps.count(val) || callableTokenToSynthetic.count(val))) {
                    namesThatMayHoldCallableTokens.insert(node->id);
                }
                return;
            }
            // B5 (closure propagation): if RHS is a descriptor bundle, mark the target as carrying
            // a bundle so later bare-name callees and calls can extract cells from it.
            if (!val.empty() && bundleTemps.count(val)) {
                bundleTemps.insert(node->id);
                auto bit = bundleToSynthetic.find(val);
                if (bit != bundleToSynthetic.end()) bundleToSynthetic[node->id] = bit->second;
                auto dit = descriptorCells.find(val);
                if (dit != descriptorCells.end()) descriptorCells[node->id] = dit->second;
                namesThatMayHoldBundles.insert(node->id);
            }
            // B5 (function-returned bundle propagation via assign): if RHS is a call result
            // we previously marked as returning a bundle, mark the target name accordingly.
            if (!val.empty() && bundleTemps.count(val)) {
                // already handled above
            }
            std::string vt = typeOf(val);
            bool isGlob = isGlobalHere(node->id);
            if (!isGlob && (numericLocals.count(node->id) || (vt == "int" || vt == "i64" || vt == "bool"))) {
                // A2.1: use native i64assign for proven int/bool/i64 local
                ir.addInstruction(currentFunc, "i64assign", {val}, node->id, "i64");
                numericLocals.insert(node->id);
                noteType(node->id, "i64");
            } else {
                ir.addInstruction(currentFunc, "assign", {val}, node->id);
                noteType(node->id, vt);
                // S4: For module-level dicts, propagate value types to the global name
                if (isGlob && vt == "dict" && tempContainerElementTypes.count(val)) {
                    dictValueTypes[node->id] = tempContainerElementTypes[val];
                }
                // For globals, remove from numericLocals to prevent i64 alloca creation
                if (isGlob) numericLocals.erase(node->id);
            }
            if (vt != "int" && vt != "i64" && vt != "bool") {
                killNumericLocal(node->id);
            }
            // If the RHS value is a synthetic lambda name (or we just lowered a lambda
            // expression and captured its synthetic), remember the alias so future
            // calls through 'node->id' can resolve to the nested IR function.
            if (!val.empty() && val.rfind("__lambda_", 0) == 0) {
                lambdaAliases[node->id] = val;
            } else if (!lastLambdaSynthetic.empty()) {
                lambdaAliases[node->id] = lastLambdaSynthetic;
                lastLambdaSynthetic.clear();
            }
            // B4 token propagation for bare names:
            // - If the value is a tracked callable token temp (or the token const itself),
            //   mark the target name so bare-name callees will load its runtime value as the token.
            if (!val.empty() && (callableTokenTemps.count(val) || callableTokenToSynthetic.count(val))) {
                namesThatMayHoldCallableTokens.insert(node->id);
            }
            // - If the RHS is a call to a function known to return a callable, mark the target.
            //   (We also mark the result temp below in lowerCall when we detect such a call.)
            //   Here we conservatively also check if the value temp came from such a call.
            //   (The call-site marking below is the primary path; this is a belt-and-suspenders.)
        }
    }

    void lowerUnpackTarget(const ASTNode* target, const std::string& value) {
        if (!target) return;
        if (target->type == "Name") {
            if (!target->id.empty()) {
                auto cit = funcCells.find(currentFunc);
                bool isCell = false;
                if (cit != funcCells.end()) {
                    for (const auto& cv : cit->second) { if (cv == target->id) { isCell = true; break; } }
                }
                if (isCell) {
                    std::string cellSlot = target->id + "_cell";
                    std::string dummy = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyCell_Set", cellSlot, value}, dummy);
                    noteType(target->id, typeOf(value));
                    killNumericLocal(target->id);
                    // B4 token propagation (rare but keep behavior consistent).
                    if (!value.empty() && (callableTokenTemps.count(value) || callableTokenToSynthetic.count(value) ||
                                           listsContainingCallableTokens.count(value))) {
                        namesThatMayHoldCallableTokens.insert(target->id);
                    }
                    return;
                }
                ir.addInstruction(currentFunc, "assign", {value}, target->id);
            }
            noteType(target->id, typeOf(value));
            killNumericLocal(target->id);  // A2.1: unpack sources are boxed for now
            // B4: if the unpacked value is a tracked callable token (or the container we
            // are unpacking from is known to contain tokens), mark the target name so that
            // later bare-name calls through it are routed via Pyc_Apply.
            if (!value.empty() && (callableTokenTemps.count(value) || callableTokenToSynthetic.count(value) ||
                                   listsContainingCallableTokens.count(value))) {
                namesThatMayHoldCallableTokens.insert(target->id);
            }
            return;
        }
        if (target->type != "Tuple" && target->type != "List") return;
        for (size_t i = 0; i < target->children.size(); ++i) {
            std::string ic = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(i)}, ic);
            std::string elem = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", value, ic}, elem);
            // Propagate token nature into the element temps for nested unpack targets.
            if (!value.empty() && (callableTokenTemps.count(value) || listsContainingCallableTokens.count(value))) {
                callableTokenTemps.insert(elem);
            }
            // S1: Propagate container element types to unpacked elements.
            // If value has containerElementTypes at index i, annotate elem accordingly.
            // Also propagate listElementTypes and subscriptElementTypes for per-index tracking.
            for (auto& fn : ir.functions) {
                if (fn.name != currentFunc) continue;
                
                // Check containerElementTypes (type-level: "float_list" → subscript elem is "float")
                auto cit = fn.containerElementTypes.find(value);
                if (cit != fn.containerElementTypes.end()) {
                    auto iit = cit->second.find(i);
                    if (iit != cit->second.end()) {
                        std::string elemCType = iit->second;
                        if (elemCType == "float_list") {
                            for (size_t idx = 0; idx <= 20; idx++) {
                                fn.subscriptElementTypes[elem][idx] = "float";
                            }
                            fn.containerElementTypes[elem][0] = "float_list";
                        } else if (elemCType == "int_list") {
                            for (size_t idx = 0; idx <= 20; idx++) {
                                fn.subscriptElementTypes[elem][idx] = "int";
                            }
                            fn.containerElementTypes[elem][0] = "int_list";
                        } else if (elemCType == "float" || elemCType == "int") {
                            noteType(elem, elemCType);
                        }
                    }
                }
                
                // Also propagate listElementTypes as a fallback when containerElementTypes has no entry
                // This handles mixed-type containers where each index has a concrete element type
                auto lit = fn.listElementTypes.find(value);
                if (lit != fn.listElementTypes.end() && !lit->second.empty() && i < lit->second.size()) {
                    std::string elemType = lit->second[i];
                    if (elemType == "float" || elemType == "float_list" || elemType == "list_float") {
                        for (size_t idx = 0; idx <= 20; idx++) {
                            fn.subscriptElementTypes[elem][idx] = "float";
                        }
                        if (elemType == "list_float" || elemType == "float_list") {
                            fn.containerElementTypes[elem][0] = "float_list";
                        } else {
                            noteType(elem, "float");
                        }
                    } else if (elemType == "int" || elemType == "int_list" || elemType == "list_int") {
                        for (size_t idx = 0; idx <= 20; idx++) {
                            fn.subscriptElementTypes[elem][idx] = "int";
                        }
                        if (elemType == "list_int" || elemType == "int_list") {
                            fn.containerElementTypes[elem][0] = "int_list";
                        } else {
                            noteType(elem, "int");
                        }
                    }
                }
                
                // Propagate subscriptElementTypes from the source to the element
                // If source has typed elements, the element at index i should inherit the type
                auto sit = fn.subscriptElementTypes.find(value);
                if (sit != fn.subscriptElementTypes.end()) {
                    // Check if this is a container with typed element subscripts (like float_list)
                    for (const auto& [idx, et] : sit->second) {
                        if (et == "float" || et == "float_list" || et == "list_float") {
                            // If value is itself a float_list-like container, all indices are float
                            for (size_t eidx = 0; eidx <= 20; eidx++) {
                                fn.subscriptElementTypes[elem][eidx] = "float";
                            }
                            fn.containerElementTypes[elem][0] = "float_list";
                            break;
                        } else if (et == "int" || et == "int_list" || et == "list_int") {
                            for (size_t eidx = 0; eidx <= 20; eidx++) {
                                fn.subscriptElementTypes[elem][eidx] = "int";
                            }
                            fn.containerElementTypes[elem][0] = "int_list";
                            break;
                        }
                    }
                }
            }
            lowerUnpackTarget(target->children[i].get(), elem);
        }
    }

    // Lower a single `del` target. Supports:
    //   del name        — free the local/global alloca (DECREF the value, mark slot as unowned)
    //   del d[k]        — call PyDict_DelItem(dict, key)
    //   del obj.attr    — best-effort: del obj's instance/class attr via the same machinery
    //                     used for getattr. If the attribute is missing this is a no-op,
    //                     which differs from CPython's AttributeError but keeps the compiler
    //                     simple.
    void lowerDelTarget(const ASTNode* target) {
        if (!target) return;
        if (target->type == "Name") {
            // DECREF the value held in the name's alloca (if any) so the storage is
            // conceptually cleared. Then store a real null constant into the slot so
            // subsequent reads see None instead of a freed pointer.
            const std::string& name = target->id;
            std::string dummy = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Py_DECREF", name}, dummy);
            // Emit a real nconst and assign it to the slot.
            std::string noneRes = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "nconst", {}, noneRes, "none");
            ir.addInstruction(currentFunc, "assign", {noneRes}, name);
            noteType(name, "none");
            killNumericLocal(name);
        } else if (target->type == "Subscript") {
            // del d[k]
            if (target->children.size() < 2) return;
            std::string obj = lowerExpr(target->children[0].get());
            std::string idx = lowerExpr(target->children[1].get());
            std::string dummy = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyDict_DelItem", obj, idx}, dummy);
        } else if (target->type == "Attribute") {
            // del obj.attr — best-effort: store the empty string into the instance/class attr
            // so it's effectively gone. CPython would remove the key from the dict entirely,
            // but we don't track that here. We use a sentinel marker attribute __deleted__.
            // A future fix can add a real delete; for now this is a no-op via the existing
            // setattr path (overwriting the value with None preserves the key).
            // To do better without a runtime helper, simply Pyc_SetItem with a sentinel name;
            // since we don't have a deletion path for attr dicts, fall back to a no-op.
            (void)target;  // intentionally no-op
        }
    }

    void lowerAugAssign(const ASTNode* node) {
        if (node->children.empty()) return;
        std::string op = node->op;
        if      (op == "Add")      op = "add";
        else if (op == "Sub")      op = "sub";
        else if (op == "Mult")     op = "mul";
        else if (op == "FloorDiv") op = "div";
        else if (op == "Div")      op = "truediv";
        else if (op == "Mod")      op = "mod";
        else if (op == "Pow")      op = "pow";
        else                       op = "add";

        if (node->id == "__subscript__") {
            // a[i] op= val — children[0]=Subscript, children[1]=rhs
            if (node->children.size() < 2) return;
            const ASTNode* sub = node->children[0].get();
            std::string obj = lowerExpr(sub->children.size() > 0 ? sub->children[0].get() : nullptr);
            std::string idx = lowerExpr(sub->children.size() > 1 ? sub->children[1].get() : nullptr);
            std::string rhs = lowerExpr(node->children[1].get());
            std::string cur = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_Subscript", obj, idx}, cur);
            noteType(cur, typeOf(rhs));
            std::string res = "t" + std::to_string(tempCounter++);
            std::string resultType = numericResultType(op, cur, rhs);
            ir.addInstruction(currentFunc, op, {cur, rhs}, res, resultType);
            noteType(res, resultType);
            std::string dummy = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", obj, idx, res}, dummy);
        } else {
            // Normal name: children[0] = rhs
            // B5: obtain the current LHS value via the cell (PyCell_Get) if the target is
            // cell-backed here. We cannot just pass the bare name into the arithmetic op,
            // because codegen for ops resolves bare names via getOrLoad (plain local/global),
            // which would bypass the cell for a nonlocal. We must explicitly load through
            // the cell so that augassign (x += k etc.) sees and updates the shared cell.
            std::string lhsVal;
            if (isCellBackedHere(node->id)) {
                std::string cellSlot = node->id + "_cell";
                lhsVal = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyCell_Get", cellSlot}, lhsVal);
            } else {
                lhsVal = node->id;
            }
            std::string rhs = lowerExpr(node->children[0].get());
            std::string result = "t" + std::to_string(tempCounter++);
            std::string resultType = numericResultType(op, lhsVal, rhs);
            ir.addInstruction(currentFunc, op, {lhsVal, rhs}, result, resultType);
            noteType(result, resultType);
            if (isCellBackedHere(node->id)) {
                std::string cellSlot = node->id + "_cell";
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyCell_Set", cellSlot, result}, dummy);
                return;
            }
            if (!isGlobalHere(node->id) && (numericLocals.count(node->id) || resultType == "int" || resultType == "i64" || resultType == "bool")) {
                ir.addInstruction(currentFunc, "i64assign", {result}, node->id, "i64");
                numericLocals.insert(node->id);
                noteType(node->id, "i64");
            } else {
                ir.addInstruction(currentFunc, "assign", {result}, node->id);
                noteType(node->id, resultType);
            }
        }
    }

    std::string lowerSubscriptGet(const ASTNode* node) {
        // Subscript node: children[0]=object, children[1]=slice/index
        std::string obj = lowerExpr(node->children.size() > 0 ? node->children[0].get() : nullptr);
        if (node->children.size() > 1 && node->children[1] &&
            node->children[1]->type == "Slice") {
            const ASTNode* slice = node->children[1].get();
            std::string start = lowerExpr(slice->children.size() > 0 ? slice->children[0].get() : nullptr);
            std::string stop = lowerExpr(slice->children.size() > 1 ? slice->children[1].get() : nullptr);
            std::string step = (slice->children.size() > 2 && slice->children[2])
                                   ? lowerExpr(slice->children[2].get()) : "";
            std::string res = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetSlice", obj, start, stop, step}, res);
            return res;
        }
        std::string idx = lowerExpr(node->children.size() > 1 ? node->children[1].get() : nullptr);
        std::string res = "t" + std::to_string(tempCounter++);
        // A4: determine element type for homogeneous lists so codegen can use native path
        std::string elemType = "boxed";
        std::string objType = typeOf(obj);
        if (objType == "list_int") {
            elemType = "int";
            noteType(res, "int");
        }
        else if (objType == "list_float") {
            elemType = "float";
            noteType(res, "float");
        }
        else if (objType == "list") {
            // A7: track known-float/int lists from lowerList and use them
            // for subscript element type inference even when objType is generic.
            if (knownFloatLists.count(obj)) {
                elemType = "float";
                noteType(res, "float");
            }
            else if (knownIntLists.count(obj)) {
                elemType = "int";
                noteType(res, "int");
            }
            // S1: check containerElementTypes / subscriptElementTypes for variable names
            // that have known element types propagated from defaults or globals.
            // Try to match a literal integer index for exact lookup.
            else {
                std::string idxValue = "";
                if (node->children.size() > 1 && node->children[1] && 
                    node->children[1]->type == "Constant" && node->children[1]->args.size() == 1) {
                    idxValue = node->children[1]->args[0];
                }
                size_t idxVal = 0;
                bool hasLiteralIndex = !idxValue.empty();
                if (hasLiteralIndex) {
                    try { idxVal = std::stoull(idxValue); } catch (...) { hasLiteralIndex = false; }
                }
                
                for (auto& fn : ir.functions) {
                    // Check ALL functions' subscriptElementTypes - needed for module-level globals
                    // that are defined in a different function than the caller
                    
                    // First try subscriptElementTypes (direct element type)
                    auto sit = fn.subscriptElementTypes.find(obj);
                    if (sit != fn.subscriptElementTypes.end() && !sit->second.empty()) {
                        if (hasLiteralIndex) {
                            auto iit = sit->second.find(idxVal);
                            if (iit != sit->second.end()) {
                                elemType = iit->second;
                                if (elemType == "float") noteType(res, "float");
                                else if (elemType == "int") noteType(res, "int");
                                else noteType(res, "boxed");
                            }
                        } else {
                            // No literal index - use first entry (wildcard or generic)
                            auto iit = sit->second.begin();
                            elemType = iit->second;
                            if (elemType == "float") noteType(res, "float");
                            else if (elemType == "int") noteType(res, "int");
                            else noteType(res, "boxed");
                        }
                        break; // Found element types
                    }
                    // Then try containerElementTypes for typed container elements
                    (void)elemType; // suppress unused warning if not used below
                }
                
                // S3: check listElementTypes for per-index element types from list construction
                for (auto& fn : ir.functions) {
                    // Check ALL functions' listElementTypes for module-level globals
                    auto lit = fn.listElementTypes.find(obj);
                    if (lit != fn.listElementTypes.end()) {
                        if (!lit->second.empty()) {
                            if (hasLiteralIndex && idxVal < lit->second.size()) {
                                elemType = lit->second[idxVal];
                                if (elemType == "float") noteType(res, "float");
                                else if (elemType == "float_list" || elemType == "list_float") noteType(res, "float");
                                else if (elemType == "int") noteType(res, "int");
                                else if (elemType == "int_list" || elemType == "list_int") noteType(res, "int");
                            }
                        }
                        break;
                    }
                    
                    // Also check if any container variable has this obj as its element type
                    // (for unpacked subscript results like v1 where v1[i] where listElementTypes[obj][i] = "list_float")
                    if (hasLiteralIndex) {
                        for (const auto& [cname, etypes] : fn.listElementTypes) {
                            if (cname == obj && idxVal < etypes.size()) {
                                std::string et = etypes[idxVal];
                                if (et == "float" || et == "float_list" || et == "list_float") {
                                    elemType = "float";
                                    noteType(res, "float");
                                    break;
                                } else if (et == "int" || et == "int_list" || et == "list_int") {
                                    elemType = "int";
                                    noteType(res, "int");
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }
                
                // Check containerElementTypes for typed element containers (float_list, etc.)
                for (auto& fn : ir.functions) {
                    if (fn.name != currentFunc) continue;
                    auto cit = fn.containerElementTypes.find(obj);
                    if (cit != fn.containerElementTypes.end() && !cit->second.empty()) {
                        std::string matchType = "";
                        size_t matchIdx = 0;
                        
                        // Look for exact index match or wildcard (0)
                        for (const auto& [ikey, ctype] : cit->second) {
                            if (ikey == idxVal) { matchType = ctype; matchIdx = ikey; break; }
                        }
                        if (matchType.empty() && !cit->second.empty()) {
                            // Use first entry (likely wildcard)
                            auto wit = cit->second.begin();
                            matchType = wit->second;
                        }
                        
                        // Extract element type from container type
                        if (matchType == "float_list") { elemType = "float"; if (objType == "list") noteType(res, "float"); }
                        else if (matchType == "int_list") { elemType = "int"; if (objType == "list") noteType(res, "int"); }
                        else if (matchType == "boxed_tuple") { elemType = "boxed"; }
                        else if (matchType == "boxed") { elemType = "boxed"; }
                        break;
                    }
                }
            }
            // Also check if obj is a temp variable that was previously marked
            // as list_float / list_int via noteType in lowerList or unpack.
            // (Note: this is a fallback; the S1 checks above cover most cases)
            if (obj.size() >= 2 && obj.substr(0, 2) == "t" && 
                obj.size() > 2 && isdigit(obj[2])) {
                std::string t = typeOf(obj);
                if (t == "list_float") {
                    elemType = "float";
                    noteType(res, "float");
                }
                else if (t == "list_int") {
                    elemType = "int";
                    noteType(res, "int");
                }
            }
        }
        ir.addInstruction(currentFunc, "call", {"Pyc_Subscript", obj, idx}, res, elemType);
        // B4: if the container is a list we built that contained callable tokens, or the
        // container name is marked as holding tokens, mark the subscript result as a token temp.
        if (listsContainingCallableTokens.count(obj) || namesThatMayHoldCallableTokens.count(obj)) {
            callableTokenTemps.insert(res);
        }
        if (listsContainingBundles.count(obj) || namesThatMayHoldBundles.count(obj) || namesThatMayHoldListsWithBundles.count(obj)) {
            bundleTemps.insert(res);
        }
        return res;
    }

    std::string lowerReturnExpr(const ASTNode* node) {
        std::string val = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        // Returning from inside try scopes: pop still-pushed frames and run
        // pending finally bodies (innermost first) before the ret. The return
        // value is evaluated first, matching Python.
        if (!activeTries.empty()) emitTryExits(0);
        ir.addInstruction(currentFunc, "ret", {val}, val);
        // S2 (flow-sensitive types): track return types for function return type inference
        if (!val.empty()) {
            std::string rt = typeOf(val);
            if (currentFnReturnType == "boxed" || currentFnReturnType.empty()) {
                currentFnReturnType = rt;
            } else if (currentFnReturnType != rt) {
                // Multiple different return types -> promote to boxed
                currentFnReturnType = "boxed";
            }
        }
        // B4: if this return carries a tracked callable token (or is the result of a function
        // known to return callables), mark the current function so callers can propagate tokens.
        if (!val.empty() && (callableTokenTemps.count(val) || callableTokenToSynthetic.count(val))) {
            currentFnReturnsCallable = true;
        }
        // B5 (closures): if this return carries a descriptor bundle (capturing lambda/closure),
        // mark the current function so callers can propagate bundles and extract cells.
        if (!val.empty() && bundleTemps.count(val)) {
            currentFnReturnsBundle = true;
            auto sit = bundleToSynthetic.find(val);
            if (sit != bundleToSynthetic.end()) currentReturnedBundleSynthetic = sit->second;
            auto dit = descriptorCells.find(val);
            if (dit != descriptorCells.end()) currentReturnedBundleCaps = dit->second;
        }
        return val;
    }

    void lowerReturn(const ASTNode* node) {
        lowerReturnExpr(node);
    }

    // Active try scopes in the current function, innermost last. Tracks
    // whether the runtime frame is still pushed in the region being lowered
    // and which finally body (if any) must run when control exits the scope
    // early (return / break / continue).
    struct ActiveTry {
        bool framePushed;
        const ASTNode* finallyBody;   // synthetic "finalbody" node, or null
    };
    std::vector<ActiveTry> activeTries;
    // activeTries.size() at each enclosing loop entry — break/continue exit
    // try scopes down to the innermost loop's depth only.
    std::vector<size_t> loopTryDepths;

    // Emit frame pops + pending finally bodies for every try scope above
    // targetDepth (0 = function exit for `return`). Each finally body is
    // lowered with its own scope already removed from activeTries so nested
    // early exits inside the finally only see outer scopes.
    void emitTryExits(size_t targetDepth) {
        auto saved = activeTries;
        while (activeTries.size() > targetDepth) {
            ActiveTry t = activeTries.back();
            activeTries.pop_back();
            if (t.framePushed) ir.addInstruction(currentFunc, "call", {"pyc_try_pop"}, "");
            if (t.finallyBody)
                for (const auto& s : t.finallyBody->children) if (s) lower(s.get());
        }
        activeTries = saved;
    }

    // Lower a region (handler body or else clause) whose raises must run
    // `finallyBody` before propagating. With a finally, the region runs under
    // its own setjmp frame: on exception, run the finally, then re-raise
    // outward. Without one, the statements lower inline. Ends the region with
    // a branch to afterL on the normal path.
    void lowerFinallyProtected(const std::vector<const ASTNode*>& stmts,
                               const ASTNode* finallyBody,
                               const std::string& afterL,
                               const std::string& endL) {
        if (!finallyBody || stmts.empty()) {
            for (const auto* s : stmts) lower(s);
            ir.addInstruction(currentFunc, "br", {}, afterL);
            return;
        }
        int pc = tempCounter++;
        std::string jmp   = "__tryjmp_p" + std::to_string(pc);
        std::string bodyL = "try_pb_" + std::to_string(pc);
        std::string pexcL = "try_px_" + std::to_string(pc);
        ir.addInstruction(currentFunc, "try_begin", {jmp, bodyL, pexcL}, bodyL);
        ir.addInstruction(currentFunc, "label", {}, bodyL);
        activeTries.push_back({true, finallyBody});
        for (const auto* s : stmts) lower(s);
        activeTries.pop_back();
        ir.addInstruction(currentFunc, "call", {"pyc_try_pop"}, "");
        ir.addInstruction(currentFunc, "br", {}, afterL);
        ir.addInstruction(currentFunc, "label", {}, pexcL);
        std::string e2 = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"pyc_current_exception"}, e2);
        for (const auto& s : finallyBody->children) if (s) lower(s.get());
        ir.addInstruction(currentFunc, "call", {"pyc_raise", e2}, "");
        ir.addInstruction(currentFunc, "br", {}, endL);   // unreachable formality
    }

    // try/except/else/finally lowering with typed handler dispatch.
    //
    // Runtime protocol (see Runtime.cpp): try_begin pushes a setjmp frame on
    // first entry only; pyc_raise POPS the innermost frame before longjmp'ing
    // to it and leaves the exception in g_current_exception. So on the
    // exception path the frame is already gone: handler dispatch, re-raise
    // (to the next outer try or fatal), and handler-body raises all work
    // without further stack bookkeeping. The normal path pops explicitly
    // after the body.
    //
    // Early exits: activeTries records each region's pushed-frame state and
    // pending finally; lowerReturnExpr and Break/Continue emit the required
    // pops + finally bodies (emitTryExits). Handler bodies and the else
    // clause of a try that has a finally run under their own frame
    // (lowerFinallyProtected) so raises there still run the finally.
    //
    // Layout:
    //   try_begin(jmp, tryL, excL)
    //   tryL:   body; pyc_try_pop; [else]; br finallyL/endL
    //   excL:   exc = pyc_current_exception; typed dispatch chain
    //   H_i:    [bind as-name]; pyc_clear_exception; handler body; br finallyL/endL
    //   nomatchL: [finally]; pyc_raise(exc)  (propagate outward)
    //   finallyL: finally body; br endL
    //   endL:
    void lowerTry(const ASTNode* node) {
        if (node->children.empty()) return;
        // Split children into body / handlers / optional else / optional finally.
        std::vector<const ASTNode*> bodyStmts;
        std::vector<const ASTNode*> handlers;
        const ASTNode* elseBody = nullptr;
        const ASTNode* finallyBody = nullptr;
        for (const auto& c : node->children) {
            if (!c) continue;
            if (c->type == "finalbody") {
                finallyBody = c.get();
            } else if (c->type == "elsebody") {
                elseBody = c.get();
            } else if (c->type == "ExceptHandler") {
                handlers.push_back(c.get());
            } else {
                bodyStmts.push_back(c.get());
            }
        }
        int c = tempCounter++;
        std::string jmpVar   = "__tryjmp_" + std::to_string(c);
        std::string tryL     = "try_body_" + std::to_string(c);
        std::string excL     = "try_exc_" + std::to_string(c);
        std::string nomatchL = "try_nomatch_" + std::to_string(c);
        std::string endL     = "try_end_"  + std::to_string(c);
        std::string finallyL = "try_finally_" + std::to_string(c);
        std::string afterBodyL = finallyBody ? finallyL : endL;
        std::vector<std::string> handlerLabels;
        for (size_t i = 0; i < handlers.size(); ++i)
            handlerLabels.push_back("try_h_" + std::to_string(c) + "_" + std::to_string(i));

        if (handlers.empty() && finallyBody == nullptr) {
            // Degenerate try (no except, no finally): body + else inline.
            for (const auto* s : bodyStmts) lower(s);
            if (elseBody) for (const auto& s : elseBody->children) if (s) lower(s.get());
            return;
        }

        ir.addInstruction(currentFunc, "try_begin", {jmpVar, tryL, excL}, tryL);

        // Normal path: body, pop the frame, then else (outside the frame --
        // exceptions in else/finally are not caught by this try).
        ir.addInstruction(currentFunc, "label", {}, tryL);
        activeTries.push_back({true, finallyBody});
        for (const auto* s : bodyStmts) lower(s);
        activeTries.pop_back();
        ir.addInstruction(currentFunc, "call", {"pyc_try_pop"}, "");
        {
            std::vector<const ASTNode*> elseStmts;
            if (elseBody) for (const auto& s : elseBody->children) if (s) elseStmts.push_back(s.get());
            lowerFinallyProtected(elseStmts, finallyBody, afterBodyL, endL);
        }

        // Exception path: fetch the exception, then run the dispatch chain.
        ir.addInstruction(currentFunc, "label", {}, excL);
        std::string excVar = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"pyc_current_exception"}, excVar);
        for (size_t i = 0; i < handlers.size(); ++i) {
            const ASTNode* h = handlers[i];
            std::string nextClauseL = (i + 1 < handlers.size())
                ? ("try_chk_" + std::to_string(c) + "_" + std::to_string(i + 1))
                : nomatchL;
            if (h->args.empty()) {
                // bare `except:` catches everything
                ir.addInstruction(currentFunc, "br", {}, handlerLabels[i]);
            } else {
                // One check per listed type; any match enters the handler.
                for (size_t k = 0; k < h->args.size(); ++k) {
                    std::string nameConst = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + h->args[k] + "\""}, nameConst, "str");
                    std::string m = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"pyc_exc_matches", excVar, nameConst}, m);
                    std::string noMatchNextL = (k + 1 < h->args.size())
                        ? ("try_chk_" + std::to_string(c) + "_" + std::to_string(i) + "_" + std::to_string(k + 1))
                        : nextClauseL;
                    ir.addInstruction(currentFunc, "br", {m, handlerLabels[i], noMatchNextL});
                    if (k + 1 < h->args.size())
                        ir.addInstruction(currentFunc, "label", {}, noMatchNextL);
                }
            }
            if (i + 1 < handlers.size())
                ir.addInstruction(currentFunc, "label", {}, nextClauseL);
        }

        // No handler matched (or a typed chain fell through): run finally,
        // then propagate to the next outer try (or fatal if none).
        ir.addInstruction(currentFunc, "label", {}, nomatchL);
        if (finallyBody) for (const auto& s : finallyBody->children) if (s) lower(s.get());
        ir.addInstruction(currentFunc, "call", {"pyc_raise", excVar}, "");
        ir.addInstruction(currentFunc, "br", {}, endL);   // unreachable formality

        // Handler bodies. With a finally present, each runs finally-protected
        // (a raise in the handler must still run this try's finally).
        for (size_t i = 0; i < handlers.size(); ++i) {
            const ASTNode* h = handlers[i];
            ir.addInstruction(currentFunc, "label", {}, handlerLabels[i]);
            if (!h->id.empty()) {
                ir.addInstruction(currentFunc, "assign", {excVar}, h->id);
            }
            ir.addInstruction(currentFunc, "call", {"pyc_clear_exception"}, "");
            std::vector<const ASTNode*> hStmts;
            for (const auto& s : h->children) if (s) hStmts.push_back(s.get());
            lowerFinallyProtected(hStmts, finallyBody, afterBodyL, endL);
        }

        if (finallyBody) {
            ir.addInstruction(currentFunc, "label", {}, finallyL);
            for (const auto& s : finallyBody->children) if (s) lower(s.get());
            ir.addInstruction(currentFunc, "br", {}, endL);
        }
        ir.addInstruction(currentFunc, "label", {}, endL);
    }


    // obj.method(args) dispatch
    std::string lowerMethodCall(const ASTNode* node) {
        // node->children[0] = Attribute(obj, method_name)
        // node->children[1..] = positional args
        const ASTNode* attr = node->children[0].get();
        std::string methodName = attr->id;

        // B6: Handle super().method() — detect super() before lowering the object
        bool isSuperCall = false;
        if (!attr->children.empty() && attr->children[0] &&
            attr->children[0]->type == "Call" && !attr->children[0]->children.empty() &&
            attr->children[0]->children[0]->type == "Name" &&
            attr->children[0]->children[0]->id == "super") {
            isSuperCall = true;
        }

        std::string obj;
        if (isSuperCall && !currentClass.empty()) {
            // Create a super proxy
            obj = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Super"}, obj);
            superProxyTemps.insert(obj);
        } else {
            obj = lowerExpr(attr->children.empty() ? nullptr : attr->children[0].get());
        }

        std::vector<std::string> args;
        for (size_t i = 1; i < node->children.size(); ++i) {
            if (node->children[i] && node->children[i]->type != "Keyword")
                args.push_back(lowerExpr(node->children[i].get()));
        }

        // B6: Handle super().method() — look up method on parent class
        if (isSuperCall && superProxyTemps.count(obj) && !currentClass.empty()) {
            // Python's super() uses the MRO of the runtime instance's class.
            // We delegate to a runtime helper that:
            // 1. Gets self.__class__
            // 2. Looks up __mro__ from that class dict
            // 3. Finds currentClass in the MRO
            // 4. Calls the method on the next class in the MRO
            std::string res = "t" + std::to_string(tempCounter++);
            std::string methodNameConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + methodName + "\""}, methodNameConst, "str");
            std::string definingClassConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + currentClass + "\""}, definingClassConst, "str");
            
            // Build args list: self, definingClass, methodName, [remaining args]
            std::string argList = "t" + std::to_string(tempCounter++);
            std::string argCount = std::to_string(args.size() + 3); // self + definingClass + methodName + args
            std::string argCountConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {argCount}, argCountConst);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", argCountConst}, argList);
            
            // Add self at index 0
            std::string idxConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxConst);
            std::string setRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, "self"}, setRes);
            
            // Add definingClass at index 1
            idxConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, idxConst);
            setRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, definingClassConst}, setRes);
            
            // Add methodName at index 2
            idxConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"2"}, idxConst);
            setRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, methodNameConst}, setRes);
            
            // Add remaining args at indices 3+
            for (size_t i = 0; i < args.size(); ++i) {
                idxConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i + 3)}, idxConst);
                setRes = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, args[i]}, setRes);
            }
            
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_SuperMethod", argList}, res);
            return res;
        }

        std::string res = "t" + std::to_string(tempCounter++);

        // re module dispatch: detect `re.<name>(...)` by looking at the
        // AST node (Name "re" as the Attribute's base) rather than the
        // inferred type (the module value is a dict, typeOf would say
        // "boxed" / "dict"). This way the runtime helpers
        // PyBuiltin_ReFinditer / ReFindall / ReCompile are called directly
        // with the pattern and subject PyObject* values.
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" && attr->children[0]->id == "re") {
            if (methodName == "finditer" || methodName == "findall" || methodName == "compile" ||
                methodName == "search" || methodName == "match" || methodName == "sub" ||
                methodName == "split") {
                std::string pat = args.size() > 0 ? args[0] : "";
                std::string subj = args.size() > 1 ? args[1] : "";
                if (methodName == "sub") {
                    std::string rep = args.size() > 1 ? args[1] : "";
                    std::string sub = args.size() > 2 ? args[2] : "";
                    std::string cnt;
                    // Look for the `count=...` keyword in the call.
                    for (size_t i = 1; i < node->children.size(); ++i) {
                        const auto* ch = node->children[i].get();
                        if (ch && ch->type == "Keyword" && ch->id == "count" &&
                            !ch->children.empty()) {
                            cnt = lowerExpr(ch->children[0].get());
                        }
                    }
                    ir.addInstruction(currentFunc, "call", {"PyBuiltin_ReSub", pat, rep, sub, cnt}, res);
                    return res;
                } else if (methodName == "split") {
                    ir.addInstruction(currentFunc, "call", {"PyBuiltin_ReSplit", pat, subj, args.size() > 2 ? args[2] : ""}, res);
                    return res;
                }
                std::string fn;
                if (methodName == "finditer")      fn = "PyBuiltin_ReFinditer";
                else if (methodName == "findall")   fn = "PyBuiltin_ReFindall";
                else if (methodName == "compile")   fn = "PyBuiltin_ReCompile";
                else if (methodName == "search")    fn = "PyBuiltin_ReSearch";
                else if (methodName == "match")     fn = "PyBuiltin_ReSearch";  // match → search for now
                if (methodName == "compile") {
                    ir.addInstruction(currentFunc, "call", {fn, pat}, res);
                    noteType(res, "regex");
                } else {
                    // Take the first two positional args and ignore extra
                    // args like re.IGNORECASE (case-insensitive flag).
                    ir.addInstruction(currentFunc, "call", {fn, pat, subj}, res);
                    if (methodName == "finditer" || methodName == "findall") {
                        noteType(res, "match_list");
                    } else if (methodName == "search" || methodName == "match") {
                        noteType(res, "match");
                    }
                }
                return res;
            }
            // Other re.* methods fall through to default lookup.
        }

        // cmath module dispatch: detect `cmath.<name>(...)` by looking at the
        // AST node (Name "cmath" as the Attribute's base).
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" && attr->children[0]->id == "cmath") {
            if (methodName == "sqrt" || methodName == "log" || methodName == "exp" ||
                methodName == "sin" || methodName == "cos" || methodName == "tan") {
                std::string z = args.empty() ? "" : args[0];
                std::string fn;
                if (methodName == "sqrt") fn = "PyCmath_Sqrt";
                else if (methodName == "log") fn = "PyCmath_Log";
                else if (methodName == "exp") fn = "PyCmath_Exp";
                else if (methodName == "sin") fn = "PyCmath_Sin";
                else if (methodName == "cos") fn = "PyCmath_Cos";
                else if (methodName == "tan") fn = "PyCmath_Tan";
                ir.addInstruction(currentFunc, "call", {fn, z}, res);
                noteType(res, "boxed");
                return res;
            }
            // Other cmath.* methods fall through to default lookup.
        }

        // Match.group(i) — dispatch when the inferred type of obj is a
        // Match (which we'll tag with "match" when we construct it).
        if (methodName == "group" && typeOf(obj) == "match") {
            std::string i = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_ReMatchGroup", obj, i}, res);
            return res;
        }

        // List methods (count must come before the string `count` case so
        // that `a.count(x)` for a list dispatches to PyList_Count).
        if (methodName == "count") {
            std::string arg = args.empty() ? "" : args[0];
            std::string fn = (typeOf(obj) == "str") ? "PyString_Count" : "PyList_Count";
            ir.addInstruction(currentFunc, "call", {fn, obj, arg}, res, "int");
            noteType(res, "int");
        // Known list methods
        } else if (methodName == "append") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyList_Append", obj, arg}, res);
        } else if (methodName == "insert") {
            std::string idx = args.size() > 0 ? args[0] : "";
            std::string item = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyList_Insert", obj, idx, item}, res);
        } else if (methodName == "remove") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyList_Remove", obj, arg}, res);
        } else if (methodName == "index") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyList_Index", obj, arg}, res, "int");
            noteType(res, "int");
        } else if (methodName == "reverse") {
            ir.addInstruction(currentFunc, "call", {"PyList_Reverse", obj}, res);
        } else if (methodName == "extend") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyList_Extend", obj, arg}, res);
        } else if (methodName == "copy" && (typeOf(obj) == "list" || typeOf(obj) == "list_int" || typeOf(obj) == "list_float")) {
            ir.addInstruction(currentFunc, "call", {"PyList_Copy", obj}, res);
        } else if (methodName == "clear" && (typeOf(obj) == "list" || typeOf(obj) == "list_int" || typeOf(obj) == "list_float")) {
            ir.addInstruction(currentFunc, "call", {"PyList_Clear", obj}, res);
        // Known string methods
        } else if (methodName == "upper") {
            ir.addInstruction(currentFunc, "call", {"PyString_Upper", obj}, res);
        } else if (methodName == "lower") {
            ir.addInstruction(currentFunc, "call", {"PyString_Lower", obj}, res);
        } else if (methodName == "strip") {
            ir.addInstruction(currentFunc, "call", {"PyString_Strip", obj}, res);
        } else if (methodName == "lstrip") {
            ir.addInstruction(currentFunc, "call", {"PyString_LStrip", obj}, res);
        } else if (methodName == "rstrip") {
            ir.addInstruction(currentFunc, "call", {"PyString_RStrip", obj}, res);
        } else if (methodName == "startswith") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyString_StartsWith", obj, arg}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "endswith") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyString_EndsWith", obj, arg}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "casefold") {
            ir.addInstruction(currentFunc, "call", {"PyString_Casefold", obj}, res);
        } else if (methodName == "title") {
            ir.addInstruction(currentFunc, "call", {"PyString_Title", obj}, res);
        } else if (methodName == "zfill") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyString_ZFill", obj, arg}, res);
        } else if (methodName == "center") {
            std::string w = args.size() > 0 ? args[0] : "";
            std::string fill = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyString_Center", obj, w, fill}, res);
        } else if (methodName == "ljust") {
            std::string w = args.size() > 0 ? args[0] : "";
            std::string fill = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyString_LJust", obj, w, fill}, res);
        } else if (methodName == "rjust") {
            std::string w = args.size() > 0 ? args[0] : "";
            std::string fill = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyString_RJust", obj, w, fill}, res);
        } else if (methodName == "isalpha") {
            ir.addInstruction(currentFunc, "call", {"PyString_IsAlpha", obj}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "isdigit") {
            ir.addInstruction(currentFunc, "call", {"PyString_IsDigit", obj}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "isalnum") {
            ir.addInstruction(currentFunc, "call", {"PyString_IsAlnum", obj}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "islower") {
            ir.addInstruction(currentFunc, "call", {"PyString_IsLower", obj}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "isupper") {
            ir.addInstruction(currentFunc, "call", {"PyString_IsUpper", obj}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "isspace") {
            ir.addInstruction(currentFunc, "call", {"PyString_IsSpace", obj}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "split") {
            if (args.empty()) {
                ir.addInstruction(currentFunc, "call", {"PyString_SplitWhitespace", obj}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyString_Split", obj, args[0]}, res);
            }
        } else if (methodName == "join") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyString_Join", obj, arg}, res);
        } else if (methodName == "find") {
            if (args.size() >= 2) {
                ir.addInstruction(currentFunc, "call", {"PyString_Find3", obj, args[0], args[1]}, res, "int");
            } else {
                std::string arg = args.empty() ? "" : args[0];
                ir.addInstruction(currentFunc, "call", {"PyString_Find", obj, arg}, res, "int");
            }
            noteType(res, "int");
        } else if (methodName == "rfind") {
            if (args.size() >= 3) {
                ir.addInstruction(currentFunc, "call", {"PyString_RFind4", obj, args[0], args[1], args[2]}, res, "int");
            } else if (args.size() == 2) {
                ir.addInstruction(currentFunc, "call", {"PyString_RFind3", obj, args[0], args[1]}, res, "int");
            } else {
                std::string arg = args.empty() ? "" : args[0];
                ir.addInstruction(currentFunc, "call", {"PyString_RFind", obj, arg}, res, "int");
            }
            noteType(res, "int");
        } else if (methodName == "count") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyString_Count", obj, arg}, res, "int");
            noteType(res, "int");
        } else if (methodName == "replace") {
            std::string a = args.size() > 0 ? args[0] : "";
            std::string b = args.size() > 1 ? args[1] : "";
            if (args.size() >= 3) {
                ir.addInstruction(currentFunc, "call", {"PyString_ReplaceN", obj, a, b, args[2]}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyString_Replace", obj, a, b}, res);
            }
            noteType(res, "str");
        // Dict methods
        } else if (methodName == "get") {
            // d.get(k) → PyDict_GetItem(d, k)
            // d.get(k, default) → PyDict_GetItemWithDefault(d, k, default)
            // If no default is given, pass null; the runtime returns null in that case.
            std::string keyArg = args.empty() ? "" : args[0];
            if (args.size() >= 2) {
                ir.addInstruction(currentFunc, "call", {"PyDict_GetItemWithDefault", obj, keyArg, args[1]}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyDict_GetItem", obj, keyArg}, res);
            }
            noteType(res, "boxed");
        } else if (methodName == "keys") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Keys", obj}, res);
            noteType(res, "list");
            // S4: Propagate dict key type if known (for nbody: always "str")
            // dict keys in nbody module are always string literals
            (void)typeOf(obj); // obj is the dict
        } else if (methodName == "values") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Values", obj}, res);
            noteType(res, "list");
            // S4: Propagate dict value type to result for list() to inherit.
            // When obj is a known dict name (or maps to one), lookup its value type.
            std::string valueType = dictValueTypes[obj];
            if (!valueType.empty()) {
                // Mark this temp as having known element type for downstream list() inference
                tempContainerElementTypes[res] = valueType;
                // Also note it as list_of_<valueType> for typeOf() lookup
                // We use a convention: "list_of_X" where X is the value type
                std::string listElemType = "list_of_" + valueType;
                // Store this so later list() can propagate it
                noteType(res, "list_values_typed"); // hint name
            }
            noteType(res, valueType == "boxed" ? "list" : "list");
        } else if (methodName == "items") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Items", obj}, res);
            noteType(res, "list");
        } else if (methodName == "update") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyDict_Update", obj, arg}, res);
        } else if (methodName == "setdefault") {
            std::string key = args.size() > 0 ? args[0] : "";
            std::string defv = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyDict_SetDefault", obj, key, defv}, res);
        } else if (methodName == "copy") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Copy", obj}, res);
        } else if (methodName == "clear") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Clear", obj}, res);
        } else if (methodName == "pop" && typeOf(obj) == "dict") {
            std::string key = args.size() > 0 ? args[0] : "";
            std::string defv = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyDict_Pop", obj, key, defv}, res);
        } else if (methodName == "popitem") {
            ir.addInstruction(currentFunc, "call", {"PyDict_PopItem", obj}, res);
        } else if (methodName == "fromkeys") {
            std::string keys = args.size() > 0 ? args[0] : "";
            std::string defv = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyDict_FromKeys", keys, defv}, res);
        // os.path stub methods
        } else if (methodName == "exists") {
            std::string pathArg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"Pyc_OsPathExists", pathArg}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "isfile") {
            std::string pathArg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"Pyc_OsPathIsFile", pathArg}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "isdir") {
            std::string pathArg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"Pyc_OsPathIsDir", pathArg}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "unlink") {
            std::string pathArg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"Pyc_OsUnlink", pathArg}, res, "int");
            noteType(res, "int");
        // subprocess stub methods
        } else if (methodName == "call") {
            // subprocess.call(cmd) -> exit status (<< 8)
            if (!args.empty()) {
                ir.addInstruction(currentFunc, "call", {"Pyc_SubprocessCall", args[0]}, res, "int");
                noteType(res, "int");
            }
        } else if (methodName == "check_output") {
            // subprocess.check_output(cmd) -> stdout as string
            if (!args.empty()) {
                ir.addInstruction(currentFunc, "call", {"Pyc_SubprocessCheckOutput", args[0]}, res, "str");
                noteType(res, "str");
            }
        // List methods
        } else if (methodName == "sort") {
            ir.addInstruction(currentFunc, "call", {"PyList_Sort", obj}, res);
        } else if (methodName == "pop" && (typeOf(obj) == "list" || typeOf(obj) == "list_int" || typeOf(obj) == "list_float")) {
            if (args.empty()) {
                ir.addInstruction(currentFunc, "call", {"PyList_Pop", obj}, res);
            } else {
                std::string idx = args[0];
                ir.addInstruction(currentFunc, "call", {"PyList_PopAt", obj, idx}, res);
            }
        // String method fallbacks (some keys like "find",
        // "replace", "split" are common to both list and string; the
        // list-specific cases above win for lists).
        } else {
            // Chained module attribute call: `mod.path.func(args)`. The
            // dict-path branch handles the simple case (obj is a dict,
            // e.g. sys.stderr = {"write": "pyc_stderr_write"}). The
            // str-path branch handles the case where chained attribute
            // access already resolved to a string token (e.g.
            // os.path.exists resolves to "PyBuiltin_OsPathExists" via
            // two Pyc_GetItem calls; here the obj's typeOf is "dict"
            // from lowerAttribute, but at runtime the value is a str).
            if (typeOf(obj) == "dict" || typeOf(obj) == "str") {
                std::string methodNameConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + methodName + "\""}, methodNameConst, "str");
                std::string methodLookup = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, methodNameConst}, methodLookup);
                std::vector<std::string> methodArgs;
                for (auto& a : args) {
                    methodArgs.push_back(a);
                }
                std::string argList = "t" + std::to_string(tempCounter++);
                std::string argCount = std::to_string(methodArgs.size());
                std::string argCountConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {argCount}, argCountConst);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", argCountConst}, argList);
                for (size_t i = 0; i < methodArgs.size(); ++i) {
                    std::string idxConst = "c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idxConst);
                    std::string setRes = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, methodArgs[i]}, setRes);
                }
                ir.addInstruction(currentFunc, "call", {"Pyc_Apply", methodLookup, argList}, res);
                return res;
            }
            // Try to call as user-defined method
            // For class instances: look up method on class dict
            std::string methodLookup = "t" + std::to_string(tempCounter++);
            std::string methodNameConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + methodName + "\""}, methodNameConst, "str");
            // Get __class__ from instance, then look up method on class dict
            std::string classKeyConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"__class__\""}, classKeyConst, "str");
            std::string classRef = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, classKeyConst}, classRef);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", classRef, methodNameConst}, methodLookup);
            // Build args list with self prepended
            std::vector<std::string> methodArgs;
            methodArgs.push_back(obj);
            for (auto& a : args) {
                methodArgs.push_back(a);
            }
            // Build flat arg list for Pyc_Apply
            std::string argList = "t" + std::to_string(tempCounter++);
            std::string argCount = std::to_string(methodArgs.size());
            std::string argCountConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {argCount}, argCountConst);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", argCountConst}, argList);
            for (size_t i = 0; i < methodArgs.size(); ++i) {
                std::string idxConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idxConst);
                std::string setRes = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, methodArgs[i]}, setRes);
            }
            // Call method via Pyc_Apply
            ir.addInstruction(currentFunc, "call", {"Pyc_Apply", methodLookup, argList}, res);
        }
        return res;
    }

    // Class definition lowering for minimal data-only classes.
    // A class becomes:
    // 1. A module-level global that is a callable token (string name)
    // 2. When called, creates an instance dict and calls __init__ on it
    // 3. Method calls on instances are attribute lookups on the instance dict
    //    followed by a call with 'self' prepended
    void lowerClass(const ASTNode* node) {
        std::string className = node->id;
        knownClasses.insert(className);
        // Decorators (synthetic "Decorator" children appended by the parser).
        std::vector<const ASTNode*> decorators;
        for (const auto& c : node->children)
            if (c && c->type == "Decorator" && !c->children.empty()) decorators.push_back(c->children[0].get());
        // Method bodies get fresh try-scope state (mirrors FunctionDef).
        std::vector<ActiveTry> savedActiveTries = activeTries; activeTries.clear();
        std::vector<size_t> savedLoopTryDepths = loopTryDepths; loopTryDepths.clear();
        struct TryScopeRestore {
            LoweringVisitor* v;
            std::vector<ActiveTry> at;
            std::vector<size_t> ltd;
            ~TryScopeRestore() { v->activeTries = std::move(at); v->loopTryDepths = std::move(ltd); }
        } tryScopeRestore{this, std::move(savedActiveTries), std::move(savedLoopTryDepths)};
        // Register class as module-level global
        ir.addModuleGlobal(className);
        // Create class dict to hold methods
        std::string classDictTemp = "c" + std::to_string(tempCounter++);
        ir.addInstruction("__module__", "call", {"PyDict_New"}, classDictTemp, "dict");
        // Register class name as known IR function so it can be called directly
        knownIRFunctions.insert(className);
        
        // B6: Track whether this class defines its own __init__
        bool hasOwnInitDefined = false;
        
        // B6: Track all base classes for super() and multiple inheritance support
        // Bases are stored in node->args by the parser
        for (const auto& baseName : node->args) {
            if (baseName.empty() || baseName == "(complex base)") continue;
            classBases[className].push_back(baseName);
        }

        // B6b: Compute MRO for this class using C3 linearization
        computeMRO(className);

        // B6: Copy inherited methods into the class dict following the MRO in
        // reverse, so that classes earlier in the MRO override later ones
        // (Python resolution order). The base class dict IS the class global
        // (classes are represented as dicts).
        {
            const auto& mro = classMRO[className];
            for (auto it = mro.rbegin(); it != mro.rend(); ++it) {
                if (*it == className) continue;
                ir.addInstruction("__module__", "call", {"PyDict_Update", classDictTemp, *it}, "dummy");
            }
        }
        if (getenv("PYC_DEBUG_MRO")) {
            llvm::errs() << "[mro] " << className << " bases=[";
            for (const auto& b : getAllBases(className)) llvm::errs() << b << ",";
            llvm::errs() << "] mro=[";
            for (const auto& m : classMRO[className]) llvm::errs() << m << ",";
            llvm::errs() << "]\n";
        }
        
        // Process all methods
        std::string savedClass = currentClass;
        currentClass = className;
        for (const auto& c : node->children) {
            if (!c || c->type != "FunctionDef") continue;
            std::string methodName = c->id;
            knownIRFunctions.insert(methodName);
            if (methodName == "__init__") {
                hasOwnInitDefined = true;
                // Store __init__ param names from the AST
                std::string initParams;
                for (size_t i = 0; i < c->args.size(); ++i) {
                    if (i > 0) initParams += ",";
                    std::string pname = c->args[i];
                    if (!pname.empty() && pname[0] == '*') pname = pname.substr(1);
                    initParams += pname;
                }
                classInitParams[className] = initParams;
                // Register defaults for this __init__ so the call site (A() /
                // A(x)) can inject trailing defaults when the user omits args.
                // Mirrors the default-handling block in the FunctionDef
                // lowering (see "Count defaults and collect their values").
                {
                    std::vector<std::string> defaults;
                    size_t defaultIndex = 0;
                    for (const auto& cc : c->children) {
                        if (cc && cc->type == "Default") {
                            std::string defVal = lowerExpr(cc.get());
                            std::string slot = "__default___init__" + std::to_string(defaultIndex++);
                            ir.addModuleGlobal(slot);
                            ir.addInstruction("__module__", "assign", {defVal}, slot);
                            defaults.push_back(slot);
                        }
                    }
                    if (!defaults.empty()) {
                        funcDefaultCount["__init__"] = defaults.size();
                        funcDefaultValues["__init__"] = defaults;
                    }
                }
                // Generate __init__ function with correct params
                std::string initFuncName = className + "__init__";
                std::vector<std::string> initFuncParams;
                std::stringstream ss(initParams);
                std::string param;
                while (std::getline(ss, param, ',')) {
                    initFuncParams.push_back(param);
                }
                ir.addFunction(initFuncName, initFuncParams);
                // Lower __init__ body into the init function
                std::string savedFunc = currentFunc;
                currentFunc = initFuncName;
                for (size_t i = 0; i < c->children.size(); ++i) {
                    if (c->children[i] && c->children[i]->type != "Default" && c->children[i]->type != "Decorator") {
                        lower(c->children[i].get());
                    }
                }
            // __init__ must return self (the first argument)
                ir.addInstruction(initFuncName, "ret", {"self"});
                // Store __init__ in class dict as a callable token (string name)
                std::string methodConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + methodName + "\""}, methodConst, "str");
                std::string methodToken = "c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + initFuncName + "\""}, methodToken, "str");
                knownIRFunctions.insert(initFuncName);
                // Store the function name string in the class dict
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, methodConst, methodToken}, dummy);
                currentFunc = savedFunc;
           } else {
                // Lower regular method
                std::string methodFuncName = className + "__" + methodName;
                std::vector<std::string> methodParams;
                for (size_t i = 0; i < c->args.size(); ++i) {
                    std::string pname = c->args[i];
                    if (!pname.empty() && pname[0] == '*') pname = pname.substr(1);
                    methodParams.push_back(pname);
                }
                ir.addFunction(methodFuncName, methodParams);
                knownIRFunctions.insert(methodFuncName);
           // Lower method body
                std::string savedFunc = currentFunc;
                currentFunc = methodFuncName;
                for (size_t i = 0; i < c->children.size(); ++i) {
                    if (c->children[i] && c->children[i]->type != "Default" && c->children[i]->type != "Decorator") {
                        lower(c->children[i].get());
                    }
                }
                // Store method in class dict as a callable token
                std::string methodConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + methodName + "\""}, methodConst, "str");
                std::string methodToken = "c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + methodFuncName + "\""}, methodToken, "str");
                knownIRFunctions.insert(methodFuncName);
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, methodConst, methodToken}, dummy);
                 currentFunc = savedFunc;
             }
         }
         // B6: Process class attributes (non-FunctionDef children)
         for (const auto& c : node->children) {
             if (!c || c->type != "Assign") continue;
             // For simple assignments (Name target), the target id is stored in c->id
             std::string attrName = c->id.empty() ? (c->args.empty() ? "" : c->args[0]) : c->id;
             if (attrName.empty()) continue;
             std::string attrValue = lowerExpr(c->children.empty() ? nullptr : c->children[0].get());
             std::string attrKeyConst = "c" + std::to_string(tempCounter++);
             ir.addInstruction("__module__", "const", {"\"" + attrName + "\""}, attrKeyConst, "str");
             std::string dummy = "t" + std::to_string(tempCounter++);
             ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, attrKeyConst, attrValue}, dummy);
         }
         currentClass = savedClass;
        // B6b: Store MRO in class dict for runtime super() support
        const auto& mro = classMRO[className];
        if (!mro.empty()) {
            std::string mroKeyConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "const", {"\"__mro__\""}, mroKeyConst, "str");
            std::string mroList = "c" + std::to_string(tempCounter++);
            std::string zeroConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "const", {"0"}, zeroConst);
            ir.addInstruction("__module__", "call", {"PyList_NewBoxed", zeroConst}, mroList);
            for (const auto& classNameInMRO : mro) {
                std::string classNameConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + classNameInMRO + "\""}, classNameConst, "str");
                std::string appendRes = "t" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "call", {"PyList_Append", mroList, classNameConst}, appendRes);
            }
            std::string mroDummy = "t" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, mroKeyConst, mroList}, mroDummy);
        }
        // Decorators, bottom-up: className = decoN(...(deco1(classDict))...).
        // Each application: lower the decorator expression, wrap classDictTemp
        // in a one-element list, call Pyc_Apply, and update classDictTemp.
        for (auto it = decorators.rbegin(); it != decorators.rend(); ++it) {
            std::string dv = lowerExpr(*it);
            std::string z = "c" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "const", {"0"}, z);
            std::string argList = "t" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "call", {"PyList_NewBoxed", z}, argList);
            ir.addInstruction("__module__", "call", {"PyList_Append", argList, classDictTemp}, "");
            classDictTemp = "t" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "call", {"Pyc_Apply", dv, argList}, classDictTemp);
        }
        // Store class dict as the class value
        ir.addInstruction("__module__", "assign", {classDictTemp}, className);
        noteType(className, "dict");
        // B6b: register the class in the runtime registry so super() can
        // resolve the class-name strings stored in __mro__ to class dicts.
        {
            std::string regNameConst = "c" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "const", {"\"" + className + "\""}, regNameConst, "str");
            ir.addInstruction("__module__", "call", {"pyc_register_class", regNameConst, classDictTemp}, "");
        }
        
        // B6: If this class doesn't define __init__, create a wrapper that calls base __init__
        if (!hasOwnInitDefined) {
            std::string initName = className + "__init__";
            std::string baseInitName = "";
            for (const auto& base : node->args) {
                if (base.empty() || base == "(complex base)") continue;
                auto basePit = classInitParams.find(base);
                if (basePit != classInitParams.end() && !basePit->second.empty()) {
                    baseInitName = base + "__init__";
                    break;
                }
            }
            if (!baseInitName.empty()) {
                // Determine params from base __init__ (includes "self")
                std::vector<std::string> initParams;
                auto basePit = classInitParams.find(baseInitName.substr(0, baseInitName.find("__init__")));
                if (basePit != classInitParams.end()) {
                    std::string params = basePit->second;
                    std::stringstream ss(params);
                    std::string param;
                    while (std::getline(ss, param, ',')) {
                        initParams.push_back(param);
                    }
                }
                ir.addFunction(initName, initParams);
                knownIRFunctions.insert(initName);
                std::string savedFunc = currentFunc;
                currentFunc = initName;
                std::vector<std::string> callArgs;
                callArgs.push_back(baseInitName);
                // Pass all params (including self)
                for (size_t i = 0; i < initParams.size(); ++i) {
                    callArgs.push_back(initParams[i]);
                }
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(initName, "call", callArgs, dummy);
                ir.addInstruction(initName, "ret", {"self"});
                currentFunc = savedFunc;
                // Store the __init__ wrapper name in the class dict (overrides base __init__)
                std::string initKeyConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"__init__\""}, initKeyConst, "str");
                std::string initValConst = "c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + initName + "\""}, initValConst, "str");
                std::string initSet = "t" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, initKeyConst, initValConst}, initSet);
            }
        }
    }

    // x if cond else y  — IfExp (ternary)
    std::string lowerIfExpr(const ASTNode* node) {
        if (node->children.size() < 3) return "";
        int c = tempCounter++;
        std::string resultVar = "ifexp_r_"    + std::to_string(c);
        std::string thenL     = "ifexp_then_" + std::to_string(c);
        std::string elseL     = "ifexp_else_" + std::to_string(c);
        std::string endL      = "ifexp_end_"  + std::to_string(c);

        // Pre-create the result alloca before the branch so both branches can store to it.
        std::string initVal = "c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, initVal);
        ir.addInstruction(currentFunc, "assign", {initVal}, resultVar);

        std::string cond = lowerExpr(node->children[0].get());
        ir.addInstruction(currentFunc, "br", {cond, thenL, elseL});

        ir.addInstruction(currentFunc, "label", {}, thenL);
        std::string tv = lowerExpr(node->children[1].get());
        ir.addInstruction(currentFunc, "assign", {tv}, resultVar);
        ir.addInstruction(currentFunc, "br", {}, endL);

        ir.addInstruction(currentFunc, "label", {}, elseL);
        std::string ev = lowerExpr(node->children[2].get());
        ir.addInstruction(currentFunc, "assign", {ev}, resultVar);

        ir.addInstruction(currentFunc, "label", {}, endL);
        return resultVar;
    }

    // Short-circuit boolean operator (and / or) with N values.
    // Produces a result alloca variable; codegen loads it for the caller.
    std::string lowerBoolOp(const ASTNode* node) {
        if (node->children.empty()) return "";
        bool isAnd = (node->op == "And");

        // Reserve a single counter for all labels of this boolop instance.
        int bc = tempCounter++;
        std::string resultVar = "boolop_r_"   + std::to_string(bc);
        std::string endLabel  = "boolop_end_" + std::to_string(bc);

        // Evaluate first value; store as initial result.
        std::string firstVal = lowerExpr(node->children[0].get());
        ir.addInstruction(currentFunc, "assign", {firstVal}, resultVar);

        for (size_t i = 1; i < node->children.size(); ++i) {
            std::string rhsLabel = "boolop_rhs_" + std::to_string(bc)
                                   + "_" + std::to_string(i);

            // Box truthiness so the br handler can unbox it.
            std::string truthVal = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call",
                              {"PyObject_TruthBoxed", resultVar}, truthVal);

            // AND: truthy → keep evaluating; OR: truthy → done
            if (isAnd)
                ir.addInstruction(currentFunc, "br", {truthVal, rhsLabel, endLabel});
            else
                ir.addInstruction(currentFunc, "br", {truthVal, endLabel, rhsLabel});

            ir.addInstruction(currentFunc, "label", {}, rhsLabel);
            std::string nextVal = lowerExpr(node->children[i].get());
            ir.addInstruction(currentFunc, "assign", {nextVal}, resultVar);
            // Codegen inserts fallthrough br to endLabel at the next label instruction.
        }

        ir.addInstruction(currentFunc, "label", {}, endLabel);
        return resultVar;
    }

    std::string lowerUnaryOp(const ASTNode* node) {
        std::string val = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        if (node->op == "UAdd") return val;   // identity

        std::string res = "t" + std::to_string(tempCounter++);
        if (node->op == "Not") {
            ir.addInstruction(currentFunc, "call", {"PyObject_Not",  val}, res, "bool");
            noteType(res, "bool");
        } else if (node->op == "USub") {
            std::string resultType = typeOf(val);
            if (resultType != "int" && resultType != "float" && resultType != "bool") {
                resultType = "boxed";
            }
            if (resultType == "int" || resultType == "float") {
                // Emit native neg; codegen will unbox operand if needed and
                // keep the result as native i64/double when possible (A3).
                ir.addInstruction(currentFunc, "neg", {val}, res, resultType);
                noteType(res, resultType);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyNumber_Negate", val}, res, resultType);
                noteType(res, resultType);
            }
        } else {
            ir.addInstruction(currentFunc, "const", {"0"}, res, "int");   // unknown → 0
            noteType(res, "int");
        }
        return res;
    }

    // Lower a single part of a JoinedStr: Constant (string literal) or FormattedValue
    std::string lowerFStrPart(const ASTNode* node) {
        if (node->type == "FormattedValue") return lowerFormattedValue(node);
        return lowerExpr(node);  // Constant string part
    }

    std::string lowerFormattedValue(const ASTNode* node) {
        std::string exprVal = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string res = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyStr_FromAny", exprVal}, res);
        return res;
    }

    std::string lowerJoinedStr(const ASTNode* node) {
        if (node->children.empty()) {
            std::string res = "c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"\""}, res);
            return res;
        }
        std::string acc = lowerFStrPart(node->children[0].get());
        for (size_t i = 1; i < node->children.size(); ++i) {
            std::string part = lowerFStrPart(node->children[i].get());
            std::string newAcc = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyString_Concat", acc, part}, newAcc);
            acc = newAcc;
        }
        return acc;
    }

    std::string lowerListComp(const ASTNode* node) {
        // [elt for x in a (if cond)* for y in b (if cond)* ...]
        // children[0] = elt expression
        // children[1..] = comprehension nodes (one per for-clause). Each
        // comprehension has children[0] = target, children[1] = iter,
        // children[2..] = filter conditions.
        if (node->children.size() < 2) return "";
        const ASTNode* eltNode = node->children[0].get();

        // A4: Detect element type from the AST to create homogeneous lists.
        std::string elemType = detectCompElementType(eltNode);
        // For names and subscripts, try to infer from the iterator type.
        // If the iterator is a known list_int/list_float, assume the element
        // inherits that type (conservative: widens to boxed on store if wrong).
        if (elemType == "boxed" && node->children[1]) {
            const ASTNode* iterNode = node->children[1].get();
            if (iterNode && iterNode->type == "Name") {
                std::string iterT = typeOf(iterNode->id);
                if (iterT == "list_int") elemType = "int";
                else if (iterT == "list_float") elemType = "float";
            }
        }

        // Create the result list with the detected type.
        std::string sc = "c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, sc);
        std::string listVar = "t" + std::to_string(tempCounter++);
        if (elemType == "int") {
            ir.addInstruction(currentFunc, "call", {"PyList_NewIntBoxed", sc}, listVar);
            noteType(listVar, "list_int");
        } else if (elemType == "float") {
            ir.addInstruction(currentFunc, "call", {"PyList_NewFloatBoxed", sc}, listVar);
            noteType(listVar, "list_float");
        } else {
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sc}, listVar);
            noteType(listVar, "list");
        }
        std::string listSlot = "__lc_lst_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "assign", {listVar}, listSlot);
        // Propagate the list type to the slot so downstream assignments see it.
        noteType(listSlot, elemType == "int" ? "list_int" : (elemType == "float" ? "list_float" : "list"));

        // S1: Record per-index element types in subscriptElementTypes for comprehension results.
        // This enables inferListElementTypes to optimize subscripts on comprehension temps.
        if (elemType == "int" || elemType == "float") {
            std::unordered_map<size_t, std::string> compElemTypes;
            std::string etStr = (elemType == "float") ? "float" : "int";
            for (size_t i = 0; i <= 20; i++) compElemTypes[i] = etStr;
            for (auto& fnx : ir.functions) {
                if (fnx.name == currentFunc) {
                    fnx.subscriptElementTypes[listVar] = compElemTypes;
                    fnx.subscriptElementTypes[listSlot] = compElemTypes;
                    break;
                }
            }
        }

        // For each generator, emit a loop that materialises the iterator,
        // iterates with an index, applies the target+condition filters, and
        // either recurses to the next generator or appends the elt to the
        // result list. The nesting is encoded by a sequence of label
        // triplets (loop, body, cont, exit) per generator; the inner
        // generator's loopL is jumped to by the previous generator's body
        // after the conditions pass.
        struct GenCtx {
            std::string loopL, bodyL, contL, exitL;
            std::string target;
            std::vector<const ASTNode*> conds;
        };
        std::vector<GenCtx> gens;
        gens.reserve(node->children.size() - 1);
        for (size_t gi = 1; gi < node->children.size(); ++gi) {
            const ASTNode* genNode = node->children[gi].get();
            if (!genNode || genNode->type != "comprehension" ||
                genNode->children.size() < 2) {
                // Malformed comprehension: bail out by appending nothing.
                continue;
            }
            int c = tempCounter++;
            GenCtx g;
            g.loopL  = "lc_lp_" + std::to_string(c);
            g.bodyL  = "lc_bd_" + std::to_string(c);
            g.contL  = "lc_ct_" + std::to_string(c);
            g.exitL  = "lc_ex_" + std::to_string(c);
            g.target = genNode->children[0]->id;
            for (size_t ci = 2; ci < genNode->children.size(); ++ci) {
                g.conds.push_back(genNode->children[ci].get());
            }
            gens.push_back(g);
        }
        if (gens.empty()) {
            // No usable generator: the comprehension is empty.
            return listSlot;
        }

        // Pre-evaluate each generator's iterator (evaluated once, stored in
        // a slot). This mirrors CPython's behaviour where the iter is
        // computed once at comprehension start.
        std::vector<std::string> iterSlots;
        iterSlots.reserve(gens.size());
        for (size_t gi = 0; gi < gens.size(); ++gi) {
            const ASTNode* genNode = node->children[gi + 1].get();
            std::string iterVal = lowerExpr(genNode->children[1].get());
            std::string iterSlot = "__lci_" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {iterVal}, iterSlot);
            // Also materialise as a list (handles dict/string iterables).
            std::string listified = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", iterSlot}, listified);
            std::string listifiedSlot = "__lcmat_" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {listified}, listifiedSlot);
            iterSlots.push_back(listifiedSlot);
        }

        // Emit each generator's index / loop / body / cont / exit blocks.
        // For nested generators, the last block of generator i (after all
        // conditions pass) branches to generator (i+1)'s loopL, instead of
        // appending the elt to the result. The innermost generator appends.
        std::vector<std::string> idxVars;
        std::vector<std::string> lenSlots;
        idxVars.reserve(gens.size());
        lenSlots.reserve(gens.size());
        for (size_t gi = 0; gi < gens.size(); ++gi) {
            std::string idxVar  = "lc_i_" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            std::string idxInit = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxInit);
            ir.addInstruction(currentFunc, "assign", {idxInit}, idxVar);
            idxVars.push_back(idxVar);

            std::string lenRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", iterSlots[gi]}, lenRes);
            std::string lenSlot = "__lcsl_" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlot);
            lenSlots.push_back(lenSlot);
        }

        // Pre-allocate the post-comp label name so we can reference it
        // from the outermost gen's exitL. The label is emitted after all
        // gens are processed; the codegen's label handler will add the
        // fall-through br from the immediately-preceding (unterminated)
        // block.
        std::string postL = "lc_post_" + std::to_string(tempCounter++);

        // Wire the loops. For each generator, emit a label, the comparison
        // and branch. The body assigns the iteration target, evaluates the
        // conditions, and either branches to the next generator's loopL
        // (after the contL of the current one) or appends the elt.
        // To keep the IR well-formed, the body of generator i ends with
        // either a branch to gens[i+1].loopL (if i+1 exists) or an
        // append followed by a branch to contL.
        for (size_t gi = 0; gi < gens.size(); ++gi) {
            const auto& g = gens[gi];
            ir.addInstruction(currentFunc, "label", {}, g.loopL);
            std::string cmpR = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", idxVars[gi], lenSlots[gi]}, cmpR);
            ir.addInstruction(currentFunc, "br", {cmpR, g.bodyL, g.exitL});

            ir.addInstruction(currentFunc, "label", {}, g.bodyL);
            std::string item = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", iterSlots[gi], idxVars[gi]}, item);
            ir.addInstruction(currentFunc, "assign", {item}, g.target);

            // Conditions: if any is false, jump to contL (skip append / recurse).
            for (const auto* cond : g.conds) {
                std::string trueL = "lc_ci_" + std::to_string(tempCounter++);
                std::string condV = lowerExpr(cond);
                ir.addInstruction(currentFunc, "br", {condV, trueL, g.contL});
                ir.addInstruction(currentFunc, "label", {}, trueL);
            }

            if (gi + 1 < gens.size()) {
                // Recurse into the next generator. The deeper gens' idx
                // slots persist across this gen's iterations, so we must
                // explicitly reset them to 0 here; otherwise the inner
                // loop's `idx < len` check fails on subsequent outer
                // iterations and the inner never re-runs.
                for (size_t gj = gi + 1; gj < gens.size(); ++gj) {
                    std::string zeroR = "t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, zeroR);
                    ir.addInstruction(currentFunc, "assign", {zeroR}, idxVars[gj]);
                }
                ir.addInstruction(currentFunc, "br", {}, gens[gi + 1].loopL);
            } else {
                // Innermost: evaluate element and append.
                std::string eltVal = lowerExpr(eltNode);
                ir.addInstruction(currentFunc, "call", {"PyList_Append", listSlot, eltVal}, "");
            }
            // Fall through to contL.
            ir.addInstruction(currentFunc, "label", {}, g.contL);
            std::string one = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, one);
            std::string nxt = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "add", {idxVars[gi], one}, nxt);
            ir.addInstruction(currentFunc, "assign", {nxt}, idxVars[gi]);
            ir.addInstruction(currentFunc, "br", {}, g.loopL);
            ir.addInstruction(currentFunc, "label", {}, g.exitL);
            if (gi + 1 == gens.size()) {
                // Innermost: when this exits, the outer's idx must
                // advance. Branch to the outer's contL. We emit this
                // here so the codegen does NOT add a fall-through br
                // from this exitL to the next label (which is the
                // post-comp label below).
                if (gi > 0) {
                    ir.addInstruction(currentFunc, "br", {}, gens[gi - 1].contL);
                }
                // For gi == 0 (single-gen comp), the exitL naturally
                // falls through to the post-comp label below.
            } else if (gi == 0) {
                // Outermost (with nested gens): this exitL should fall
                // through to the post-comp code. We explicitly branch
                // to the post-comp label to defeat the codegen's
                // natural fall-through to the next generator's loopL
                // (which would be the wrong target).
                ir.addInstruction(currentFunc, "br", {}, postL);
            } else {
                // Middle gen (3+ gens): exit advances the previous gen's
                // idx, so branch to the previous gen's contL.
                ir.addInstruction(currentFunc, "br", {}, gens[gi - 1].contL);
            }
        }
        // Post-comp label. The print / use-site of the comp result lands
        // here. The codegen's label handler adds the fall-through br
        // from the previous block (which is now the innermost gen's
        // contL, after its iteration advances its idx and re-branches to
        // its loopL — but wait, that's already terminated). The
        // post-comp label is reached from the outermost gen's exitL via
        // the explicit br we emitted above.
        ir.addInstruction(currentFunc, "label", {}, postL);
        return listSlot;
    }

    std::string lowerDictComp(const ASTNode* node) {
        // {key: val for target in iter if conds ...}  (supports multiple generators for product/nested)
        if (node->children.size() < 2) {
            std::string dictRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyDict_New"}, dictRes);
            return dictRes;
        }
        const ASTNode* keyNode = node->children[0].get();
        const ASTNode* valNode = node->children[1].get();

        // Collect generator nodes (comprehension children after key/value)
        std::vector<const ASTNode*> gens;
        for (size_t i = 2; i < node->children.size(); ++i) {
            if (node->children[i] && node->children[i]->type == "comprehension")
                gens.push_back(node->children[i].get());
        }
        if (gens.empty()) {
            std::string dictRes = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyDict_New"}, dictRes);
            return dictRes;
        }

        // Create result dict
        std::string dictRes = "t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyDict_New"}, dictRes);

        // Recursive emitter for nested generators.
        // gi: current generator index; after last, emit the key:val insertion.
        std::function<void(size_t)> emitLevel = [&](size_t gi) {
            if (gi == gens.size()) {
                // innermost: compute key/val and insert
                std::string kVal = lowerExpr(keyNode);
                std::string vVal = lowerExpr(valNode);
                std::string dummy = "t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", dictRes, kVal, vVal}, dummy);
                return;
            }
            const ASTNode* g = gens[gi];
            // iter for this level; stored in a slot so owned refs (e.g. sorted()) are
            // freed at scope exit instead of leaking past the loop body same-block check.
            std::string iterVal = lowerExpr(g->children.size() > 1 ? g->children[1].get() : nullptr);
            std::string dcIterSlot = "__dci_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {iterVal}, dcIterSlot);
            iterVal = dcIterSlot;
            std::string lenRes  = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", iterVal}, lenRes);
            std::string lenSlotDC = "__sl_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlotDC);

            // per-level index
            std::string idxVar  = "dc_i" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            std::string idxInit = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxInit);
            ir.addInstruction(currentFunc, "assign", {idxInit}, idxVar);

            int dc = tempCounter++;
            std::string loopL = "dc_lp" + std::to_string(gi) + "_" + std::to_string(dc);
            std::string bodyL = "dc_bd" + std::to_string(gi) + "_" + std::to_string(dc);
            std::string contL = "dc_ct" + std::to_string(gi) + "_" + std::to_string(dc);
            std::string exitL = "dc_ex" + std::to_string(gi) + "_" + std::to_string(dc);

            ir.addInstruction(currentFunc, "label", {}, loopL);
            std::string cmpR = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", idxVar, lenSlotDC}, cmpR);
            ir.addInstruction(currentFunc, "br", {cmpR, bodyL, exitL});

            ir.addInstruction(currentFunc, "label", {}, bodyL);
            std::string item = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", iterVal, idxVar}, item);
            // target is the first child of the comprehension (Name or unpack pattern)
            if (g->children.size() > 0 && g->children[0]) {
                if (g->children[0]->type == "Name") {
                    ir.addInstruction(currentFunc, "assign", {item}, g->children[0]->id);
                } else {
                    // tuple/list target unpack (reuse the unpack helper)
                    lowerUnpackTarget(g->children[0].get(), item);
                }
            }

            // per-generator if conditions
            std::string afterCondsL = "dc_ac" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            std::string cur = afterCondsL;
            for (size_t ci = 2; ci < g->children.size(); ++ci) {
                if (!g->children[ci]) continue;
                std::string trueL = "dc_ci" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
                std::string condV = lowerExpr(g->children[ci].get());
                ir.addInstruction(currentFunc, "br", {condV, trueL, contL});
                ir.addInstruction(currentFunc, "label", {}, trueL);
            }
            // now emit next level (or body insert)
            emitLevel(gi + 1);

            // increment and continue outer loop
            ir.addInstruction(currentFunc, "label", {}, afterCondsL);  // fallthrough from body if no ifs
            ir.addInstruction(currentFunc, "label", {}, contL);
            std::string one = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, one);
            std::string nxt = "t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "add", {idxVar, one}, nxt);
            ir.addInstruction(currentFunc, "assign", {nxt}, idxVar);
            ir.addInstruction(currentFunc, "br", {}, loopL);
            ir.addInstruction(currentFunc, "label", {}, exitL);
        };

        emitLevel(0);
        return dictRes;
    }
};

// Legacy thin wrapper kept temporarily for any external callers (to be removed)

void lowerAST(const ASTNode* node, ModuleIR& ir,
               const std::unordered_set<std::string>& compiledModules = {},
               const std::unordered_map<std::string, std::vector<std::string>>& importedModuleGlobals = {}) {
    if (!node) return;
    LoweringVisitor visitor(ir, compiledModules, importedModuleGlobals);
    visitor.lower(node);
    // A6: Generate specialized variants after lowering completes.
    visitor.generateSpecializedVariants();
    // S1: Infer container element types for type stability tracking.
    visitor.inferContainerElementTypes();
    // S3: Infer per-index element types for temps created by subscript.
    visitor.inferListElementTypes();
    // A7: Analyze param types from call-site signatures (for native param slots).
    visitor.generateParamTypeAnalysis();
    // A7: Update numericFloatLocals/numericLocals from paramTypes for each function.
    // This propagates known-float params back into the IR so codegen can use native slots.
    for (auto& fnr : ir.functions) {
        for (size_t i = 0; i < fnr.args.size(); ++i) {
            if (i < fnr.paramTypes.size() && fnr.paramTypes[i] == "float") {
                fnr.numericFloatLocals.push_back(fnr.args[i]);
            } else if (i < fnr.paramTypes.size() && fnr.paramTypes[i] == "int") {
                fnr.numericLocals.push_back(fnr.args[i]);
            }
        }
    }
}

bool Compiler::compile(const std::string& inputPath, const std::string& outputPath, bool useStatic, int optLevel, bool emitLLVM, bool emitASM, bool verbose) {
    // B7: First parse the main file to find imports, then scan for those modules
    PythonParser mainParser;
    auto mainAst = mainParser.parseFile(inputPath);
    if (!mainAst) {
        std::cerr << "Parse error for " << inputPath << std::endl;
        return false;
    }
    if (verbose) std::cout << "Parsed AST root: " << mainAst->type << " (depth " << mainAst->children.size() << ")\n";
    
    // Collect import names from the main AST
    std::unordered_set<std::string> importNames;
    std::function<void(const ASTNode*)> collectImports = [&](const ASTNode* node) {
        if (!node) return;
        if (node->type == "Import") {
            std::stringstream ss(node->id);
            std::string tok;
            while (ss >> tok) importNames.insert(tok);
        } else if (node->type == "ImportFrom") {
            if (!node->id.empty()) importNames.insert(node->id);
        }
        for (const auto& c : node->children) collectImports(c.get());
    };
    collectImports(mainAst.get());
    
    if (verbose && !importNames.empty()) {
        std::cout << "B7: Found imports: ";
        for (auto& name : importNames) {
            std::cout << name << " ";
        }
        std::cout << "\n";
    }
    
    // Build list of files to compile: main file + imported modules
    std::string dir = fs::path(inputPath).parent_path().string();
    if (dir.empty()) dir = ".";
    
    std::string mainBasename = fs::path(inputPath).stem().string();
    std::vector<std::string> pyFiles;
    pyFiles.push_back(inputPath); // Main file first
    
    // Add imported modules if they exist as .py files in the same directory
    for (auto& moduleName : importNames) {
        std::string modulePath = dir + "/" + moduleName + ".py";
        if (fs::exists(modulePath) && fs::is_regular_file(modulePath)) {
            pyFiles.push_back(modulePath);
        }
    }
    std::sort(pyFiles.begin(), pyFiles.end());
    
    if (verbose) {
        std::cout << "B7: Compiling " << pyFiles.size() << " module(s)\n";
        for (auto& f : pyFiles) {
            std::cout << "  - " << f << "\n";
        }
    }
    
    // Collect module names for B7 runtime support (exclude main module)
    std::vector<std::string> moduleNames;
    
    // B7: Build set of compiled module names (excluding main) so that import
    // lowering in the main module can decide whether to emit __module__<name>
    // or pyc_import_failed.
    std::unordered_set<std::string> compiledModules;
    for (auto& pyFile : pyFiles) {
        if (pyFile != inputPath) {
            compiledModules.insert(fs::path(pyFile).stem().string());
        }
    }

    // B7: pre-parse each imported module and collect its exported globals so
    // that `from X import *` in the main module can be expanded statically
    // (each exported name becomes a real module global in the main module).
    // The parser is the same one we use for full compilation; this is just
    // a light pass that walks top-level Assign/FunctionDef children and
    // records the bound names.
    std::unordered_map<std::string, std::vector<std::string>> importedModuleGlobals;
    {
        auto collectTopLevelNames = [](const ASTNode* modNode) {
            std::vector<std::string> out;
            if (!modNode || modNode->type != "Module") return out;
            std::function<void(const ASTNode*, bool)> walk = [&](const ASTNode* n, bool top) {
                if (!n) return;
                if (top) {
                    if (n->type == "Assign") {
                        if (!n->args.empty()) {
                            for (const auto& nm : n->args) {
                                if (!nm.empty()) out.push_back(nm);
                            }
                        } else if (!n->id.empty() && n->id != "__subscript__" && n->id != "__unpack__") {
                            out.push_back(n->id);
                        }
                    } else if (n->type == "FunctionDef" && !n->id.empty()) {
                        out.push_back(n->id);
                    } else if (n->type == "ClassDef" && !n->id.empty()) {
                        out.push_back(n->id);
                    }
                }
                for (const auto& c : n->children) walk(c.get(), false);
            };
            for (const auto& c : modNode->children) walk(c.get(), true);
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        };
        for (auto& pyFile : pyFiles) {
            if (pyFile == inputPath) continue;
            PythonParser pp;
            auto ast = pp.parseFile(pyFile);
            if (!ast) continue;
            std::string mn = fs::path(pyFile).stem().string();
            importedModuleGlobals[mn] = collectTopLevelNames(ast.get());
        }
    }

    // Compile each .py file to an LLVM module
    std::vector<std::unique_ptr<llvm::Module>> modules;
    llvm::LLVMContext context;

    for (auto& pyFile : pyFiles) {
        PythonParser parser;
        auto ast = parser.parseFile(pyFile);
        if (!ast) {
            std::cerr << "Warning: Failed to parse " << pyFile << ", skipping\n";
            continue;
        }

        std::string moduleName = fs::path(pyFile).stem().string();
        ModuleIR ir;
        ir.moduleName = moduleName;
        // Pass compiledModules only when lowering the main module so that
        // import lowering can emit pyc_import_failed for missing modules.
        // Pass importedModuleGlobals only to the main module so its
        // `from X import *` can be expanded statically.
        if (pyFile == inputPath) {
            lowerAST(ast.get(), ir, compiledModules, importedModuleGlobals);
        } else {
            lowerAST(ast.get(), ir, std::unordered_set<std::string>{}, std::unordered_map<std::string, std::vector<std::string>>{});
        }
        
        Codegen codegen;
        
        // Only add non-main modules to the B7 module registry
        if (pyFile != inputPath) {
            moduleNames.push_back(moduleName);
        }
        
        auto module = codegen.generate(ir, context, "pyc_" + moduleName);
        if (!module) {
            std::cerr << "Warning: Codegen failed for " << pyFile << ", skipping\n";
            continue;
        }
        
        // B7: Rename the module entry point function to include the module name
        // The entry point is named "__module__" by the lowering visitor.
        // For the main module, rename to "pyc_user_main" so the C runtime can call it.
        // For other modules, rename to "__module__<moduleName>" so they can be called at runtime.
        llvm::Function* entryFunc = module->getFunction("__module__");
        if (entryFunc) {
            std::string newEntryName;
            if (pyFile == inputPath) {
                // Main module: rename to pyc_user_main
                newEntryName = "pyc_user_main";
            } else {
                // Other modules: rename to __module__<moduleName>
                newEntryName = "__module__" + moduleName;
            }
            entryFunc->setName(newEntryName);
        }
        
        if (verbose) {
            std::cout << "  Generated LLVM module for " << moduleName << "\n";
        }
        
        modules.push_back(std::move(module));
    }
    
    if (modules.empty()) {
        std::cerr << "Error: No modules generated\n";
        return false;
    }
    
    // B7: Generate C source file with module registry for runtime module execution
    std::string b7CSource = "#include <string.h>\n";
    b7CSource += "#include <stdio.h>\n";
    b7CSource += "#include <stdlib.h>\n";
    b7CSource += "#include <unistd.h>\n\n";
    
    // Declare extern functions for each module entry point
    for (auto& name : moduleNames) {
        b7CSource += "extern void* __module__" + name + "(void);\n";
    }
    b7CSource += "\n";
    
    // Define the module registry
    b7CSource += "typedef struct {\n";
    b7CSource += "    const char* name;\n";
    b7CSource += "    void* (*entry)(void);\n";
    b7CSource += "} pyc_module_entry;\n\n";
    
    b7CSource += "static pyc_module_entry pyc_modules[] = {\n";
    for (auto& name : moduleNames) {
        b7CSource += "    {\"" + name + "\", __module__" + name + "},\n";
    }
    b7CSource += "    {NULL, NULL}\n";
    b7CSource += "};\n\n";
    
    // Generate the pyc_run_module function - accepts PyObject* (Python string)
    b7CSource += "extern const char* PyStr_AsUTF8(void* obj);\n";
    b7CSource += "void pyc_run_module(void* moduleNameObj) {\n";
    b7CSource += "    const char* moduleName = PyStr_AsUTF8(moduleNameObj);\n";
    b7CSource += "    if (!moduleName) return;\n";
    b7CSource += "    for (int i = 0; pyc_modules[i].name != NULL; i++) {\n";
    b7CSource += "        if (strcmp(pyc_modules[i].name, moduleName) == 0) {\n";
    b7CSource += "            void* result = pyc_modules[i].entry();\n";
    b7CSource += "            return;\n";
    b7CSource += "        }\n";
    b7CSource += "    }\n";
    b7CSource += "    // Module not found - silently skip (unsupported module)\n";
    b7CSource += "}\n";
    
    // Stub implementations for common built-in modules
    b7CSource += "\n";
    b7CSource += "// Stub: sys module - provides argv and stderr\n";
    b7CSource += "void __module__sys(void) {\n";
    b7CSource += "    // sys.argv is set up by pyc_setup_sys in MainWrapper.cpp\n";
    b7CSource += "    // sys.stderr is a file object - we use the real stderr\n";
    b7CSource += "}\n";
    b7CSource += "\n";
    b7CSource += "// Stub: os module - provides path operations\n";
    b7CSource += "void __module__os(void) {\n";
    b7CSource += "    // os.environ - return an empty dict (no env var support)\n";
    b7CSource += "    // os.path.exists, isfile, isdir - use real POSIX functions\n";
    b7CSource += "}\n";
    b7CSource += "\n";
    b7CSource += "// Stub: subprocess module - provides call and check_output\n";
    b7CSource += "void __module__subprocess(void) {\n";
    b7CSource += "    // subprocess.call - use real system()\n";
    b7CSource += "    // subprocess.check_output - use real popen()\n";
    b7CSource += "}\n";
    
    // Write the C source to a temporary file
    std::string b7CFile = outputPath + "_b7_modules.c";
    std::ofstream b7Out(b7CFile);
    if (b7Out.is_open()) {
        b7Out << b7CSource;
        b7Out.close();
    } else {
        std::cerr << "Warning: Could not write B7 module registry to " << b7CFile << "\n";
    }
    
    // Merge all modules into one
    Codegen codegen;
    auto module = Codegen::mergeModules(modules, context, "pyc_module");
    if (!module) return false;
    
    // LTO: Link precompiled runtime bitcode before optimization
    std::string rtBitcodePath = PYC_RUNTIME_BC;
    codegen.linkRuntimeBitcode(module.get(), rtBitcodePath);
    
    codegen.optimize(module.get(), optLevel);
    if (emitLLVM) {
        if (codegen.emitLLVM(module.get(), outputPath + ".ll")) {
            std::cout << "Emitted LLVM IR " << outputPath << ".ll (opt=" << optLevel << ")\n";
            return true;
        }
        return false;
    }
    if (emitASM) {
        if (codegen.emitAssembly(module.get(), outputPath + ".s")) {
            std::cout << "Emitted assembly " << outputPath << ".s (opt=" << optLevel << ")\n";
            return true;
        }
        return false;
    }
    if (codegen.emitObject(module.get(), outputPath + ".o")) {
        std::cout << "Generated object " << outputPath << ".o (opt=" << optLevel << ")\n";
        std::string linkCmd = "clang++ ";
        if (useStatic) linkCmd += "-static -s -Wl,--gc-sections ";

        // Prefer prebuilt static runtime lib if available (from build or install)
        // Otherwise fall back to compiling the small runtime source (always works during development)
        std::string sourceDir = PYC_SOURCE_DIR;
        std::string runtimeLink = " " + sourceDir + "/src/runtime/Runtime.cpp";
        // Try common locations for libpycrt.a
        for (const auto& libdir : {"./build", "../build", ".", "/usr/local/lib", "/usr/lib"}) {
            std::string libpath = std::string(libdir) + "/libpycrt.a";
            if (std::ifstream(libpath).good()) {
                runtimeLink = " -L" + std::string(libdir) + " -lpycrt";
                break;
            }
        }
        // The MainWrapper.cpp provides the C `main` that calls
        // pyc_setup_sys(argc, argv) and then dispatches to the
        // user code's `pyc_user_main`. We always compile it from
        // source here for simplicity (it has no other dependencies
        // beyond the runtime header).
        // B7: Include the generated module registry C source
        // Also add Python include path for B7 module registry
        std::string pythonIncludes = "";
        FILE* pipe = popen("python3-config --includes 2>/dev/null | grep -o '\\-I[^ ]*' | head -1", "r");
        if (pipe) {
            char buf[256];
            if (fgets(buf, sizeof(buf), pipe)) {
                buf[strcspn(buf, "\n")] = 0;
                pythonIncludes = buf;
            }
            pclose(pipe);
        }
        // -x c applies to everything after it on the command line, so reset with
        // -x none or the C++ sources that follow would be compiled as C.
        // Use -flto=thin to enable LinkTimeOptimization.
        // -Wl,--allow-multiple-definition allows LLVM to inline runtime functions
        // into the generated object while still linking libpycrt.a for
        // non-inlined symbols (PCRE2, system calls, etc.)
        linkCmd += outputPath + ".o -flto=thin " +
            "-Wl,--allow-multiple-definition " +
            "-x c " + b7CFile + " -x none -I" + sourceDir + "/include " +
            pythonIncludes + " " + sourceDir + "/src/runtime/MainWrapper.cpp" +
            runtimeLink + " -lpcre2-8 -o " + outputPath + " -O" + std::to_string(optLevel);
        if (std::system(linkCmd.c_str()) == 0) {
            std::cout << "Linked with runtime to " << outputPath << " (static=" << useStatic << ")\n";
        } else {
            std::cerr << "Link failed. Run manually: " << linkCmd << std::endl;
            return false;
        }
        return true;
    }
    return false;
}

} // namespace pyc
