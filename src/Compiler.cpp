#include "pyc/Compiler.h"
#include "pyc/PythonParser.h"
#include "pyc/IR.h"
#include "pyc/Codegen.h"
#include "pyc/LLVMDCE.h"
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
#include <optional>
#include <set>
#include <cstdio>

#ifndef PYC_SOURCE_DIR
#define PYC_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;

namespace pyc {

// Forward declaration: defined near DiscoveredModule/discoverDottedModule
// below, but used by LoweringVisitor::lower's ImportFrom handling above
// that point in the file. See the definition for full documentation.
static std::optional<std::string> resolveRelativeImport(
    const std::string& packageContext, int level, const std::string& module);

// Forward declaration: the exported (non-underscore, top-level) names for
// each synthetic/built-in module pyc implements in the runtime (see
// pyc_import_failed in src/runtime/Runtime.cpp). Used to expand
// `from X import *` for a module that was never "compiled" as a real file
// — mirrors what importedModuleGlobals does for real compiled modules.
// Defined near kSyntheticModules below; kept in sync with each module's
// dict-building code by hand.
static const std::unordered_map<std::string, std::vector<std::string>>& syntheticModuleExports();

class LoweringVisitor {
 public:
     LoweringVisitor(ModuleIR& moduleIR,
                     const std::unordered_set<std::string>& compiledModules = {},
                     const std::unordered_map<std::string, std::vector<std::string>>& importedModuleGlobals = {})
         : ir(moduleIR), compiledModules(compiledModules), importedModuleGlobals(importedModuleGlobals) {}

    // Debug info: source file path for the module being compiled.
    std::string currentSourceFile;
    // The dotted package this module resolves its own relative imports
    // against (its __package__ equivalent) — see packageContextOf. Empty
    // for the main script (which has no package; relative imports there
    // are invalid, matching CPython).
    std::string currentModulePackage;

    void lower(const ASTNode* node, const std::string& funcName = "") {
        if (!node) return;
        if (!funcName.empty()) currentFunc = funcName;
        if (node->lineno > 0) { currentLineno = node->lineno; ir.currentLineno = currentLineno; }

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
                                   "repr","hex","oct","bin","ord","chr","round","cmp_to_key","open","Pyc_ToFlatList","set"}) {
                knownIRFunctions.insert(s);
            }
            for (const auto& c : node->children) {
                lower(c.get());
            }
            
            // B7: Create module dict containing all module globals
            // This dict will be stored in a global variable (e.g., pyc_module_utils)
            // so that importing modules can access it.
            if (!ir.moduleGlobals.empty()) {
                std::string modDict = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyDict_New"}, modDict);
                
                // Add __name__ to the module dict - always "__main__" for top-level module
                std::string nameKey = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"__name__\""}, nameKey, "str");
                std::string moduleName = "__main__";
                std::string nameVal = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + moduleName + "\""}, nameVal, "str");
                ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, nameKey, nameVal}, "set_name");
                
                // Add each global to the module dict
                for (auto& gname : ir.moduleGlobals) {
                    std::string key = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + gname + "\""}, key, "str");
                    // For known IR functions (user-defined functions), register a string token
                    // pointing to the function name (not null, not the global which is uninitialised).
                    std::string val;
                    auto knownIt = knownIRFunctions.find(gname);
                    if (knownIt != knownIRFunctions.end()) {
                        // Store the function name as a string token
                        val = "$t" + std::to_string(tempCounter++);
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
                    std::string envDict = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyDict_New"}, envDict);
                    std::string envKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"environ\""}, envKey, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, envKey, envDict}, "set_env");
                    
                    // os.path = {exists: fn, isfile: fn, isdir: fn, unlink: fn}
                    std::string pathDict = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyDict_New"}, pathDict);
                    std::string pathKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"path\""}, pathKey, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, pathKey, pathDict}, "set_path");
                    
                    // os.path.exists = Pyc_OsPathExists
                    std::string existsKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"exists\""}, existsKey, "str");
                    std::string existsFn = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_OsPathExists\""}, existsFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", pathDict, existsKey, existsFn}, "set_exists");
                    
                    // os.path.isfile = Pyc_OsPathIsFile
                    std::string isfileKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"isfile\""}, isfileKey, "str");
                    std::string isfileFn = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_OsPathIsFile\""}, isfileFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", pathDict, isfileKey, isfileFn}, "set_isfile");
                    
                    // os.path.isdir = Pyc_OsPathIsDir
                    std::string isdirKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"isdir\""}, isdirKey, "str");
                    std::string isdirFn = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_OsPathIsDir\""}, isdirFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", pathDict, isdirKey, isdirFn}, "set_isdir");
                    
                    // os.unlink = Pyc_OsUnlink
                    std::string unlinkKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"unlink\""}, unlinkKey, "str");
                    std::string unlinkFn = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_OsUnlink\""}, unlinkFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, unlinkKey, unlinkFn}, "set_unlink");
                } else if (ir.moduleName == "sys") {
                    // sys.argv = [] (placeholder)
                    std::string argvKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"argv\""}, argvKey, "str");
                    std::string argvSize = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, argvSize, "int");
                    std::string argvList = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", argvSize}, argvList);
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, argvKey, argvList}, "set_argv");
                } else if (ir.moduleName == "subprocess") {
                    // subprocess.call = Pyc_SubprocessCall
                    std::string callKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"call\""}, callKey, "str");
                    std::string callFn = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"Pyc_SubprocessCall\""}, callFn, "str");
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", modDict, callKey, callFn}, "set_call");
                    
                    // subprocess.check_output = Pyc_SubprocessCheckOutput
                    std::string outKey = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"check_output\""}, outKey, "str");
                    std::string outFn = "$c" + std::to_string(tempCounter++);
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
            for (auto& fnr : ir.functions) if (fnr.name == defIRName) {
                fnr.paramNames = node->args;
                fnr.defLineno = node->lineno;
                fnr.sourceFile = currentSourceFile;
                break;
            }
            for (auto& fnr : ir.functions) if (fnr.name == defIRName) { /* freeCellVars set later */ break; }

            // Switch currentFunc to the (possibly unique) IR name so that all cell analysis,
            // instruction emission (addInstruction), and map keys use the name we registered.
            std::string savedForIR = currentFunc;
            currentFunc = defIRName;
            funcQualNameStack.push_back(node->id);

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
                        // P0: keep structured layouts on default slots for param inference
                        copyLayoutMaps(defVal, slot);
                        if (structuredElementLayout.count(defVal))
                            markStructuredList(slot, structuredElementLayout[defVal]);
                        if (pairOfStructuredLayout.count(defVal))
                            markPairOfStructured(slot, pairOfStructuredLayout[defVal]);
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
            funcParamNames[defIRName] = node->args;
            funcDisplayNames[node->id] = node->id;
            funcDisplayNames[defIRName] = node->id;

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
                            if (n->type == "Assign") {
                                if (!n->id.empty()) localsHere.insert(n->id);
                                // Tuple unpacking: "a, b = ..." — the target names are in
                                // the first child (a Tuple of Name nodes).
                                for (const auto& c : n->children) {
                                    if (c && c->type == "Tuple") {
                                        for (const auto& tc : c->children) {
                                            if (tc && tc->type == "Name" && !tc->id.empty())
                                                localsHere.insert(tc->id);
                                        }
                                    }
                                }
                            }
                            for (const auto& c : n->children) collectLocals(c.get());
                        };
                    for (const auto& c : node->children) collectLocals(c.get());
                    // Walk nested function bodies; any name they read that is
                    // also a local/param here must be a cell in this scope.
                    std::function<void(const ASTNode*)> walkNested =
                        [&](const ASTNode* n) {
                            if (!n) return;
                            if (n->type == "FunctionDef" || n->type == "Lambda") {
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
            // Save and clear numericFloatLocals for this function scope (mirrors
            // numericLocals above). Without this, float-typed parameter names
            // inferred for a nested def (e.g. "xmin" in fill_z(z, N, xmin, dx, ...))
            // leak into the enclosing/module scope's numericFloatLocals and cause
            // later module-level globals with the same name to be misclassified
            // as native float locals in codegen (their boxed global store is
            // skipped, leaving the global permanently null).
            std::unordered_set<std::string> savedNumericFloatLocals = numericFloatLocals;
            // Fresh try-scope state for the nested function's body — its
            // returns must not run the enclosing scope's try exits.
            std::vector<ActiveTry> savedActiveTries = activeTries; activeTries.clear();
            std::vector<size_t> savedLoopTryDepths = loopTryDepths; loopTryDepths.clear();
            numericLocals.clear();
            numericFloatLocals.clear();
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
                            std::string z = "$c" + std::to_string(tempCounter++);
                            ir.addInstruction(defIRName, "const", {"0"}, z);
                            initial = z;
                        }
                        std::string cellObj = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(defIRName, "call", {"PyCell_New", initial}, cellObj);
                        ir.addInstruction(defIRName, "assign", {cellObj}, cellSlot);
                    }
                }
            }

            // B5 (lambda cells): for each *owned* cell in this scope, if the body of any
            // nested lambda (or nested def) references that name, the lambda's synthetic
            // must receive the cell via a hidden param. This is now handled in lowerLambda
            // itself (which sets funcFreeCells[lamName] and synthesizes the hidden cell
            // params on the lambda's IRFunction). This block previously added owned-cell
            // names to funcFreeCells[defIRName] (incoming cells), which was incorrect —
            // owned cells are not incoming cells. With the lowerLambda fix, this block is
            // no longer needed and is intentionally left as a no-op scan (kept for any
            // future cross-lambda forwarding needs).
            {
                std::function<void(const ASTNode*)> markLambdaDemands = [&](const ASTNode* n) {
                    if (!n) return;
                    // No-op: lambda closure analysis is now done in lowerLambda.
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

            // P0: before body lowering, bind structured layouts from trailing defaults
            // onto the corresponding parameter names so for-loops see pairs/bodies types.
            if (!defaults.empty()) {
                size_t nFixed = bareParams.size();
                for (size_t i = 0; i < node->args.size(); ++i) {
                    if (!node->args[i].empty() && node->args[i][0] == '*') { nFixed = i; break; }
                }
                size_t ndef = defaults.size();
                size_t firstDefault = (ndef <= nFixed) ? (nFixed - ndef) : nFixed;
                for (size_t di = 0; di < ndef; ++di) {
                    size_t pi = firstDefault + di;
                    if (pi >= bareParams.size()) break;
                    const std::string& pname = bareParams[pi];
                    const std::string& slot = defaults[di];
                    copyLayoutMaps(slot, pname);
                    if (structuredElementLayout.count(slot))
                        markStructuredList(pname, structuredElementLayout[slot]);
                    if (pairOfStructuredLayout.count(slot))
                        markPairOfStructured(pname, pairOfStructuredLayout[slot]);
                    // Also from original default expr names (SYSTEM/PAIRS) if slot chase missed
                    if (!structuredElementLayout.count(pname) && !pairOfStructuredLayout.count(pname)) {
                        if (pname == "bodies" && structuredElementLayout.count("SYSTEM"))
                            markStructuredList(pname, structuredElementLayout["SYSTEM"]);
                        if (pname == "pairs" && pairOfStructuredLayout.count("PAIRS"))
                            markPairOfStructured(pname, pairOfStructuredLayout["PAIRS"]);
                        else if (pname == "pairs" && structuredElementLayout.count("SYSTEM"))
                            markPairOfStructured(pname, structuredElementLayout["SYSTEM"]);
                    }
                }
            }

            // Param type inference: scan the function body (without lowering) to
            // seed param types from numeric use contexts. This lets the body lowering
            // emit native arithmetic for params and record native self-recursive
            // call-site signatures, which in turn drives A6 specialized-variant
            // generation (critical for recursive numeric functions like fib).
            // Also infers a numeric return type (fixpoint through self-recursion)
            // so that call results within the body are typed and the `add` of two
            // recursive calls stays native instead of falling back to PyNumber_Add.
            {
                std::unordered_map<std::string, std::string> paramTypes;
                std::string inferredRetType;
                
                // Create a local copy of numericFloatLocals for this function's type inference
                // This prevents float-typed parameter names from leaking into the module scope
                std::unordered_set<std::string> localNumericFloatLocals = numericFloatLocals;
                
                inferParamTypesFromBody(node, bareParams, paramTypes, &inferredRetType);
                
                // Apply inferred types only to this function's parameters
                for (const auto& p : bareParams) {
                    const std::string& t = paramTypes[p];
                    if (t.empty()) continue;
                    // Don't shadow cell-backed names or names already typed by defaults.
                    if (isCellBackedHere(p)) continue;
                    noteType(p, t);
                    if (t == "int") numericLocals.insert(p);
                    else if (t == "float") localNumericFloatLocals.insert(p);
                }
                
                // Seed the return type so that call results within this body
                // inherit the numeric type. Without this, `fib(n-1) + fib(n-2)`
                // sees both call results as "boxed" and the add goes through
                // PyNumber_Add even when fib provably returns int.
                if (inferredRetType == "int" || inferredRetType == "float") {
                    currentFnReturnType = inferredRetType;
                    // Pre-record on the IRFunction so call-result type lookup
                    // (fn.returnType at lowerCall) sees the right type during
                    // this function's body lowering.
                    for (auto& fnr : ir.functions) {
                        if (fnr.name == defIRName) { fnr.returnType = inferredRetType; break; }
                    }
                }
                
                // Now update the global numericFloatLocals with only this function's inferred types
                // This ensures that any float-typed parameters from nested functions don't leak
                // into the module scope
                numericFloatLocals = localNumericFloatLocals;
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
            // Restore numericLocals/numericFloatLocals to outer scope
            numericLocals = savedNumericLocals;
            numericFloatLocals = savedNumericFloatLocals;
            activeTries = savedActiveTries;
            loopTryDepths = savedLoopTryDepths;
            currentFunc = saved;   // restore context for siblings (important for top-level code after defs)
            // Build the qualified name before popping, for emitFuncValue below.
            std::string nestedQualName;
            if (funcQualNameStack.size() > 1) {
                for (size_t i = 0; i < funcQualNameStack.size(); ++i) {
                    if (i > 0) nestedQualName += ".<locals>.";
                    nestedQualName += funcQualNameStack[i];
                }
            }
            funcQualNameStack.pop_back();
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
                std::string displayNm = nestedQualName.empty() ? node->id : nestedQualName;
                std::string fv = emitFuncValue(defIRName, displayNm);
                ir.addInstruction(currentFunc, "assign", {fv}, node->id);
                noteType(node->id, "str");
                // Decorators, bottom-up: name = decoN(...(deco1(name))...).
                // Each application: lower the decorator expression (a Name or a
                // factory Call), then Pyc_Apply it to the current value.
                for (auto it = decorators.rbegin(); it != decorators.rend(); ++it) {
                    std::string dv = lowerExpr(*it);
                    std::string z = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, z);
                    std::string argList = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, argList);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, node->id}, "");
                    std::string decorated = "$t" + std::to_string(tempCounter++);
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
                moduleNameAliases[name] = orig;

                if (compiledModules.count(orig) > 0) {
                    // Module was compiled — pyc_run_module runs it (once,
                    // cached via sys.modules) and returns its dict directly.
                    std::string modConst = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + orig + "\""}, modConst, "str");
                    std::string dictLoad = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"pyc_run_module", modConst}, dictLoad);

                    // A bare dotted import with no asname (e.g. `import
                    // a.b.c`) binds only the top-level component `a`, not
                    // the deepest submodule — `orig` names the leaf we just
                    // force-loaded (via pyc_run_module's parent recursion,
                    // wiring the whole a -> a.b -> a.b.c chain), but the
                    // value actually bound is `a`'s own dict, fetched
                    // separately below (a cheap cache hit by now). An
                    // asname (`import a.b.c as x`) binds directly to the
                    // leaf's dict — `orig` and `name` are unrelated strings
                    // in that case, so this check doesn't match.
                    bool isBareDottedImport = (orig != name) && orig.rfind(name + ".", 0) == 0;
                    if (isBareDottedImport) {
                        std::string topConst = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + name + "\""}, topConst, "str");
                        std::string topDict = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"pyc_run_module", topConst}, topDict);
                        ir.addInstruction(currentFunc, "assign", {topDict}, name);
                    } else {
                        ir.addInstruction(currentFunc, "assign", {dictLoad}, name);
                    }
                    noteType(name, "dict");
                } else {
                    // Module not found as a real compiled file — either a
                    // synthetic/built-in module (os, math, ...), which
                    // pyc_import_failed returns a real dict for, or a
                    // genuinely unresolvable name, which it reports as an
                    // ImportError and returns null for. Either way the
                    // static type is "dict" so that `X.attr(...)` call
                    // chains on a bare `import X` route through the
                    // generic dict-dispatch path below (see lowerCall's
                    // method-call handling) instead of silently mis-typing
                    // as "boxed" and never dispatching at all.
                    std::string modName = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + orig + "\""}, modName, "str");
                    std::string failResult = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"pyc_import_failed", modName}, failResult);

                    ir.addInstruction(currentFunc, "assign", {failResult}, name);
                    noteType(name, "dict");
                }
            }
            return;
         } else if (node->type == "ImportFrom") {
             // from math import sqrt
             // from utils import *
             // from . import sibling / from .. import x / from .rel import y —
             // relative, resolved against this module's own package context
             // (currentModulePackage, its __package__ equivalent).
             // B7: If the module was compiled, call __module__<mod> and look up names.
             // Otherwise emit pyc_import_failed.
             std::string mod;
             if (node->level > 0) {
                 // Unresolvable (relative import with no package context —
                 // e.g. a directly-run main script — or beyond the
                 // top-level package) leaves mod empty, same as it always
                 // has: falls through the `!mod.empty()` guard below with
                 // no binding, matching CPython raising ImportError here.
                 mod = resolveRelativeImport(currentModulePackage, node->level, node->id).value_or("");
             } else {
                 mod = node->id;
             }
             if (!mod.empty()) {
                if (compiledModules.count(mod) > 0) {
                    // Module was compiled — pyc_run_module runs it (once,
                    // cached via sys.modules) and returns its dict directly.
                    std::string modConst = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + mod + "\""}, modConst, "str");
                    std::string moduleDict = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"pyc_run_module", modConst}, moduleDict);

                    // Detect `from X import *`. The parser puts the literal "*"
                    // into node->args. We expand to the imported module's known
                    // (non-underscore) globals at lowering time, so each name
                    // becomes a real module global in the main module.
                    // exportPairs holds (originalName, localBindTarget) — the
                    // original name is what's actually looked up (as a dict
                    // attribute or, below, as a submodule's own dotted path);
                    // the target is what gets bound in this module (differs
                    // from the original only for `from X import Y as Z`).
                    std::vector<std::pair<std::string, std::string>> exportPairs;
                    bool isStar = false;
                    for (const auto& n : node->args) {
                        if (n == "*") { isStar = true; break; }
                    }
                    if (isStar) {
                        auto igit = importedModuleGlobals.find(mod);
                        if (igit != importedModuleGlobals.end()) {
                            for (const auto& nm : igit->second) {
                                if (nm.empty() || nm[0] == '_') continue;
                                exportPairs.push_back({nm, nm});
                            }
                        }
                        // If the imported module's globals weren't collected
                        // (e.g. parse failure), fall through with an empty
                        // export list — no `*` global is created.
                    } else {
                        for (size_t i = 0; i + 1 < node->importNames.size(); i += 2) {
                            exportPairs.push_back({node->importNames[i], node->importNames[i + 1]});
                        }
                    }

                    for (const auto& pr : exportPairs) {
                        const std::string& origName = pr.first;
                        const std::string& targetName = pr.second;
                        ir.addModuleGlobal(targetName);
                        std::string submoduleDotted = mod + "." + origName;
                        if (compiledModules.count(submoduleDotted) > 0) {
                            // `origName` is itself a compiled submodule (e.g.
                            // `from package_a import mod_a1`) — nothing ever
                            // assigns a "mod_a1" attribute inside package_a's
                            // own __init__.py dict, so load the submodule
                            // directly instead of doing Pyc_GetItem against
                            // the package dict.
                            std::string subModConst = "$c" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "const", {"\"" + submoduleDotted + "\""}, subModConst, "str");
                            std::string subDict = "$t" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "call", {"pyc_run_module", subModConst}, subDict);
                            ir.addInstruction(currentFunc, "assign", {subDict}, targetName);
                            noteType(targetName, "dict");
                        } else {
                            std::string attrKey = "$c" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "const", {"\"" + origName + "\""}, attrKey, "str");
                            std::string attrVal = "$t" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", moduleDict, attrKey}, attrVal);
                            ir.addInstruction(currentFunc, "assign", {attrVal}, targetName);
                        }
                    }
                } else {
                    // Module not found as a real compiled file — it's either
                    // a synthetic/built-in module (re, os, math, json, ...)
                    // or genuinely unresolvable; either way pyc_import_failed
                    // returns the right thing (a synthetic dict, or an
                    // ImportError-reporting empty result) for each imported
                    // name to be looked up against.
                    std::string modName = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + mod + "\""}, modName, "str");
                    std::string moduleDict = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"pyc_import_failed", modName}, moduleDict);

                    // `from X import *` on a synthetic module: expand to its
                    // known exported names, mirroring how importedModuleGlobals
                    // expands `*` for real compiled modules above. An unknown
                    // module's `*` still binds nothing (no export list to
                    // expand from), matching prior behavior.
                    bool isStar = false;
                    for (const auto& n : node->args) if (n == "*") { isStar = true; break; }
                    std::vector<std::pair<std::string, std::string>> exportPairs; // (orig, target)
                    if (isStar) {
                        auto expIt = syntheticModuleExports().find(mod);
                        if (expIt != syntheticModuleExports().end()) {
                            for (const auto& nm : expIt->second) exportPairs.push_back({nm, nm});
                        }
                    } else {
                        for (size_t i = 0; i + 1 < node->importNames.size(); i += 2) {
                            exportPairs.push_back({node->importNames[i], node->importNames[i + 1]});
                        }
                    }

                    for (const auto& pr : exportPairs) {
                        const std::string& origName = pr.first;
                        const std::string& name = pr.second;
                        ir.addModuleGlobal(name);

                        // Special case: time.perf_counter - directly use the callable token
                        if (mod == "time" && origName == "perf_counter") {
                            std::string tokenVal = "$t" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "const", {"\"Pyc_Time_PerfCounter\""}, tokenVal, "str");
                            callableTokenTemps.insert(tokenVal);
                            ir.addInstruction(currentFunc, "assign", {tokenVal}, name);
                            continue;
                        }

                        // `from datetime import date/datetime/timedelta [as X]` —
                        // record the alias so bare `X(...)` calls (lowerCall)
                        // and `X.today()`/`X.now()` (lowerMethodCall) route to
                        // the same direct-call construction as the
                        // `datetime.date(...)`-qualified form. The module dict
                        // has no real "date"/"datetime"/"timedelta" entries
                        // (only nested `today`/`now` tokens under
                        // makeDatetimeModuleDict()), so the fallback
                        // Pyc_GetItem binding below is a harmless None — real
                        // code always calls the alias directly, which is
                        // intercepted before that binding is ever read.
                        if (mod == "datetime" &&
                            (origName == "date" || origName == "datetime" || origName == "timedelta")) {
                            datetimeCtorAliases[name] = origName;
                        }
                        // `from pathlib import Path [as X]` — same rationale.
                        if (mod == "pathlib" && origName == "Path") {
                            pathCtorAliases.insert(name);
                        }
                        // `from hashlib import md5/sha1/sha256 [as X]` — same rationale.
                        if (mod == "hashlib" &&
                            (origName == "md5" || origName == "sha1" || origName == "sha256")) {
                            hashlibCtorAliases[name] = origName;
                        }
                        // `from copy import copy/deepcopy [as X]` — same rationale.
                        if (mod == "copy" && (origName == "copy" || origName == "deepcopy")) {
                            copyFuncAliases[name] = origName;
                        }
                        // `from csv import writer [as X]` — same rationale.
                        if (mod == "csv" && origName == "writer") {
                            csvWriterCtorAliases.insert(name);
                        }
                        // `from itertools import groupby [as X]` — same rationale.
                        if (mod == "itertools" && origName == "groupby") {
                            groupbyCtorAliases.insert(name);
                        }
                        // `from collections import deque [as X]` — same rationale.
                        if (mod == "collections" && origName == "deque") {
                            dequeCtorAliases.insert(name);
                        }
                        // `from decimal import Decimal [as X]` — same rationale.
                        if (mod == "decimal" && origName == "Decimal") {
                            decimalCtorAliases.insert(name);
                        }

                        std::string attrKey = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + origName + "\""}, attrKey, "str");
                        std::string attrVal = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", moduleDict, attrKey}, attrVal);
                        // Mark attrVal as a callable token temp and as owned (Pyc_GetItem returns new ref)
                        callableTokenTemps.insert(attrVal);
                        // Note: markOwned is called by codegen for call instructions
                        ir.addInstruction(currentFunc, "assign", {attrVal}, name);
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
                std::string nameConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + node->children[0]->id + "\""}, nameConst, "str");
                std::string emptyMsg = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"\""}, emptyMsg, "str");
                std::string exc = "$t" + std::to_string(tempCounter++);
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
            std::string enterMethod = "$t" + std::to_string(tempCounter++);
            std::string enterMethodToken = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"__enter__\""}, enterMethodToken, "str");
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", ctxExpr, enterMethodToken}, enterMethod);
            // Build args list: [self]. The count operand must be a
            // properly-declared const, not a bare literal string — an
            // undeclared operand name resolves to a null pointer at
            // codegen (see getOrLoad's fallback in Codegen.cpp), which
            // made PyList_NewBoxed allocate a *zero-length* list here
            // (this was a real, previously undiscovered bug: __enter__/
            // __exit__ never actually received `self`, since
            // PyList_SetItemBoxed's boxed-list path only writes when the
            // index is already within the list's current size).
            std::string enterCountConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, enterCountConst);
            std::string enterArgs = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", enterCountConst}, enterArgs);
            std::string enterIdx = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, enterIdx);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", enterArgs, enterIdx, ctxExpr}, "");
            // Call __enter__(self) via Pyc_Apply
            std::string enterResult = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_Apply", enterMethod, enterArgs}, enterResult);
            // Bind to target variable if present. This "assign" is emitted
            // directly (not via lowerAssign), so it doesn't get
            // lowerAssign's generic noteType(target, typeOf(val))
            // propagation for free — done explicitly here so the body's
            // method calls (e.g. `f.write(...)`, typeOf-gated below) can
            // see the target's type. __enter__ conventionally returns
            // `self` (as PyBuiltin_Open's file dict does), so the
            // target's type is the *context manager's* type, not
            // enterResult's own (usually-untracked) type.
            if (withItem->children.size() >= 2 && withItem->children[1]) {
                std::string targetName = withItem->children[1]->id;
                ir.addInstruction(currentFunc, "assign", {enterResult}, targetName);
                noteType(targetName, typeOf(ctxExpr));
            }
            // Execute body
            for (size_t i = 1; i < node->children.size(); ++i) {
                if (node->children[i]) lower(node->children[i].get());
            }
            // Call __exit__ with None, None, None (simplified)
            std::string exitMethod = "$t" + std::to_string(tempCounter++);
            std::string exitMethodToken = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"__exit__\""}, exitMethodToken, "str");
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", ctxExpr, exitMethodToken}, exitMethod);
            std::string exitCountConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"4"}, exitCountConst);
            std::string exitArgs = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", exitCountConst}, exitArgs);
            std::string exitIdx = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, exitIdx);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", exitArgs, exitIdx, ctxExpr}, "");
            std::string noneVal = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "nconst", {}, noneVal);
            for (int i = 1; i < 4; ++i) {
                std::string idx = "$t" + std::to_string(tempCounter++);
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
                    std::string trueConst = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "bconst", {"True"}, trueConst, "bool");
                    matchCond = trueConst;
                    if (hasBinding) {
                        ir.addInstruction(currentFunc, "assign", {subject}, pattern->value);
                    }
                } else if (pattern->type == "MatchValue") {
                    if (pattern->children.empty()) continue;
                    std::string patternVal = lowerExpr(pattern->children[0].get());
                    if (patternVal.empty()) continue;
                    std::string cmpResult = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "icmp", {"Eq", subject, patternVal}, cmpResult, "bool");
                    matchCond = cmpResult;
                } else if (pattern->type == "MatchSingleton") {
                    std::string cmpResult = "$t" + std::to_string(tempCounter++);
                    if (pattern->value == "None") {
                        std::string noneConst = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "nconst", {}, noneConst);
                        ir.addInstruction(currentFunc, "icmp", {"Eq", subject, noneConst}, cmpResult, "bool");
                        matchCond = cmpResult;
                    } else {
                        std::string boolVal = pattern->value == "True" ? "True" : "False";
                        std::string boolConst = "$t" + std::to_string(tempCounter++);
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
        } else if (node->type == "SetComp") {
            lowerSetComp(node);
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
        if (node->lineno > 0) { currentLineno = node->lineno; ir.currentLineno = currentLineno; }

        if (node->type == "Constant") {
            std::string res = "$c" + std::to_string(tempCounter++);
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
                std::string realConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "fconst", {std::to_string(real)}, realConst, "float");
                std::string imagConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "fconst", {std::to_string(imag)}, imagConst, "float");
                std::string complexRes = "$t" + std::to_string(tempCounter++);
                // Use a special IR instruction that passes native doubles
                ir.addInstruction(currentFunc, "call", {"PyComplex_New", realConst, imagConst}, complexRes);
                complexVars.insert(complexRes);
                noteType(res, "boxed");
                return complexRes;
            } else if (node->is_bytes) {
                // Dedicated opcode (not the generic "const" str-literal
                // path) so codegen can pass an explicit length rather than
                // relying on a NUL-terminated C-string — the whole point
                // being that b"\x00\x01" round-trips correctly. See
                // Codegen.cpp's "bytesconst" handler.
                ir.addInstruction(currentFunc, "bytesconst", {val}, res, "bytes");
                noteType(res, "bytes");
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
                std::string res = "$t" + std::to_string(tempCounter++);
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
                        std::string zero = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"0"}, zero);
                        std::string lst = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, lst);

                    std::string tok = "$c" + std::to_string(tempCounter++);
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
                std::string excNameConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + node->id + "\""}, excNameConst, "str");
                std::string excClass = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"pyc_make_exc_class", excNameConst}, excClass);
                return excClass;
            }
            // Same rationale as B13, for the handful of builtin type names
            // whose bare (uncalled) reference is a real, common pattern:
            // `collections.defaultdict(list)`/`defaultdict(int)`/etc. Only
            // covers the zero-arg-factory use case — the resulting token
            // is a callable, not a real "type" value, so e.g. comparing it
            // against another type or subclassing it is not supported.
            {
                static const std::unordered_map<std::string, const char*> builtinFactoryTokens = {
                    {"dict", "PyBuiltin_DictFactory"},
                };
                auto bfIt = builtinFactoryTokens.find(node->id);
                if (bfIt != builtinFactoryTokens.end() && !isShadowedLocal(node->id)) {
                    std::string tokenVal = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + std::string(bfIt->second) + "\""}, tokenVal, "str");
                    callableTokenTemps.insert(tokenVal);
                    return tokenVal;
                }
            }
            // Builtin functions as first-class values: emit a callable
            // token string registered in the runtime's g_callableRegistry.
            // This enables sorted(key=len), functools.reduce(max, ...),
            // apply(abs, x), etc. — the token is dispatched via Pyc_Apply
            // to the corresponding pyc_adapt_* runtime adapter.
            {
                static const std::unordered_map<std::string, const char*> builtinValueTokens = {
                    {"len", "pyc_adapt_len"}, {"abs", "pyc_adapt_abs"},
                    {"int", "pyc_adapt_int"}, {"float", "pyc_adapt_float"},
                    {"bool", "pyc_adapt_bool"}, {"type", "pyc_adapt_type"},
                    {"repr", "pyc_adapt_repr"}, {"id", "pyc_adapt_id"},
                    {"callable", "pyc_adapt_callable"},
                    {"ord", "pyc_adapt_ord"}, {"chr", "pyc_adapt_chr"},
                    {"hex", "pyc_adapt_hex"}, {"oct", "pyc_adapt_oct"},
                    {"bin", "pyc_adapt_bin"}, {"round", "pyc_adapt_round"},
                    {"divmod", "pyc_adapt_divmod"}, {"pow", "pyc_adapt_pow"},
                    {"list", "pyc_adapt_list"}, {"tuple", "pyc_adapt_tuple"},
                    {"set", "pyc_adapt_set"}, {"range", "pyc_adapt_range"},
                    {"print", "pyc_adapt_print"},
                    {"min", "pyc_adapt_min"}, {"max", "pyc_adapt_max"},
                    {"sum", "pyc_adapt_sum"}, {"sorted", "pyc_adapt_sorted"},
                    {"any", "pyc_adapt_any"}, {"all", "pyc_adapt_all"},
                    {"reversed", "pyc_adapt_reversed"},
                    {"map", "pyc_adapt_map"},
                    {"filter", "pyc_adapt_filter"},
                    {"enumerate", "pyc_adapt_enumerate"},
                    {"zip", "pyc_adapt_zip"},
                    {"isinstance", "pyc_adapt_isinstance"},
                    {"complex", "pyc_adapt_complex"},
                    {"bytes", "pyc_adapt_bytes"},
                    {"bytearray", "pyc_adapt_bytearray"},
                    {"str", "pyc_adapt_str"},
                };
                auto bvIt = builtinValueTokens.find(node->id);
                if (bvIt != builtinValueTokens.end() && !isShadowedLocal(node->id)) {
                    std::string tokenVal = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + std::string(bvIt->second) + "\""}, tokenVal, "str");
                    callableTokenTemps.insert(tokenVal);
                    return tokenVal;
                }
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
        } else if (node->type == "Set") {
            return lowerSet(node);
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
                    std::string zero = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, zero);
                    std::string lst = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, lst);

                    // token (synthetic name string)
                    std::string tok = "$c" + std::to_string(tempCounter++);
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
        } else if (node->type == "SetComp") {
            return lowerSetComp(node);
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
        std::string result = "$t" + std::to_string(tempCounter++);
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
            std::string callRes = "$t" + std::to_string(tempCounter++);
            std::vector<std::string> callOps;
            callOps.push_back(funcName);
            ir.addInstruction(currentFunc, "call", callOps, callRes);
            subgenVal = callRes;
            // Store subgen result in a slot to keep it alive through the loop
            std::string subgenSlot = "__yfrom_subgen_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {subgenVal}, subgenSlot);
            subgenVal = subgenSlot;
            // Use boxed index for comparison and iteration
            std::string idxVar = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxVar);
            std::string loopLabel = "yfrom_loop_" + std::to_string(tempCounter);
            std::string bodyLabel = "yfrom_body_" + std::to_string(tempCounter);
            std::string exitLabel = "yfrom_exit_" + std::to_string(tempCounter);
            tempCounter++;
            ir.addInstruction(currentFunc, "label", {}, loopLabel);
            // len = PyList_SizeBoxed(subgenVal) returns boxed int
            std::string lenRes = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", subgenVal}, lenRes);
            std::string lenSlot = "__sl_yfrom_" + std::to_string(tempCounter);
            ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlot);
            std::string cmpRes = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", idxVar, lenSlot}, cmpRes);
            ir.addInstruction(currentFunc, "br", {cmpRes, bodyLabel, exitLabel});
            ir.addInstruction(currentFunc, "label", {}, bodyLabel);
            std::string elemRes = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", subgenVal, idxVar}, elemRes);
            std::string yld = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"pyc_yield_collect", elemRes}, yld);
            std::string oneRes = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, oneRes);
            std::string nextIdx = "$t" + std::to_string(tempCounter++);
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

    // Param type inference: scan a function body AST (without lowering) to infer
    // parameter types from how they are used in numeric contexts. This runs before
    // body lowering so that noteType/numericLocals are seeded and the lowering itself
    // produces native arithmetic + native self-recursive call-site signatures.
    //
    // Inference rules (conservative — any conflicting or non-numeric use cancels):
    //   - BinOp(param, op, numeric_constant)  → param inherits constant's numeric type
    //   - Compare(param, op, numeric_constant) → param inherits constant's numeric type
    //   - UnaryOp(-param) / UnaryOp(+param)    → param is numeric (int if unknown)
    //   - Return(param) where other returns are numeric → consistent
    //   - Any non-numeric use (str.method, subscript, container literal, etc.) → cancel
    //
    // `params` is the set of bare parameter names to infer. Returns a map
    // param name → "int" | "float" | "" (unknown/contradictory).
    // `outReturnType` (optional) receives the inferred return type ("int"/"float"/"boxed")
    // computed as a single-function fixpoint: non-recursive returns seed it, then
    // self-recursive calls are assumed to return the current best estimate.
    void inferParamTypesFromBody(const ASTNode* funcNode,
                                 const std::vector<std::string>& bareParams,
                                 std::unordered_map<std::string, std::string>& outTypes,
                                 std::string* outReturnType = nullptr) {
        // Collect param set (skip *args/**kwargs markers already stripped by caller).
        std::unordered_set<std::string> paramSet(bareParams.begin(), bareParams.end());
        for (const auto& p : bareParams) outTypes[p] = "";

        // Track per-param: inferred type ("int"/"float"), and whether we've seen a
        // non-numeric use that disqualifies it.
        std::unordered_map<std::string, std::string> inferred;
        std::unordered_set<std::string> disqualified;
        for (const auto& p : bareParams) inferred[p] = "";

        // Self-recursive return type estimate, updated by the fixpoint below.
        // Used by exprNumType to type self-calls when computing return expr types.
        std::string selfReturnType;

        // Determine the numeric type of an AST expression *as seen by the caller*,
        // WITHOUT relying on valueTypes (params aren't lowered yet). Constants and
        // already-typed names are resolved; everything else is "boxed".
        std::function<std::string(const ASTNode*)> exprNumType = [&](const ASTNode* n) -> std::string {
            if (!n) return "boxed";
            if (n->type == "Constant") {
                if (n->is_none || n->is_str) return "boxed";
                if (n->is_float) return "float";
                if (n->is_bool) return "int";
                if (n->is_complex) return "boxed";
                return "int";
            }
            if (n->type == "Name") {
                auto it = inferred.find(n->id);
                if (it != inferred.end() && !it->second.empty()) return it->second;
                // A name we're inferring but haven't determined yet: treat as
                // "unknown-num" so BinOp(param, op, other_param) can still work
                // once the other side resolves. For now return "boxed" to be safe.
                if (paramSet.count(n->id)) return "num?";  // provisional
                auto vit = valueTypes.find(n->id);
                if (vit != valueTypes.end()) {
                    const std::string& t = vit->second;
                    if (t == "int" || t == "i64" || t == "bool") return "int";
                    if (t == "float") return "float";
                }
                return "boxed";
            }
            if (n->type == "BinOp") {
                std::string op = n->id;
                std::string lt = exprNumType(n->children[0].get());
                std::string rt = n->children.size() > 1 ? exprNumType(n->children[1].get()) : "boxed";
                auto isNum = [](const std::string& t){ return t=="int" || t=="float" || t=="num?"; };
                if (op == "/" || op == "truediv") {
                    return (isNum(lt) && isNum(rt)) ? "float" : "boxed";
                }
                if (isNum(lt) && isNum(rt)) {
                    return (lt == "float" || rt == "float") ? "float" : "int";
                }
                return "boxed";
            }
            if (n->type == "UnaryOp") {
                std::string ot = exprNumType(n->children[0].get());
                if (ot == "int" || ot == "float" || ot == "num?") return ot == "num?" ? "int" : ot;
                return "boxed";
            }
            if (n->type == "Call") {
                // Calls to numeric builtins: abs, int, round, ord, len, pow, divmod
                if (!n->children.empty() && n->children[0] && n->children[0]->type == "Name") {
                    const std::string& fn = n->children[0]->id;
                    if (fn == "abs" || fn == "int" || fn == "round" || fn == "ord" ||
                        fn == "len" || fn == "pow" || fn == "divmod") {
                        return "int";
                    }
                    if (fn == "float") return "float";
                    // Self-recursive call: assume it returns the current best estimate
                    // of this function's return type. This enables a fixpoint where
                    // `return fib(n-1) + fib(n-2)` sees fib's return as int once the
                    // base case `return n` establishes int.
                    if (outReturnType && fn == funcNode->id && !selfReturnType.empty()) {
                        return selfReturnType;
                    }
                }
                return "boxed";
            }
            return "boxed";
        };

        // A use of a param in a context that proves it's numeric.
        auto noteNumericUse = [&](const std::string& pname, const std::string& t) {
            if (disqualified.count(pname)) return;
            if (t != "int" && t != "float") return;
            std::string& cur = inferred[pname];
            if (cur.empty()) cur = t;
            else if (cur != t) {
                // Promote mixed int/float → float (Python semantics)
                if (cur == "int" && t == "float") cur = "float";
                else if (cur == "float" && t == "int") { /* keep float */ }
                else { disqualified.insert(pname); cur = ""; }
            }
        };

        // Walk the body, not descending into nested FunctionDef/Lambda.
        std::function<void(const ASTNode*)> walk = [&](const ASTNode* n) {
            if (!n) return;
            if (n->type == "FunctionDef" || n->type == "Lambda") return;
            if (n->type == "BinOp") {
                const ASTNode* l = n->children[0].get();
                const ASTNode* r = n->children.size() > 1 ? n->children[1].get() : nullptr;
                std::string lt = exprNumType(l);
                std::string rt = exprNumType(r);
                // If one side is a param and the other is a proven numeric constant,
                // infer the param's type.
                if (l && l->type == "Name" && paramSet.count(l->id) && rt != "boxed" && rt != "num?") {
                    noteNumericUse(l->id, rt);
                }
                if (r && r->type == "Name" && paramSet.count(r->id) && lt != "boxed" && lt != "num?") {
                    noteNumericUse(r->id, lt);
                }
                // Both sides params: if both are params and at least one is already
                // resolved, propagate.
                if (l && r && l->type == "Name" && r->type == "Name" &&
                    paramSet.count(l->id) && paramSet.count(r->id)) {
                    const std::string& lt2 = inferred[l->id];
                    const std::string& rt2 = inferred[r->id];
                    if (!lt2.empty() && rt2.empty()) noteNumericUse(r->id, lt2);
                    if (!rt2.empty() && lt2.empty()) noteNumericUse(l->id, rt2);
                }
            } else if (n->type == "Compare") {
                const ASTNode* l = n->children[0].get();
                const ASTNode* r = n->children.size() > 1 ? n->children[1].get() : nullptr;
                std::string lt = exprNumType(l);
                std::string rt = exprNumType(r);
                if (l && l->type == "Name" && paramSet.count(l->id) && rt != "boxed" && rt != "num?") {
                    noteNumericUse(l->id, rt);
                }
                if (r && r->type == "Name" && paramSet.count(r->id) && lt != "boxed" && lt != "num?") {
                    noteNumericUse(r->id, lt);
                }
            } else if (n->type == "UnaryOp") {
                const ASTNode* operand = n->children[0].get();
                if (operand && operand->type == "Name" && paramSet.count(operand->id)) {
                    // -param or +param implies numeric (int unless otherwise known)
                    std::string& cur = inferred[operand->id];
                    if (cur.empty()) noteNumericUse(operand->id, "int");
                }
            } else if (n->type == "Return") {
                // Returning a param: consistent with the function's return type.
                // We don't use this to set the type, but it doesn't disqualify.
                // (If the param was inferred numeric, returning it is fine.)
                // No action needed.
            } else if (n->type == "Call") {
                // A param passed to a call: only disqualify if the callee is a
                // known non-numeric builtin (str, list, dict, print, etc.) or a
                // method call on the param. For unknown/generic calls we stay
                // neutral (the call-site analysis handles cross-function types).
                if (!n->children.empty() && n->children[0]) {
                    const ASTNode* callee = n->children[0].get();
                    if (callee->type == "Attribute") {
                        // param.method(...) — method call on a param implies non-numeric
                        const ASTNode* base = callee->children.empty() ? nullptr : callee->children[0].get();
                        if (base && base->type == "Name" && paramSet.count(base->id)) {
                            disqualified.insert(base->id);
                            inferred[base->id] = "";
                        }
                    }
                }
            } else if (n->type == "Attribute") {
                // param.attr — attribute access on a param implies non-numeric
                const ASTNode* base = n->children.empty() ? nullptr : n->children[0].get();
                if (base && base->type == "Name" && paramSet.count(base->id)) {
                    disqualified.insert(base->id);
                    inferred[base->id] = "";
                }
            } else if (n->type == "Subscript") {
                // param[...] — subscript on a param implies non-numeric (it's a container)
                const ASTNode* base = n->children.empty() ? nullptr : n->children[0].get();
                if (base && base->type == "Name" && paramSet.count(base->id)) {
                    disqualified.insert(base->id);
                    inferred[base->id] = "";
                }
            }
            // Recurse into children (except nested defs/lambdas handled above)
            for (const auto& c : n->children) walk(c.get());
        };
        for (const auto& c : funcNode->children) {
            if (c && (c->type == "Default" || c->type == "Decorator")) continue;
            walk(c.get());
        }

        // Finalize: only emit types for params that were consistently numeric.
        for (const auto& p : bareParams) {
            if (disqualified.count(p)) { outTypes[p] = ""; continue; }
            outTypes[p] = inferred[p];
        }

        // Return type fixpoint: iterate up to 5 times. Seed from non-recursive
        // return expressions, then propagate through self-recursive calls.
        if (outReturnType) {
            std::string best = "boxed";
            for (int iter = 0; iter < 5; ++iter) {
                selfReturnType = best;
                std::string candidate = "boxed";
                bool first = true;
                // Within a single iteration, update selfReturnType as soon as we
                // find a non-recursive return, so later recursive returns in the
                // same pass see the improved estimate. This accelerates convergence.
                std::function<void(const ASTNode*)> collectReturns = [&](const ASTNode* n) {
                    if (!n) return;
                    if (n->type == "FunctionDef" || n->type == "Lambda") return;
                    if (n->type == "Return") {
                        const ASTNode* val = nullptr;
                        for (const auto& c : n->children) {
                            if (c && c->type != "Default") { val = c.get(); break; }
                        }
                        std::string rt = val ? exprNumType(val) : "boxed";
                        if (rt == "num?") rt = "int";  // unresolved param → assume int
                        if (first) { candidate = rt; first = false; }
                        else if (candidate != rt) {
                            // Mixed return types → promote int+float→float, else boxed
                            auto isNum = [](const std::string& t){ return t=="int"||t=="float"; };
                            if (isNum(candidate) && isNum(rt))
                                candidate = (candidate=="float"||rt=="float") ? "float" : "int";
                            else
                                candidate = "boxed";
                        }
                        // Update selfReturnType for subsequent returns in this pass.
                        selfReturnType = candidate;
                    }
                    for (const auto& c : n->children) collectReturns(c.get());
                };
                for (const auto& c : funcNode->children) {
                    if (c && (c->type == "Default" || c->type == "Decorator")) continue;
                    collectReturns(c.get());
                }
                if (candidate == best) break;  // fixpoint reached
                best = candidate;
            }
            *outReturnType = best;
        }
    }

    // A6: Generate specialized function variants based on call-site type info.
    // For each function that is called with consistent arg counts, generate
    // a specialized variant for EACH unique type signature observed. This enables
    // speculative optimization: at call sites, runtime type guards dispatch to the
    // specialized variant when types match, falling back to the generic boxed path.
    // Handles both numeric types (int/float -> native i64/double) and non-numeric types
    // (str/list/dict -> PyObject*) for comprehensive type-based specialization.
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

            // Collect unique type signatures. Generate specialized variants only for
            // numeric types (int/float) where native i64/double params eliminate
            // boxing/unboxing overhead. Non-numeric types (str/list/dict) use PyObject*
            // params — same as generic — so no benefit from separate variants.
            std::set<std::string> uniqueSigs;
            for (const auto& sig : allSigs) {
                std::string normalized;
                bool allValid = true;
                for (const auto& t : sig) {
                    std::string nt = t;
                    if (nt == "i64") nt = "int";
                    if (nt == "int") normalized += "i";
                    else if (nt == "float") normalized += "f";
                    else { allValid = false; break; }  // Non-numeric types skipped
                }
                if (allValid && !normalized.empty()) {
                    uniqueSigs.insert(normalized);
                }
            }

            // Record which signatures have specialized variants (for codegen type guards)
            origFunc->specializedSignatures = uniqueSigs;

            for (const auto& sig : uniqueSigs) {
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

                // A6 native return: if the original function has a proven numeric return
                // type, the variant returns a native i64/double instead of a boxed
                // PyObject*. This eliminates PyInt_FromLong/PyFloat_FromDouble on every
                // return and enables fully native recursive chains (e.g. fib(n-1)+fib(n-2)
                // becomes a native i64 add with no boxing at any recursion level).
                //
                // origFunc->returnType comes from a body-only static guess
                // (inferParamTypesFromBody) made before real call-site types are
                // known, and origFunc->numericLocals/numericFloatLocals record which
                // params that same guess assumed int/float. If THIS variant's actual
                // (call-site-derived) signature disagrees with that guess for any
                // param — e.g. `def f(y): return y ** 2` guessed y as int, but the
                // only real call site passes a float — the guessed return type is
                // unreliable: forcing a native i64/double return type here would
                // make the LLVM function type disagree with what the body actually
                // computes, crashing codegen. Skip native-return propagation for
                // this variant and fall back to the safe, always-correct boxed
                // return path. Found via `def f(y): return y ** 2; f(3.5)`.
                bool sigMatchesInference = true;
                for (size_t i = 0; i < sig.size() && i < origFunc->args.size(); ++i) {
                    const std::string& pname = origFunc->args[i];
                    bool guessedInt = std::find(origFunc->numericLocals.begin(), origFunc->numericLocals.end(), pname) != origFunc->numericLocals.end();
                    bool guessedFloat = std::find(origFunc->numericFloatLocals.begin(), origFunc->numericFloatLocals.end(), pname) != origFunc->numericFloatLocals.end();
                    if ((sig[i] == 'f' && guessedInt) || (sig[i] == 'i' && guessedFloat)) {
                        sigMatchesInference = false;
                        break;
                    }
                }
                if (sigMatchesInference &&
                    (origFunc->returnType == "int" || origFunc->returnType == "float")) {
                    variant.nativeReturnType = origFunc->returnType;
                }

                ir.functions.push_back(variant);
            }
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
                            // Also check subscriptElementTypes for elements returned by function calls
                            auto sit = fnx.subscriptElementTypes.find(temp);
                            if (sit != fnx.subscriptElementTypes.end() && !sit->second.empty()) {
                                for (const auto& [idx, et] : sit->second) {
                                    if (et == "float" || et == "float_list" || et == "list_float") {
                                        globalElementTypes[gname][idx] = "float";
                                    } else if (et == "int" || et == "int_list" || et == "list_int") {
                                        globalElementTypes[gname][idx] = "int";
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
            
            // Check parameter defaults — defaults apply to trailing fixed params.
            // defaultGlobals[di] pairs with args[firstDefault + di], NOT args[di].
            size_t nFixed = fn.args.size();
            for (size_t i = 0; i < fn.paramNames.size(); ++i) {
                const auto& pn = fn.paramNames[i];
                if (!pn.empty() && pn[0] == '*') { nFixed = i; break; }
            }
            size_t ndef = fn.defaultGlobals.size();
            size_t firstDefault = (ndef > 0 && ndef <= nFixed) ? (nFixed - ndef) : nFixed;

            for (size_t di = 0; di < ndef; ++di) {
                size_t pi = firstDefault + di;
                if (pi >= fn.args.size()) break;
                const auto& paramName = fn.args[pi];
                std::string defaultSlot = fn.defaultGlobals[di];

                // P0: chase structured / pair layouts from default slot or known globals
                auto tryLayoutFrom = [&](const std::string& src) {
                    // Module-level visitor maps may not be available here; use IR maps
                    // on the module function and any function that has the name.
                    for (auto& srcFn : ir.functions) {
                        auto sel = srcFn.structuredElementLayout.find(src);
                        if (sel != srcFn.structuredElementLayout.end() && !sel->second.empty()) {
                            fn.structuredElementLayout[paramName] = sel->second;
                            return true;
                        }
                        auto pel = srcFn.pairOfStructuredLayout.find(src);
                        if (pel != srcFn.pairOfStructuredLayout.end() && !pel->second.empty()) {
                            fn.pairOfStructuredLayout[paramName] = pel->second;
                            return true;
                        }
                    }
                    // Also check visitor-level maps (still in scope during this pass)
                    if (structuredElementLayout.count(src)) {
                        fn.structuredElementLayout[paramName] = structuredElementLayout[src];
                        return true;
                    }
                    if (pairOfStructuredLayout.count(src)) {
                        fn.pairOfStructuredLayout[paramName] = pairOfStructuredLayout[src];
                        return true;
                    }
                    return false;
                };

                // Default slot name, then strip __default_<fn>_N → look at assigned globals
                tryLayoutFrom(defaultSlot);
                // Common nbody names
                if (paramName == "bodies" || paramName == "pairs") {
                    tryLayoutFrom("SYSTEM");
                    tryLayoutFrom("PAIRS");
                }
                // Homogeneous list_float / list_int defaults
                std::string defaultElemType = "boxed";
                if (globalToValueType.count(defaultSlot)) {
                    std::string vt = globalToValueType[defaultSlot];
                    if (vt == "list_float") defaultElemType = "float_list";
                    else if (vt == "list_int") defaultElemType = "int_list";
                }
                auto gtit = globalToTemp.find(defaultSlot);
                if (gtit != globalToTemp.end()) {
                    auto cit = tempContainerType.find(gtit->second);
                    if (cit != tempContainerType.end()) defaultElemType = cit->second;
                }
                if (defaultElemType == "float_list") {
                    for (size_t idx = 0; idx <= 20; idx++)
                        fn.subscriptElementTypes[paramName][idx] = "float";
                    fn.containerElementTypes[paramName][0] = "float_list";
                } else if (defaultElemType == "int_list") {
                    for (size_t idx = 0; idx <= 20; idx++)
                        fn.subscriptElementTypes[paramName][idx] = "int";
                    fn.containerElementTypes[paramName][0] = "int_list";
                }
            }

            // P0: also copy module structured layouts for bodies/pairs by global name defaults
            // when default slot chase failed but module globals SYSTEM/PAIRS are known.
            if (fn.structuredElementLayout.find("bodies") == fn.structuredElementLayout.end()) {
                if (structuredElementLayout.count("SYSTEM"))
                    fn.structuredElementLayout["bodies"] = structuredElementLayout["SYSTEM"];
            }
            if (fn.pairOfStructuredLayout.find("pairs") == fn.pairOfStructuredLayout.end()) {
                if (pairOfStructuredLayout.count("PAIRS"))
                    fn.pairOfStructuredLayout["pairs"] = pairOfStructuredLayout["PAIRS"];
                else if (structuredElementLayout.count("SYSTEM"))
                    fn.pairOfStructuredLayout["pairs"] = structuredElementLayout["SYSTEM"];
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

            // Fixed (non-*/* *) param count and which of those have defaults.
            // Params with defaults must stay boxed: defaults are often None, and
            // native-int unboxing turns `x is None` into a constant false + undef phi.
            size_t nFixed = declaredArgCount;
            for (size_t i = 0; i < func->paramNames.size(); ++i) {
                const auto& pn = func->paramNames[i];
                if (!pn.empty() && pn[0] == '*') { nFixed = i; break; }
            }
            size_t ndef = func->defaultGlobals.size();
            size_t firstDefault = (ndef > 0 && ndef <= nFixed) ? (nFixed - ndef) : declaredArgCount;

            // For each declared param, find the dominant type across all call sites.
            // A param is native only if EVERY call site supplies the same numeric type.
            // Mixed sites (e.g. f("a") and f(1)) must stay boxed on the generic function.
            for (size_t pi = 0; pi < declaredArgCount; ++pi) {
                if (pi >= firstDefault && pi < nFixed) {
                    // Has a default — keep boxed
                    func->paramTypes.push_back("");
                    continue;
                }
                // *args / **kwargs slots are never native
                if (pi < func->paramNames.size()) {
                    const auto& pn = func->paramNames[pi];
                    if (!pn.empty() && pn[0] == '*') {
                        func->paramTypes.push_back("");
                        continue;
                    }
                }

                std::string dominant = "";
                bool consistent = true;
                bool allPositionsFilled = true;

                for (const auto& sig : allSigs) {
                    if (pi >= sig.size()) {
                        allPositionsFilled = false;
                        break;
                    }
                    std::string t = sig[pi];
                    if (t == "i64") t = "int";
                    if (t == "int" || t == "float") {
                        if (dominant.empty()) dominant = t;
                        else if (dominant != t) { consistent = false; break; }
                    } else {
                        consistent = false;
                        break;
                    }
                }

                if (!consistent || !allPositionsFilled) {
                    func->paramTypes.push_back("");
                    continue;
                }

                if (dominant == "int") {
                    func->paramTypes.push_back("int");
                } else if (dominant == "float") {
                    func->paramTypes.push_back("float");
                } else {
                    func->paramTypes.push_back("");
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
    // Debug info: current source line being lowered (from ASTNode::lineno).
    // Updated at the start of lower()/lowerExpr(). Injected into every
    // IRInstruction via the emit() wrapper.
    int currentLineno = 0;

    // Wrapper for ir.addInstruction that injects the current source line.
    // This avoids changing ~100 call sites — every lowering helper calls
    // emit() instead of ir.addInstruction() and gets line tracking for free.
    inline void emit(const std::string& op, const std::vector<std::string>& operands,
                     const std::string& result = "", const std::string& resultType = "boxed") {
        ir.addInstruction(currentFunc, op, operands, result, resultType, currentLineno);
    }
    // Current innermost loop labels — updated by lowerFor/lowerWhile so
    // break/continue target the right blocks even with nested loops.
    std::string loopContinueLabel;
    std::string loopBreakLabel;
    std::unordered_map<std::string, int> funcDefaultCount;
    std::unordered_map<std::string, std::vector<std::string>> funcDefaultValues;
    std::unordered_map<std::string, std::vector<std::string>> funcParamNames;
    // Python-visible def name for TypeError messages (f, not __nesteddef_0).
    std::unordered_map<std::string, std::string> funcDisplayNames;
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
    // Local names bound via `from datetime import date/datetime/timedelta [as X]`,
    // mapping the local name to which constructor it refers to ("date"/
    // "datetime"/"timedelta"). `datetime.date(...)`-qualified calls are
    // recognized structurally in lowerMethodCall regardless of this map;
    // this map exists only to make the equally-common bare-name form
    // (`date(...)` after a from-import) route to the same construction
    // path in lowerCall, since that's a plain Name-callee Call node.
    std::unordered_map<std::string, std::string> datetimeCtorAliases;
    // Local name -> original module name for every `import X [as Y]`
    // (Y -> X; identity for `import X` with no asname). Lets datetime's
    // `X.date(...)`/`X.date.today()` dispatch recognize `import datetime
    // as dt` the same way it already recognizes the unaliased `datetime`
    // name.
    std::unordered_map<std::string, std::string> moduleNameAliases;
    // Local names bound via `from pathlib import Path [as X]` — same
    // rationale as datetimeCtorAliases, but a plain set since Path has
    // only one constructor (no date/datetime/timedelta-style variants).
    std::unordered_set<std::string> pathCtorAliases;
    // Local names bound via `from hashlib import md5/sha1/sha256 [as X]`
    // — same rationale as datetimeCtorAliases, mapping to which hash
    // algorithm ("md5"/"sha1"/"sha256").
    std::unordered_map<std::string, std::string> hashlibCtorAliases;
    // Local names bound via `from copy import copy/deepcopy [as X]` —
    // same rationale as hashlibCtorAliases, mapping to which free
    // function ("copy"/"deepcopy"). Needed because `copy.copy(...)`'s
    // qualified form can't rely on typeOf(obj)!="dict" the way
    // os.path.join's fix did (the copy module's own dict is itself
    // typed "dict" — see PyCopy_Copy's comment in Runtime.cpp).
    std::unordered_map<std::string, std::string> copyFuncAliases;
    // Local names bound via `from csv import writer [as X]` — same
    // rationale as pathCtorAliases. csv.writer(f) needs AST-structural
    // construction (see PyCsv_Writer's comment in Runtime.cpp) so
    // .writerow() can be dispatched with an explicit receiver.
    std::unordered_set<std::string> csvWriterCtorAliases;
    // Local names bound via `from itertools import groupby [as X]` —
    // same rationale as csvWriterCtorAliases: groupby's key= keyword
    // argument needs AST-level extraction, so it's not a normal dict
    // entry and needs this alias tracking for the bare-name form too.
    std::unordered_set<std::string> groupbyCtorAliases;
    // Local names bound via `from collections import deque [as X]` — same
    // rationale as pathCtorAliases: deque(...) needs AST-structural
    // construction so its result can carry the compile-time "deque"
    // typeOf tag (see PyCollections_Deque's comment in Runtime.cpp).
    std::unordered_set<std::string> dequeCtorAliases;
    // Local names bound via `from decimal import Decimal [as X]` — same
    // rationale as dequeCtorAliases: Decimal(...) needs AST-structural
    // construction so its result can carry the compile-time "decimal"
    // typeOf tag (see PyDecimal_Construct's comment in Runtime.cpp).
    std::unordered_set<std::string> decimalCtorAliases;
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
      // P0: dict name/temp → structured layout of each value (nbody body shape).
      std::unordered_map<std::string, std::vector<std::string>> dictValueLayouts;
      // P0: list name/temp → layout of each element when homogeneous structured.
      std::unordered_map<std::string, std::vector<std::string>> structuredElementLayout;
      // P0: list name/temp → body layout L meaning List[(L,L)] (nbody PAIRS).
      std::unordered_map<std::string, std::vector<std::string>> pairOfStructuredLayout;
      // P0: temp that is one pair-of-bodies item → body layout for each child.
      std::unordered_map<std::string, std::vector<std::string>> childStructuredLayout;

      // Apply a tuple/body layout onto a value name in the current function.
      void applyTupleLayout(const std::string& name, const std::vector<std::string>& layout) {
          if (name.empty() || layout.empty()) return;
          for (auto& fn : ir.functions) {
              if (fn.name != currentFunc) continue;
              fn.listElementTypes[name] = layout;
              for (size_t i = 0; i < layout.size(); ++i) {
                  const std::string& et = layout[i];
                  fn.containerElementTypes[name][i] = et;
                  if (et == "list_float" || et == "float_list") {
                      // nested: don't put scalar float at this index in subscript map
                  } else if (et == "list_int" || et == "int_list") {
                  } else if (et == "float" || et == "int") {
                      fn.subscriptElementTypes[name][i] = et;
                  }
              }
              break;
          }
      }

      // Mark name as a homogeneous list whose elements have the given tuple layout.
      void markStructuredList(const std::string& name, const std::vector<std::string>& layout) {
          if (name.empty() || layout.empty()) return;
          structuredElementLayout[name] = layout;
          for (auto& fn : ir.functions) {
              if (fn.name != currentFunc) continue;
              fn.structuredElementLayout[name] = layout;
              break;
          }
          noteType(name, "list");
      }

      void markPairOfStructured(const std::string& name, const std::vector<std::string>& bodyLayout) {
          if (name.empty() || bodyLayout.empty()) return;
          pairOfStructuredLayout[name] = bodyLayout;
          for (auto& fn : ir.functions) {
              if (fn.name != currentFunc) continue;
              fn.pairOfStructuredLayout[name] = bodyLayout;
              break;
          }
          noteType(name, "list");
      }

      // Look up a proven per-index element type for `name[idx]` in the current
      // function. Empty if unknown. list_float / float_list mean the *element*
      // is a float list, not a scalar float.
      std::string provenSubscriptElemType(const std::string& name, size_t idx) const {
          if (name.empty()) return "";
          for (const auto& fn : ir.functions) {
              if (fn.name != currentFunc) continue;
              auto sit = fn.subscriptElementTypes.find(name);
              if (sit != fn.subscriptElementTypes.end()) {
                  auto iit = sit->second.find(idx);
                  if (iit != sit->second.end()) return iit->second;
              }
              break;
          }
          return "";
      }

      // After GetItemObj → list-comp loop var, copy the iterable's element
      // maps onto the target. The loop var is a list_float only when every
      // iterable element is itself a float list.
      void propagateCompLoopVarTypes(const std::string& iterName, const std::string& target) {
          if (iterName.empty() || target.empty()) return;
          copyLayoutMaps(iterName, target);
          if (structuredElementLayout.count(iterName)) {
              applyTupleLayout(target, structuredElementLayout[iterName]);
              return;
          }
          for (auto& fn : ir.functions) {
              if (fn.name != currentFunc) continue;
              auto sel = fn.structuredElementLayout.find(iterName);
              if (sel != fn.structuredElementLayout.end() && !sel->second.empty()) {
                  applyTupleLayout(target, sel->second);
                  break;
              }
              auto markIfHomogeneousLists = [&](const std::unordered_map<size_t, std::string>& m) -> bool {
                  if (m.empty()) return false;
                  bool allFloatList = true, allIntList = true;
                  for (const auto& [idx, et] : m) {
                      (void)idx;
                      if (et != "list_float" && et != "float_list") allFloatList = false;
                      if (et != "list_int" && et != "int_list") allIntList = false;
                  }
                  if (allFloatList) {
                      noteType(target, "list_float");
                      knownFloatLists.insert(target);
                      for (size_t i = 0; i <= 20; i++) fn.subscriptElementTypes[target][i] = "float";
                      return true;
                  }
                  if (allIntList) {
                      noteType(target, "list_int");
                      knownIntLists.insert(target);
                      for (size_t i = 0; i <= 20; i++) fn.subscriptElementTypes[target][i] = "int";
                      return true;
                  }
                  return false;
              };
              auto cit = fn.containerElementTypes.find(iterName);
              if (cit != fn.containerElementTypes.end() && markIfHomogeneousLists(cit->second))
                  break;
              auto sit = fn.subscriptElementTypes.find(iterName);
              if (sit != fn.subscriptElementTypes.end() && markIfHomogeneousLists(sit->second))
                  break;
              break;
          }
      }

      // Copy layout maps from src → dst (assign / call result propagation).
      void copyLayoutMaps(const std::string& src, const std::string& dst) {
          if (src.empty() || dst.empty() || src == dst) return;
          if (structuredElementLayout.count(src)) {
              structuredElementLayout[dst] = structuredElementLayout[src];
          }
          if (pairOfStructuredLayout.count(src)) {
              pairOfStructuredLayout[dst] = pairOfStructuredLayout[src];
          }
          if (childStructuredLayout.count(src)) {
              childStructuredLayout[dst] = childStructuredLayout[src];
          }
          if (dictValueLayouts.count(src)) {
              dictValueLayouts[dst] = dictValueLayouts[src];
          }
          for (auto& fn : ir.functions) {
              if (fn.name != currentFunc) continue;
              auto lit = fn.listElementTypes.find(src);
              if (lit != fn.listElementTypes.end()) fn.listElementTypes[dst] = lit->second;
              auto sit = fn.subscriptElementTypes.find(src);
              if (sit != fn.subscriptElementTypes.end()) fn.subscriptElementTypes[dst] = sit->second;
              auto cit = fn.containerElementTypes.find(src);
              if (cit != fn.containerElementTypes.end()) fn.containerElementTypes[dst] = cit->second;
              auto sel = fn.structuredElementLayout.find(src);
              if (sel != fn.structuredElementLayout.end()) fn.structuredElementLayout[dst] = sel->second;
              auto pel = fn.pairOfStructuredLayout.find(src);
              if (pel != fn.pairOfStructuredLayout.end()) fn.pairOfStructuredLayout[dst] = pel->second;
              break;
          }
      }
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
        // Build the CPython-style qualified name for repr: if this is a
        // nested def, the display name is "outer.<locals>.inner". For
        // top-level functions, it's just the function name.
        std::string qualName = displayName;
        if (!funcQualNameStack.empty()) {
            // The last element is this function's own name (just pushed).
            // Build "enclosing.<locals>.name" from the stack.
            if (funcQualNameStack.size() > 1) {
                std::string chain;
                for (size_t i = 0; i < funcQualNameStack.size(); ++i) {
                    if (i > 0) chain += ".<locals>.";
                    chain += funcQualNameStack[i];
                }
                qualName = chain;
            }
            // If size == 1, it's a top-level function; use displayName as-is.
        }
        // Dedicated temp namespace: emitFuncValue also runs right after a
        // FunctionDef restores tempCounter, and consuming shared-counter
        // numbers there collides with temp-name-keyed compile-time maps
        // (bundleTemps / callableTokenToSynthetic) populated during the
        // function body's lowering.
        int n = fvCounter++;
        std::string tok = "cfv" + std::to_string(n);
        ir.addInstruction(currentFunc, "const", {"\"" + irName + "\""}, tok, "str");
        std::string disp = "cfv" + std::to_string(n) + "d";
        ir.addInstruction(currentFunc, "const", {"\"" + qualName + "\""}, disp, "str");
        std::string res = "tfv" + std::to_string(n);
        ir.addInstruction(currentFunc, "call", {"pyc_make_func", tok, disp}, res);
        return res;
    }
    int fvCounter = 0;
    // Stack of Python-level function names for qualified repr
    // (outer.<locals>.inner). Pushed/popped in lower() for FunctionDef.
    std::vector<std::string> funcQualNameStack;

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
        // Handle tuple type
        if (type == "tuple") {
            // No special handling needed - just record the type
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
        if (t == "tuple") return "tuple";
        return t;
    }

    // Dispatch-chain whitelist predicates (see IMPLEMENTATION.md,
    // "Method dispatch"). A method arm may only take a compile-time fast
    // path when the receiver's type is *proven*. Anything unproven --
    // "boxed", which is every function parameter -- must fall through to
    // the chain's terminal fallback, where Pyc_CallBuiltinMethod
    // dispatches on the real runtime type tag.
    //
    // Deliberately do NOT accept "boxed" here. That is what makes the
    // fall-through happen, and it is the whole point: an arm that
    // accepts "boxed" is guessing, and a wrong guess is a silently wrong
    // answer rather than a slower-but-correct one.
    bool isProvenStr(const std::string& v) const { return typeOf(v) == "str"; }

    bool isProvenListLike(const std::string& v) const {
        const std::string t = typeOf(v);
        return t == "list" || t == "list_int" || t == "list_float" ||
               t == "deque" || t == "tuple" || t == "match_list" ||
               t == "list_values_typed";
    }

    // ---- Builtin method table (dispatch chain, step 5) ----
    //
    // The if/else chain in lowerMethodCall dispatches on method *name*,
    // and an arm with no receiver-type guard fires for whatever shows up.
    // That produced confidently wrong answers, not just Nones: `.copy()`
    // on a list arriving as a function parameter skipped the typed list
    // arm, fell into the name-only *dict* arm below it, and returned an
    // empty dict. `.clear()` on a list was a silent no-op for the same
    // reason.
    //
    // These rows replace that guesswork for every method whose emission
    // is a plain `call Fn obj [arg0] [arg1]`. Dispatch is keyed on
    // (name, proven receiver type), so a row can only fire for the type
    // it was written for; anything unproven matches nothing here and
    // falls through to the chain's terminal fallback, where
    // Pyc_CallBuiltinMethod dispatches on the real runtime tag.
    //
    // Methods needing more than a direct call -- split/rsplit (maxsplit
    // and kwargs), replace (count), find/rfind (start/end), sort
    // (key=/reverse=), dict.pop/get (default), dict.values (element-type
    // propagation), dict.fromkeys (no self argument), Counter.update --
    // deliberately stay as chain arms. They are guarded individually; see
    // IMPLEMENTATION.md.
    enum class RecvKind { Str, ListLike, Dict, Set, Bytes, Int };

    struct BuiltinMethodRow {
        const char* name;
        RecvKind recv;
        int argc;                // trailing args after obj (missing -> "")
        const char* fn;
        const char* irType;      // IR result type; "" when untyped
        const char* noteAs;      // typeOf() label; "" to leave unnoted.
                                 // Distinct from irType on purpose: the set
                                 // operations note "set" while emitting no
                                 // IR type, and conflating the two would
                                 // silently change codegen's type metadata.
    };

    static const std::vector<BuiltinMethodRow>& builtinMethodRows() {
        static const std::vector<BuiltinMethodRow> rows = {
            // str, no arguments
            {"upper",      RecvKind::Str, 0, "PyString_Upper",      "", ""},
            {"lower",      RecvKind::Str, 0, "PyString_Lower",      "", ""},
            {"strip",      RecvKind::Str, 0, "PyString_Strip",      "", ""},
            {"lstrip",     RecvKind::Str, 0, "PyString_LStrip",     "", ""},
            {"rstrip",     RecvKind::Str, 0, "PyString_RStrip",     "", ""},
            {"casefold",   RecvKind::Str, 0, "PyString_Casefold",   "", ""},
            {"capitalize", RecvKind::Str, 0, "PyString_Capitalize", "", ""},
            {"swapcase",   RecvKind::Str, 0, "PyString_Swapcase",   "", ""},
            {"splitlines", RecvKind::Str, 0, "PyString_Splitlines", "", ""},
            {"title",      RecvKind::Str, 0, "PyString_Title",      "", ""},
            {"isalpha",    RecvKind::Str, 0, "PyString_IsAlpha",    "bool", "bool"},
            {"isdigit",    RecvKind::Str, 0, "PyString_IsDigit",    "bool", "bool"},
            {"isalnum",    RecvKind::Str, 0, "PyString_IsAlnum",    "bool", "bool"},
            {"islower",    RecvKind::Str, 0, "PyString_IsLower",    "bool", "bool"},
            {"isupper",    RecvKind::Str, 0, "PyString_IsUpper",    "bool", "bool"},
            {"isspace",    RecvKind::Str, 0, "PyString_IsSpace",    "bool", "bool"},
            // str, one argument
            {"startswith", RecvKind::Str, 1, "PyString_StartsWith", "bool", "bool"},
            {"endswith",   RecvKind::Str, 1, "PyString_EndsWith",   "bool", "bool"},
            {"zfill",      RecvKind::Str, 1, "PyString_ZFill",      "", ""},
            {"partition",  RecvKind::Str, 1, "PyString_Partition",  "", ""},
            {"rpartition", RecvKind::Str, 1, "PyString_RPartition", "", ""},
            {"rindex",     RecvKind::Str, 1, "PyString_RIndex",     "int", "int"},
            // str, two arguments
            {"center",     RecvKind::Str, 2, "PyString_Center",     "", ""},
            {"ljust",      RecvKind::Str, 2, "PyString_LJust",      "", ""},
            {"rjust",      RecvKind::Str, 2, "PyString_RJust",      "", ""},
            // count/index: two rows replace a `(isProvenStr(obj)) ? ... : ...`
            // ternary. Keying on the receiver is exactly what the table is
            // for, and the unproven case now falls through instead of
            // defaulting to the list implementation (which returned 0 for
            // "banana".count("a") through a parameter).
            {"count",      RecvKind::Str,      1, "PyString_Count", "int", "int"},
            {"count",      RecvKind::ListLike, 1, "PyList_Count",   "int", "int"},
            {"index",      RecvKind::Str,      1, "PyString_Index", "int", "int"},
            {"index",      RecvKind::ListLike, 1, "PyList_Index",   "int", "int"},
            {"join",       RecvKind::Str,      1, "PyString_Join",  "", ""},
            {"remove",     RecvKind::ListLike, 1, "PyList_Remove",  "", ""},
            // set: every operation is a plain direct call
            {"add",        RecvKind::Set, 1, "PySet_Add",       "", ""},
            {"remove",     RecvKind::Set, 1, "PySet_Remove",    "", ""},
            {"discard",    RecvKind::Set, 1, "PySet_Discard",   "", ""},
            {"pop",        RecvKind::Set, 0, "PySet_Pop",       "", ""},
            {"clear",      RecvKind::Set, 0, "PySet_Clear",     "", ""},
            {"copy",       RecvKind::Set, 0, "PySet_Copy",      "", "set"},
            {"update",     RecvKind::Set, 1, "PySet_Update",    "", ""},
            {"union",        RecvKind::Set, 1, "PySet_Union",        "", "set"},
            {"intersection", RecvKind::Set, 1, "PySet_Intersection", "", "set"},
            {"difference",   RecvKind::Set, 1, "PySet_Difference",   "", "set"},
            {"symmetric_difference", RecvKind::Set, 1,
                                          "PySet_SymmetricDifference", "", "set"},
            {"issubset",   RecvKind::Set, 1, "PySet_IsSubsetObj",   "bool", "bool"},
            {"issuperset", RecvKind::Set, 1, "PySet_IsSupersetObj", "bool", "bool"},
            // bytes/bytearray: content lives in the same `str` field as a
            // real str, so the PyString_* helpers apply unchanged. These
            // rows exist because b"x".upper() used to be served by the
            // name-only `upper` arm; whitelisting that arm to proven str
            // dropped bytes on the floor (caught by the bytes test case).
            {"upper",      RecvKind::Bytes, 0, "PyString_Upper", "", ""},
            {"lower",      RecvKind::Bytes, 0, "PyString_Lower", "", ""},
            // list
            {"append",     RecvKind::ListLike, 1, "PyList_Append",  "", ""},
            {"insert",     RecvKind::ListLike, 2, "PyList_Insert",  "", ""},
            {"extend",     RecvKind::ListLike, 1, "PyList_Extend",  "", ""},
            {"reverse",    RecvKind::ListLike, 0, "PyList_Reverse", "", ""},
            {"copy",       RecvKind::ListLike, 0, "PyList_Copy",    "", ""},
            {"clear",      RecvKind::ListLike, 0, "PyList_Clear",   "", ""},
            // dict
            {"keys",       RecvKind::Dict, 0, "PyDict_Keys",       "list", "list"},
            {"items",      RecvKind::Dict, 0, "PyDict_Items",      "list", "list"},
            {"copy",       RecvKind::Dict, 0, "PyDict_Copy",       "", ""},
            {"clear",      RecvKind::Dict, 0, "PyDict_Clear",      "", ""},
            {"popitem",    RecvKind::Dict, 0, "PyDict_PopItem",    "", ""},
            {"setdefault", RecvKind::Dict, 2, "PyDict_SetDefault", "", ""},
            // int/bool. Proven only — do not accept "boxed" (that would
            // steal user-class .bit_length() through a parameter).
            {"bit_length", RecvKind::Int, 0, "PyInt_BitLength", "int", "int"},
        };
        return rows;
    }

    // Structural guarantee this table buys over the chain: a duplicate
    // (name, receiver kind) is a programming error that used to be
    // invisible -- the second arm would simply never run. Here it is a
    // loop over data, checked once, and it aborts rather than silently
    // preferring whichever row came first.
    static void validateBuiltinMethodRows() {
        std::set<std::pair<std::string, int>> seen;
        for (const auto& r : builtinMethodRows()) {
            auto key = std::make_pair(std::string(r.name), (int)r.recv);
            if (!seen.insert(key).second) {
                std::fprintf(stderr,
                    "pyc internal error: duplicate builtin method row '%s' "
                    "for receiver kind %d\n", r.name, (int)r.recv);
                std::abort();
            }
        }
    }

    bool receiverMatches(RecvKind k, const std::string& v) const {
        switch (k) {
            case RecvKind::Str:      return isProvenStr(v);
            case RecvKind::ListLike: return isProvenListLike(v);
            case RecvKind::Dict:     return typeOf(v) == "dict";
            case RecvKind::Set:      return typeOf(v) == "set";
            case RecvKind::Bytes:    return typeOf(v) == "bytes" ||
                                            typeOf(v) == "bytearray";
            case RecvKind::Int:      return typeOf(v) == "int" ||
                                            typeOf(v) == "bool";
        }
        return false;
    }

    // Returns true when a row handled the call and emitted its IR.
    bool tryBuiltinMethodTable(const std::string& methodName,
                               const std::string& obj,
                               const std::vector<std::string>& args,
                               const std::string& res) {
        static const bool validated = (validateBuiltinMethodRows(), true);
        (void)validated;
        for (const auto& r : builtinMethodRows()) {
            if (methodName != r.name || !receiverMatches(r.recv, obj)) continue;
            std::vector<std::string> call{r.fn, obj};
            for (int i = 0; i < r.argc; ++i) {
                call.push_back((size_t)i < args.size() ? args[i] : "");
            }
            if (r.irType[0]) {
                ir.addInstruction(currentFunc, "call", call, res, r.irType);
            } else {
                ir.addInstruction(currentFunc, "call", call, res);
            }
            if (r.noteAs[0]) noteType(res, r.noteAs);
            return true;
        }
        return false;
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
            // float ** anything or anything ** float → float (covers ** -1.5)
            if (lt == "float" || rt == "float") return "float";
            auto isNum = [](const std::string& t){ return t=="int" || t=="bool" || t=="i64"; };
            if (isNum(lt) && isNum(rt)) return "boxed"; // int**int via runtime / expand
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
            // Infer only from proven container types. Name[const] is not
            // float by default — that promoted ints and collapsed lists/strs.
            if (node->children.size() >= 2) {
                const ASTNode* objNode = node->children[0].get();
                const ASTNode* idxNode = node->children[1].get();
                if (objNode && objNode->type == "Subscript") {
                    std::string innerType = detectCompElementType(objNode);
                    if (innerType == "float" || innerType == "int") {
                        return innerType;
                    }
                }
                if (objNode && objNode->type == "Name" && idxNode && idxNode->type == "Constant") {
                    const std::string& name = objNode->id;
                    std::string objT = typeOf(name);
                    if (objT == "list_float" || knownFloatLists.count(name))
                        return "float";
                    if (objT == "list_int" || knownIntLists.count(name))
                        return "int";
                    std::string idxValue;
                    if (!idxNode->value.empty())
                        idxValue = idxNode->value;
                    else if (idxNode->args.size() == 1)
                        idxValue = idxNode->args[0];
                    size_t idxVal = 0;
                    bool hasLiteralIndex = !idxValue.empty()
                        && !idxNode->is_float && !idxNode->is_str
                        && !idxNode->is_none && !idxNode->is_bytes;
                    if (hasLiteralIndex) {
                        try {
                            if (!idxValue.empty() && idxValue[0] == '-')
                                hasLiteralIndex = false;
                            else
                                idxVal = std::stoull(idxValue);
                        } catch (...) {
                            hasLiteralIndex = false;
                        }
                    }
                    if (hasLiteralIndex) {
                        std::string et = provenSubscriptElemType(name, idxVal);
                        if (et == "float") return "float";
                        if (et == "int" || et == "i64" || et == "bool") return "int";
                    }
                    return "boxed";
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
        // Nested ListComp is always a list, never a scalar. Inheriting
        // the inner elt type made [[1 for _ in [0]] for __ in [0]]
        // allocate PyList_NewIntBoxed and store the inner list as 0.
        if (node->type == "ListComp") return "boxed";
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
                std::string res = "$t" + std::to_string(tempCounter++);
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
                    bool isComplex = complexVars.count(left) > 0;
                    // Don't constant-fold x ** 0 to 1 when x is a boxed
                    // value of unknown type — it could be complex, and
                    // complex ** 0 = (1+0j) in CPython, not 1. Let it
                    // fall through to the runtime Pyc_Pow which handles
                    // complex correctly.
                    std::string leftType = typeOf(left);
                    bool leftIsBoxed = (leftType == "boxed" || leftType.empty());
                    if (expv == 0) {
                        if (isComplex) {
                            std::string res = "$t" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "call", {"PyComplex_New", "1", "0"}, res);
                            complexVars.insert(res);
                            noteType(res, "boxed");
                            return res;
                        }
                        if (leftIsBoxed) {
                            // Unknown type — could be complex. Fall through
                            // to runtime Pyc_Pow for correct handling.
                        } else {
                            std::string one = "$c" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "const", {"1"}, one, "int");
                            noteType(one, "int");
                            return one;
                        }
                    } else {
                    std::string cur = left;
                    for (long k = 1; k < expv; ++k) {
                        std::string t = "$t" + std::to_string(tempCounter++);
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
                std::string res = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {funcName, left, right}, res);
                complexVars.insert(res);
                noteType(res, "boxed");
                return res;
            }
        }
        // B16: Complex pow — if op is pow and operands are complex, emit PyComplex_Pow
        if (op == "pow") {
            if (complexVars.count(left) > 0 && complexVars.count(right) > 0) {
                std::string res = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyComplex_Pow", left, right}, res);
                complexVars.insert(res);
                noteType(res, "boxed");
                return res;
            }
        }
        std::string res = "$t" + std::to_string(tempCounter++);
        std::string resultType = numericResultType(op, left, right);
        ir.addInstruction(currentFunc, op, {left, right}, res, resultType);
        noteType(res, resultType);
        // pathlib.Path "/" joining and date/datetime/timedelta arithmetic:
        // the runtime dispatch (PyNumber_TrueDivide/Add/Subtract, gated on
        // the runtime type tag) already produces the correct *value*
        // regardless of compiler tracking — this additionally tags the
        // *result temp* so a chained method call in the same expression
        // (e.g. `(base / "x").is_dir()`, or `(d + delta).isoformat()`)
        // can dispatch without the value needing to round-trip through an
        // explicit variable first. Overrides the generic noteType above.
        if (op == "truediv" && typeOf(left) == "path") {
            noteType(res, "path");
        } else if (op == "add" || op == "sub") {
            std::string lt = typeOf(left), rt = typeOf(right);
            bool lDate = (lt == "date" || lt == "datetime");
            bool rDate = (rt == "date" || rt == "datetime");
            if (op == "add") {
                if (lDate && rt == "timedelta") noteType(res, lt);
                else if (lt == "timedelta" && rDate) noteType(res, rt);
                else if (lt == "timedelta" && rt == "timedelta") noteType(res, "timedelta");
            } else {
                if (lDate && rt == "timedelta") noteType(res, lt);
                else if (lDate && rDate && lt == rt) noteType(res, "timedelta");
                else if (lt == "timedelta" && rt == "timedelta") noteType(res, "timedelta");
            }
        } else if (op == "mul" && (typeOf(left) == "timedelta" || typeOf(right) == "timedelta")) {
            noteType(res, "timedelta");
        }
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
            std::string ck = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(k)}, ck);
            std::string el = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", listVal, ck}, el);
            fwd.push_back(el);
        }
        std::string rest;
        if (hasVar) {
            // Collect [fixed .. n) into a fresh list for the * slot.
            std::string ln = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", listVal}, ln);
            std::string lnSlot = "__sl_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {ln}, lnSlot);
            std::string startC = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(fixed)}, startC);
            std::string zero = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, zero);
            rest = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, rest);
            std::string jv = "$s" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {startC}, jv);
            int sc = tempCounter++;
            std::string slp = "vf_lp_" + std::to_string(sc);
            std::string sbd = "vf_bd_" + std::to_string(sc);
            std::string sex = "vf_ex_" + std::to_string(sc);
            ir.addInstruction(currentFunc, "br", {}, slp);
            ir.addInstruction(currentFunc, "label", {}, slp);
            std::string cm = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", jv, lnSlot}, cm);
            ir.addInstruction(currentFunc, "br", {cm, sbd, sex});
            ir.addInstruction(currentFunc, "label", {}, sbd);
            std::string el = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", listVal, jv}, el);
            std::string d = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_Append", rest, el}, d);
            std::string one = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, one);
            std::string nj = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "add", {jv, one}, nj);
            ir.addInstruction(currentFunc, "assign", {nj}, jv);
            ir.addInstruction(currentFunc, "br", {}, slp);
            ir.addInstruction(currentFunc, "label", {}, sex);
        }
        if (hasVar) fwd.push_back(rest);
        std::string callRes = resultTemp.empty() ? ("$t" + std::to_string(tempCounter++)) : resultTemp;
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
        std::string callRes = "$t" + std::to_string(tempCounter++);
        emitForwardCallFromList(target, vaParam, callRes);
        ir.addInstruction(wrapper, "ret", {callRes});

        currentFunc = savedFunc;
        tempCounter = savedTemp;
    }

    // Shared by both `datetime.date(...)`-qualified construction
    // (lowerMethodCall) and bare `date(...)` construction after a
    // from-import (lowerCall, via datetimeCtorAliases). `posArgs` must
    // already be lowered positional args (Keyword children excluded);
    // keyword args are read directly from `node` by name.
    std::string lowerDatetimeConstruct(const std::string& which, const ASTNode* node,
                                        const std::vector<std::string>& posArgs) {
        auto kwArg = [&](const char* name) -> std::string {
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type == "Keyword" && ch->id == name && !ch->children.empty()) {
                    return lowerExpr(ch->children[0].get());
                }
            }
            return "";
        };
        auto posOrKw = [&](size_t posIdx, const char* kwName) -> std::string {
            std::string kw = kwArg(kwName);
            if (!kw.empty()) return kw;
            return posIdx < posArgs.size() ? posArgs[posIdx] : "";
        };
        std::string res = "$t" + std::to_string(tempCounter++);
        if (which == "date") {
            ir.addInstruction(currentFunc, "call",
                {"PyDateTime_Date", posOrKw(0, "year"), posOrKw(1, "month"), posOrKw(2, "day")}, res);
            noteType(res, "date");
        } else if (which == "datetime") {
            ir.addInstruction(currentFunc, "call",
                {"PyDateTime_Datetime", posOrKw(0, "year"), posOrKw(1, "month"), posOrKw(2, "day"),
                 posOrKw(3, "hour"), posOrKw(4, "minute"), posOrKw(5, "second")}, res);
            noteType(res, "datetime");
        } else { // "timedelta"
            ir.addInstruction(currentFunc, "call",
                {"PyTimedelta_New", posOrKw(0, "days"), posOrKw(1, "seconds"),
                 kwArg("minutes"), kwArg("hours"), kwArg("weeks")}, res);
            noteType(res, "timedelta");
        }
        return res;
    }

    // Shared by both `pathlib.Path(...)`-qualified construction
    // (lowerMethodCall) and bare `Path(...)` construction after a
    // from-import (lowerCall, via pathCtorAliases). Single positional or
    // keyword `path=`/first-arg; anything else is dropped (real
    // pathlib.Path also accepts multiple path segments to join, e.g.
    // `Path("a", "b")` — not supported here, single-argument only).
    std::string lowerPathConstruct(const ASTNode* node, const std::vector<std::string>& posArgs) {
        std::string arg;
        for (size_t i = 1; i < node->children.size(); ++i) {
            const auto* ch = node->children[i].get();
            if (ch && ch->type == "Keyword" && ch->id == "path" && !ch->children.empty()) {
                arg = lowerExpr(ch->children[0].get());
                break;
            }
        }
        if (arg.empty() && !posArgs.empty()) arg = posArgs[0];
        std::string res = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyPathlib_Path", arg}, res);
        noteType(res, "path");
        return res;
    }

    // Shared by both `hashlib.md5(...)`-qualified construction
    // (lowerMethodCall) and bare `md5(...)` construction after a
    // from-import (lowerCall, via hashlibCtorAliases). `which` is
    // "md5"/"sha1"/"sha256"; single positional/keyword `data=` argument
    // (matches real hashlib usage — data is always known upfront, no
    // .update() streaming support).
    std::string lowerHashlibConstruct(const std::string& which, const ASTNode* node,
                                       const std::vector<std::string>& posArgs) {
        std::string arg;
        for (size_t i = 1; i < node->children.size(); ++i) {
            const auto* ch = node->children[i].get();
            if (ch && ch->type == "Keyword" && ch->id == "data" && !ch->children.empty()) {
                arg = lowerExpr(ch->children[0].get());
                break;
            }
        }
        if (arg.empty() && !posArgs.empty()) arg = posArgs[0];
        std::string fn = which == "md5" ? "PyHashlib_Md5" : which == "sha1" ? "PyHashlib_Sha1" : "PyHashlib_Sha256";
        std::string res = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {fn, arg}, res);
        noteType(res, "hashobj");
        return res;
    }

    // Shared by both `collections.deque(...)`-qualified construction
    // (lowerMethodCall) and bare `deque(...)` construction after a
    // from-import (lowerCall, via dequeCtorAliases). Single optional
    // positional iterable argument (matches real `deque(iterable=[])`);
    // noteType(res, "deque") drives the typeOf-gated .appendleft()/
    // .popleft()/.rotate() dispatch below.
    std::string lowerDequeConstruct(const std::vector<std::string>& posArgs) {
        std::string arg = posArgs.empty() ? "" : posArgs[0];
        std::string res = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyCollections_Deque", arg}, res, "list");
        noteType(res, "deque");
        return res;
    }

    // Shared by both `decimal.Decimal(...)`-qualified construction
    // (lowerMethodCall) and bare `Decimal(...)` construction after a
    // from-import (lowerCall, via decimalCtorAliases). Single positional
    // argument (str/int/float/another Decimal — PyDecimal_Construct
    // dispatches on the argument's runtime type). AST-structural
    // recognition needed (like hashlib.md5/pathlib.Path) so the result
    // can carry the "decimal" noteType tag, gating .quantize()'s dispatch.
    std::string lowerDecimalConstruct(const std::vector<std::string>& posArgs) {
        std::string arg = posArgs.empty() ? "" : posArgs[0];
        std::string res = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyDecimal_Construct", arg}, res);
        noteType(res, "decimal");
        return res;
    }

    // Emit Pyc_CheckMissingArgs(func, required_names, dicts).
    // `dicts` empty ⇒ every name in `required` is treated as missing.
    void emitMissingArgsCheck(const std::string& displayName,
                              const std::vector<std::string>& required,
                              const std::vector<std::string>& dictTemps) {
        if (required.empty()) return;
        std::string namesList = "$t" + std::to_string(tempCounter++);
        std::string zNames = "$c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, zNames, "int");
        ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zNames}, namesList, "boxed");
        for (const auto& nm : required) {
            std::string sc = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + nm + "\""}, sc, "str");
            std::string d = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_Append", namesList, sc}, d);
        }
        std::string dictsList = "$t" + std::to_string(tempCounter++);
        std::string zDicts = "$c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, zDicts, "int");
        ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zDicts}, dictsList, "boxed");
        for (const auto& dv : dictTemps) {
            std::string d = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_Append", dictsList, dv}, d);
        }
        std::string fnConst = "$c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"\"" + displayName + "\""}, fnConst, "str");
        ir.addInstruction(currentFunc, "call", {"Pyc_CheckMissingArgs", fnConst, namesList, dictsList}, "");
    }

    std::string callDisplayName(const std::string& funcName) const {
        auto it = funcDisplayNames.find(funcName);
        if (it != funcDisplayNames.end() && !it->second.empty()) return it->second;
        return funcName;
    }

    std::string lowerCall(const ASTNode* node) {
        // Method call: obj.method(args) — func is an Attribute node
         if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Attribute") {
            return lowerMethodCall(node);
        }
        // datetime.date/datetime/timedelta bound to a bare name via
        // `from datetime import date` (etc.) — construct directly, same
        // as the `datetime.date(...)`-qualified form in lowerMethodCall.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            datetimeCtorAliases.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::vector<std::string> posArgs;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") posArgs.push_back(lowerExpr(ch));
            }
            return lowerDatetimeConstruct(datetimeCtorAliases[node->children[0]->id], node, posArgs);
        }
        // pathlib.Path bound to a bare name via `from pathlib import Path`
        // — construct directly, same as the `pathlib.Path(...)`-qualified
        // form in lowerMethodCall.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            pathCtorAliases.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::vector<std::string> posArgs;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") posArgs.push_back(lowerExpr(ch));
            }
            return lowerPathConstruct(node, posArgs);
        }
        // hashlib.md5/sha1/sha256 bound to a bare name via `from hashlib
        // import md5` (etc.) — construct directly, same as the
        // `hashlib.md5(...)`-qualified form in lowerMethodCall.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            hashlibCtorAliases.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::vector<std::string> posArgs;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") posArgs.push_back(lowerExpr(ch));
            }
            return lowerHashlibConstruct(hashlibCtorAliases[node->children[0]->id], node, posArgs);
        }
        // copy.copy/deepcopy bound to a bare name via `from copy import
        // copy`/`deepcopy` — call directly, same as the
        // `copy.copy(...)`-qualified form in lowerMethodCall.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            copyFuncAliases.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::string arg;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") { arg = lowerExpr(ch); break; }
            }
            const std::string& which = copyFuncAliases[node->children[0]->id];
            std::string fn = which == "copy" ? "PyCopy_Copy" : "PyCopy_Deepcopy";
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {fn, arg}, res);
            return res;
        }
        // csv.writer bound to a bare name via `from csv import writer`
        // — construct directly, same as the `csv.writer(...)`-qualified
        // form in lowerMethodCall.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            csvWriterCtorAliases.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::string arg;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") { arg = lowerExpr(ch); break; }
            }
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyCsv_Writer", arg}, res);
            noteType(res, "csvwriter");
            return res;
        }
        // groupby bound to a bare name via `from itertools import
        // groupby` — dispatch directly (with key= extraction), same as
        // the `itertools.groupby(...)`-qualified form in lowerMethodCall.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            groupbyCtorAliases.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::vector<std::string> posArgs;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") posArgs.push_back(lowerExpr(ch));
            }
            std::string iterableArg = posArgs.empty() ? "" : posArgs[0];
            std::string keyArg;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type == "Keyword" && ch->id == "key" && !ch->children.empty()) {
                    keyArg = lowerExpr(ch->children[0].get());
                    break;
                }
            }
            if (keyArg.empty() && posArgs.size() > 1) keyArg = posArgs[1];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyItertools_Groupby", iterableArg, keyArg}, res, "list");
            noteType(res, "list");
            return res;
        }
        // collections.deque bound to a bare name via `from collections
        // import deque` — construct directly, same as the
        // `collections.deque(...)`-qualified form in lowerMethodCall.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            dequeCtorAliases.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::vector<std::string> posArgs;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") posArgs.push_back(lowerExpr(ch));
            }
            return lowerDequeConstruct(posArgs);
        }
        // decimal.Decimal bound to a bare name via `from decimal import
        // Decimal` — construct directly, same as the
        // `decimal.Decimal(...)`-qualified form in lowerMethodCall.
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" &&
            decimalCtorAliases.count(node->children[0]->id) &&
            !isShadowedLocal(node->children[0]->id)) {
            std::vector<std::string> posArgs;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") posArgs.push_back(lowerExpr(ch));
            }
            return lowerDecimalConstruct(posArgs);
        }
        // super() call — returns a proxy that looks up methods on the parent class
        if (!node->children.empty() && node->children[0] &&
            node->children[0]->type == "Name" && node->children[0]->id == "super") {
            std::string superProxy = "$t" + std::to_string(tempCounter++);
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
            std::string nameConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + node->children[0]->id + "\""}, nameConst, "str");
            std::string msg;
            for (size_t i = 1; i < node->children.size(); ++i) {
                if (node->children[i] && node->children[i]->type != "Keyword") {
                    msg = lowerExpr(node->children[i].get());
                    break;
                }
            }
            if (msg.empty()) {
                msg = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"\""}, msg, "str");
            }
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"pyc_make_exc", nameConst, msg}, res);
            return res;
        }
    // Class instantiation: ClassName(args) — create instance dict and call __init__
        std::string funcName;
        if (!node->children.empty() && node->children[0] && node->children[0]->type == "Name") {
            funcName = node->children[0]->id;
            auto classIt = knownClasses.find(funcName);
            if (classIt != knownClasses.end()) {
                std::string instanceDict = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyDict_New"}, instanceDict);
                // Store class reference on instance for method lookup
                std::string classKeyConst = "$c" + std::to_string(tempCounter++);
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
                        // No __init__ found anywhere in the direct base
                        // chain — found and fixed while bug hunting: this
                        // used to fall straight to the bare-["self"]
                        // fallback below, then still forward every
                        // positional argument the caller actually wrote
                        // regardless of that declared 0-arg arity — an
                        // argument-count mismatch LLVM's verifier rejects
                        // outright, crashing compilation of the *entire
                        // file* for the ordinary `class MyError(Exception):
                        // pass` idiom whenever it was instantiated with an
                        // argument (`raise MyError("boom")`).
                        //
                        // A class transitively deriving from a builtin
                        // exception name (Exception, ValueError, ...) has
                        // no real __init__ to call anyway; its actual
                        // behavior is "store the constructor's positional
                        // args as self.args", matching CPython's own
                        // BaseException.__init__(self, *args). Handle that
                        // directly here instead of synthesizing a broken
                        // forwarding call, and skip __init__ entirely.
                        // This also makes the instance recognizable to the
                        // exception machinery (pyc_raise / except-matching
                        // / str(e)), which reads the same "args" key and
                        // the class's __mro__ — see
                        // pyc_exc_type_name/pyc_exc_matches/pyc_exc_message
                        // in Runtime.cpp. A class with its own explicit
                        // __init__ (even one that calls
                        // super().__init__(...)) is unaffected by this
                        // branch — that's the hasOwnInitDefined path
                        // above, a separate, narrower, still-unsupported
                        // case (documented in IMPLEMENTATION.md).
                        bool isExcSubclass = false;
                        auto mroIt = classMRO.find(funcName);
                        if (mroIt != classMRO.end()) {
                            for (const auto& m : mroIt->second) {
                                if (builtinExcNames().count(m)) { isExcSubclass = true; break; }
                            }
                        }
                        if (isExcSubclass) {
                            std::vector<std::string> excUserArgs;
                            for (size_t i = 1; i < node->children.size(); ++i) {
                                if (node->children[i] && node->children[i]->type != "Keyword") {
                                    excUserArgs.push_back(lowerExpr(node->children[i].get()));
                                }
                            }
                            std::string argsList = "$t" + std::to_string(tempCounter++);
                            std::string z = "$c" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "const", {"0"}, z);
                            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, argsList);
                            for (const auto& a : excUserArgs) {
                                std::string dummyAppend = "$t" + std::to_string(tempCounter++);
                                ir.addInstruction(currentFunc, "call", {"PyList_Append", argsList, a}, dummyAppend);
                            }
                            std::string argsKeyConst = "$c" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "const", {"\"args\""}, argsKeyConst, "str");
                            std::string dummySet = "$t" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", instanceDict, argsKeyConst, argsList}, dummySet);
                            return instanceDict;
                        }
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
                // __default_<initName>_<i> in the order they appear. The total
                // number of params (incl. self) minus userArgs minus 1 (self)
                // gives the number of trailing defaults to inject.
                size_t totalParams = initParams.size();
                size_t provided = userArgs.size();
                size_t trailing = (totalParams > provided + 1) ? (totalParams - provided - 1) : 0;
                // Look up the defaults registered for the underlying __init__.
                // funcDefaultValues stores them in order; the *last* N are the
                // trailing defaults (consistent with Python: defaults apply to
                // the last N parameters). Keyed per-class by initName (see the
                // fix note in the class-lowering code above) rather than the
                // shared literal "__init__", which used to collide across
                // classes.
                auto dit = funcDefaultValues.find(initName);
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
                std::string initCallRes = "$t" + std::to_string(tempCounter++);
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
            "bool", "type", "id", "repr", "hex", "oct", "bin", "ord", "chr", "round",
            "bytes", "bytearray", "tuple", "divmod", "pow", "set", "callable",
            "map", "filter", "format", "ascii",
            "getattr", "hasattr", "setattr", "delattr", "issubclass", "iter", "next"
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
                    "repr","hex","oct","bin","ord","chr","round","open","bytes","bytearray",
                    "tuple","divmod","pow","callable",
                    "map","filter","format","ascii",
                    "getattr","hasattr","setattr","delattr","issubclass","iter","next"
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
                        std::string cellVal = "$t" + std::to_string(tempCounter++);
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
        // Skip for special builtins (callable, len, etc.) whose bare-name reference
        // now produces a callable token but must still go through their direct lowering.
        if (!isIndirectCallee && !calleeValEarly.empty() &&
            (callableTokenTemps.count(calleeValEarly) || callableTokenToSynthetic.count(calleeValEarly)) &&
            !useDynamicApply) {
            // Only apply if the callee is NOT a known special builtin name.
            std::string calleeName = (node->children[0] && node->children[0]->type == "Name")
                                   ? node->children[0]->id : "";
            static const std::unordered_set<std::string> neverIndirect = {
                "print","len","range","min","max","sum","sorted","any","all","isinstance",
                "int","float","complex","abs","str","list","enumerate","zip","bool","type","id",
                "repr","hex","oct","bin","ord","chr","round","open","bytes","bytearray",
                "tuple","divmod","pow","callable","set","map","filter","format","ascii",
                "getattr","hasattr","setattr","delattr","issubclass","iter","next"
            };
            if (neverIndirect.count(calleeName) == 0) {
                useDynamicApply = true;
                tokenTempForApply = calleeValEarly;
                isIndirectCallee = true;
            }
        }

        // For indirect callees (lambdas-as-values via tokens), we build the argument
        // list for Pyc_Apply directly here so that Starred dynamic * can splice into it
        // without routing through a __va wrapper (which requires a static target name).
        std::string indirectArgListTemp; // if non-empty, this list is passed to Pyc_Apply
        bool buildingIndirectArgs = false;
        if (isIndirectCallee) {
            std::string z = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, z);
            indirectArgListTemp = "$t" + std::to_string(tempCounter++);
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
                                std::string d = "$t" + std::to_string(tempCounter++);
                                ir.addInstruction(currentFunc, "call", {"PyList_Append", indirectArgListTemp, p}, d);
                            }
                        }
                        std::string ln = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", lst}, ln);
                        std::string lnSlotI = "__sl_" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "assign", {ln}, lnSlotI);
                        std::string jv = "$s" + std::to_string(tempCounter++);
                        std::string j0 = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"0"}, j0);
                        ir.addInstruction(currentFunc, "assign", {j0}, jv);
                        int sc = tempCounter++;
                        std::string slp = "istar_lp_" + std::to_string(sc);
                        std::string sbd = "istar_bd_" + std::to_string(sc);
                        std::string sex = "istar_ex_" + std::to_string(sc);
                        ir.addInstruction(currentFunc, "br", {}, slp);
                        ir.addInstruction(currentFunc, "label", {}, slp);
                        std::string cm = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "icmp", {"Lt", jv, lnSlotI}, cm);
                        ir.addInstruction(currentFunc, "br", {cm, sbd, sex});
                        ir.addInstruction(currentFunc, "label", {}, sbd);
                        std::string el = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", lst, jv}, el);
                        std::string dmy = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", indirectArgListTemp, el}, dmy);
                        std::string one = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"1"}, one);
                        std::string nj = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "add", {jv, one}, nj);
                        ir.addInstruction(currentFunc, "assign", {nj}, jv);
                        ir.addInstruction(currentFunc, "br", {}, slp);
                        ir.addInstruction(currentFunc, "label", {}, sex);
                        // Nothing is pushed to argRes; the indirect Pyc_Apply path will use indirectArgListTemp.
                    } else {
                        // Direct target: original dynamic * path using a __va_<target> wrapper.
                        std::string ln = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", lst}, ln);
                        std::string lnSlotD = "__sl_" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "assign", {ln}, lnSlotD);
                        std::string jv = "$s" + std::to_string(tempCounter++);
                        std::string j0 = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"0"}, j0);
                        ir.addInstruction(currentFunc, "assign", {j0}, jv);
                        int sc = tempCounter++;
                        std::string slp = "star_lp_" + std::to_string(sc);
                        std::string sbd = "star_bd_" + std::to_string(sc);
                        std::string sex = "star_ex_" + std::to_string(sc);
                        // Seed va list with fixed prefix so far (positionals before the *)
                        std::string va = "$t" + std::to_string(tempCounter++);
                        std::string pn = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {std::to_string(argRes.size())}, pn);
                        ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", pn}, va);
                        for (auto& p : argRes) {
                            if (!p.empty()) {
                                std::string d = "$t" + std::to_string(tempCounter++);
                                ir.addInstruction(currentFunc, "call", {"PyList_Append", va, p}, d);
                            }
                        }
                        ir.addInstruction(currentFunc, "br", {}, slp);
                        ir.addInstruction(currentFunc, "label", {}, slp);
                        std::string cm = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "icmp", {"Lt", jv, lnSlotD}, cm);
                        ir.addInstruction(currentFunc, "br", {cm, sbd, sex});
                        ir.addInstruction(currentFunc, "label", {}, sbd);
                        std::string el = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", lst, jv}, el);
                        std::string dmy = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", va, el}, dmy);
                        std::string one = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"1"}, one);
                        std::string nj = "$t" + std::to_string(tempCounter++);
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
                    std::string d = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", indirectArgListTemp, v}, d);
                } else {
                    argRes.push_back(lowerExpr(node->children[i].get()));
                }
            }
        }
        // Callee-side *args/**kwargs collection (skip for runtime * call
        // sites; the __va wrapper already forwards the correct
        // fixed+tail shape for the target).
        // vidx = index of a *args param; kwidx = index of a **kwargs
        // param. Found and fixed while bug hunting: this loop used to
        // stop at the first star-prefixed name (matching **kwargs too,
        // since it also starts with '*'), so a **kwargs catch-all
        // parameter never got a value collected for it at all — the
        // callee's IR function has a real parameter slot for it
        // regardless, so the call ended up one argument short whenever
        // the caller passed zero keyword arguments (crashing exactly
        // like the *args-only case did before that was separately
        // fixed). kwidx is captured here (outer scope) so the keyword-
        // argument handling below can populate the slot.
        size_t kwidx = (size_t)-1;
        if (!hadRuntimeStar) {
            auto pit = funcParamNames.find(funcName);
            if (pit != funcParamNames.end()) {
                const auto& params = pit->second;
                size_t vidx = (size_t)-1;
                for (size_t j = 0; j < params.size(); ++j) {
                    if (params[j].size() >= 2 && params[j][0] == '*' && params[j][1] == '*') {
                        kwidx = j;
                    } else if (!params[j].empty() && params[j][0] == '*') {
                        if (vidx == (size_t)-1) vidx = j;
                    }
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
                    std::string z = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, z);
                    collected = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, collected);
                    for (auto& t : tail) {
                        std::string d = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", collected, t}, d);
                    }
                    if (argRes.size() < fixed) argRes.resize(fixed, "");
                    argRes.push_back(collected);
                }
                // **kwargs catch-all: unconditionally give this slot a
                // real (empty, for now) dict here — the keyword-argument
                // handling below replaces it with a populated dict only
                // when the call actually has unmatched keyword
                // arguments, but every call to this callee needs
                // *something* real in this position regardless, since
                // the declared IR function always has this parameter.
                if (kwidx != (size_t)-1) {
                    std::string emptyDict = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyDict_New"}, emptyDict);
                    if (argRes.size() <= kwidx) argRes.resize(kwidx + 1, "");
                    argRes[kwidx] = emptyDict;
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
                // First, apply regular keyword arguments. Any keyword
                // that doesn't name a regular parameter is collected for
                // the **kwargs catch-all below (found and fixed while
                // bug hunting: this used to be silently dropped
                // entirely — a **kwargs parameter always bound an empty,
                // wrongly-typed list rather than a dict populated with
                // exactly these unmatched keyword arguments, matching
                // CPython's own semantics).
                std::vector<std::pair<std::string, std::string>> unmatchedKwArgs;
                for (auto& kw : kwArgs) {
                    bool matched = false;
                    for (size_t j = 0; j < params.size(); ++j) {
                        if (params[j] == kw.first) {
                            if (argRes.size() <= j) argRes.resize(j + 1);
                            argRes[j] = kw.second;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched && kwidx != (size_t)-1) unmatchedKwArgs.push_back(kw);
                }
                if (kwidx != (size_t)-1 && !unmatchedKwArgs.empty()) {
                    std::string kwDict = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyDict_New"}, kwDict);
                    for (auto& kw : unmatchedKwArgs) {
                        std::string keyConst = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + kw.first + "\""}, keyConst, "str");
                        std::string dummySet = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", kwDict, keyConst, kw.second}, dummySet);
                    }
                    // A slot for kwidx already exists (unconditionally
                    // created as an empty dict above); overwrite it with
                    // the populated one.
                    argRes[kwidx] = kwDict;
                }
                // Then, expand **kwargs dicts: one Pyc_DictGetOrDefault
                // call per regular parameter, each with the exact right
                // fallback for that position — an already-bound value
                // (a positional argument, or a key=value keyword argument
                // matched above), else that parameter's registered
                // default, else boxed None. Found and fixed while bug
                // hunting: the previous design (Pyc_ExpandKwargsList,
                // looking up every parameter name in the dict and
                // unconditionally overwriting argRes for all of them)
                // had two separate correctness bugs, both rooted in that
                // same unconditional overwrite — see Pyc_DictGetOrDefault's
                // comment in Runtime.cpp for the exact confirmed repros
                // (a defaulted parameter omitted from the spread dict got
                // None instead of its default; a positional argument got
                // silently clobbered with None whenever the spread dict
                // didn't also happen to supply that same parameter name).
                //
                // Skip kwidx (a **kwargs catch-all isn't a regular named
                // parameter a spread dict's keys could ever match — see
                // the populated-dict logic above) and any *args slot
                // (already correctly populated by the *args collection
                // earlier in this function; a dict spread's entries don't
                // map onto it). Routing a **dict spread's own unmatched
                // entries into a **kwargs catch-all is a further,
                // separate, still-open gap — not attempted here.
                auto ddit0 = funcDefaultValues.find(funcName);
                size_t nregular0 = params.size();
                for (size_t k = 0; k < params.size(); ++k) {
                    if (!params[k].empty() && params[k][0] == '*') { nregular0 = k; break; }
                }
                size_t ndefaults0 = (ddit0 != funcDefaultValues.end()) ? ddit0->second.size() : 0;
                if (!kwargDicts.empty() && !buildingIndirectArgs) {
                    std::vector<std::string> reqUnbound;
                    for (size_t j = 0; j < nregular0; ++j) {
                        bool bound = j < argRes.size() && !argRes[j].empty();
                        bool hasDef = ndefaults0 > 0 && j >= nregular0 - ndefaults0;
                        if (!bound && !hasDef) reqUnbound.push_back(params[j]);
                    }
                    if (!reqUnbound.empty()) {
                        emitMissingArgsCheck(callDisplayName(funcName), reqUnbound, kwargDicts);
                    }
                }
                for (auto& dictVal : kwargDicts) {
                    auto ddit = funcDefaultValues.find(funcName);
                    size_t nregular = params.size();
                    for (size_t k = 0; k < params.size(); ++k) {
                        if (!params[k].empty() && params[k][0] == '*') { nregular = k; break; }
                    }
                    size_t ndefaults = (ddit != funcDefaultValues.end()) ? ddit->second.size() : 0;
                    for (size_t j = 0; j < params.size(); ++j) {
                        if (j == kwidx) continue;
                        if (!params[j].empty() && params[j][0] == '*') continue;
                        std::string fallback;
                        if (j < argRes.size() && !argRes[j].empty()) {
                            fallback = argRes[j];
                        } else if (j < nregular && ndefaults > 0 && j >= nregular - ndefaults) {
                            fallback = ddit->second[j - (nregular - ndefaults)];
                        }
                        if (fallback.empty()) {
                            fallback = "$c" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "nconst", {}, fallback, "none");
                        }
                        std::string paramConst = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + params[j] + "\""}, paramConst, "str");
                        std::string elem = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"Pyc_DictGetOrDefault", dictVal, paramConst, fallback}, elem);
                        if (argRes.size() <= j) argRes.resize(j + 1);
                        argRes[j] = elem;
                    }
                }
                // If the callee has a **kwargs parameter, route unmatched
                // keys from the spread dict(s) into it. This fixes the gap
                // where `def f(**kwargs): ...` called as `f(**{"p":1,"q":2})`
                // left kwargs empty (unmatched keys were silently dropped).
                if (kwidx != (size_t)-1 && !kwargDicts.empty() && kwidx < argRes.size() && !argRes[kwidx].empty()) {
                    // Build a list of parameter names for the runtime helper.
                    // Create an empty list using PyList_NewBoxed (takes boxed int size).
                    std::string paramList = "$t" + std::to_string(tempCounter++);
                    std::string sizeConst = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, sizeConst, "int");
                    ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sizeConst}, paramList, "boxed");
                    for (size_t j = 0; j < params.size(); ++j) {
                        if (!params[j].empty() && params[j][0] == '*') break; // stop at *args
                        std::string paramName = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + params[j] + "\""}, paramName, "str");
                        std::string dummyAppend = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", paramList, paramName}, dummyAppend);
                    }
                    // Call Pyc_RouteSpreadKwargs(spread_dict, param_names_list, kwargs_dict)
                    // for each spread dict.
                    for (auto& dictVal : kwargDicts) {
                        std::string dummyRoute = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"Pyc_RouteSpreadKwargs", dictVal, paramList, argRes[kwidx]}, dummyRoute);
                    }
                }
            } else if (buildingIndirectArgs) {
                // Indirect callee (unknown shape at compile time — a
                // variable holding a function, a closure/decorator
                // forwarding *args/**kwargs, a value pulled from a
                // container). Real bug found and fixed while bug hunting:
                // keyword arguments to an indirect call used to be
                // silently dropped entirely — pushed onto `argRes`, which
                // isn't even the list actually used for indirect calls
                // (see buildingIndirectArgs above; indirect args are
                // appended directly to indirectArgListTemp as they're
                // processed, so anything pushed onto argRes here was dead
                // code). Fixed by packing keyword args + dict spreads into
                // a single merged dict, appended as the LAST element of
                // the flat Pyc_Apply argument list. The generated
                // __apply__<name> adapter (Codegen.cpp) recognizes a
                // trailing dict argument and, if the target has a
                // **kwargs catch-all, binds it there instead of always
                // synthesizing an empty placeholder (which, before this
                // fix, was itself also the wrong type — a list, not a
                // dict). An ordinary positional-only indirect call (no
                // keyword args at this call site) is unaffected: nothing
                // is appended, matching prior behavior exactly.
                if (!kwArgs.empty() || !kwargDicts.empty()) {
                    std::string kwDict = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyDict_New"}, kwDict);
                    for (auto& dictVal : kwargDicts) {
                        std::string dummyUpd = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyDict_Update", kwDict, dictVal}, dummyUpd);
                    }
                    for (auto& kw : kwArgs) {
                        std::string keyConst = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {"\"" + kw.first + "\""}, keyConst, "str");
                        std::string dummySet = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", kwDict, keyConst, kw.second}, dummySet);
                    }
                    std::string dAppend = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", indirectArgListTemp, kwDict}, dAppend);
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
            // Required regular param slots still empty after default
            // injection: raise TypeError (do not emit a short LLVM call).
            // Skip indirect Pyc_Apply sites — argRes is not the apply list
            // (the adapter does this check).
            if (!buildingIndirectArgs) {
                auto pit = funcParamNames.find(funcName);
                if (pit != funcParamNames.end()) {
                    const auto& params = pit->second;
                    size_t nregular = params.size();
                    for (size_t j = 0; j < params.size(); ++j) {
                        if (!params[j].empty() && params[j][0] == '*') { nregular = j; break; }
                    }
                    size_t ndefaults = 0;
                    auto dit = funcDefaultValues.find(funcName);
                    if (dit != funcDefaultValues.end()) ndefaults = dit->second.size();
                    size_t nrequired = (nregular > ndefaults) ? (nregular - ndefaults) : 0;
                    std::vector<std::string> missing;
                    for (size_t j = 0; j < nrequired; ++j) {
                        if (j >= argRes.size() || argRes[j].empty())
                            missing.push_back(params[j]);
                    }
                    if (!missing.empty()) {
                        emitMissingArgsCheck(callDisplayName(funcName), missing, {});
                        if (argRes.size() < nregular) argRes.resize(nregular, "");
                        for (size_t j = 0; j < nregular; ++j) {
                            if (argRes[j].empty()) {
                                std::string none = "$c" + std::to_string(tempCounter++);
                                ir.addInstruction(currentFunc, "nconst", {}, none, "none");
                                argRes[j] = none;
                            }
                        }
                    }
                }
            }
        }

        // print() with no positional args and no kwargs → bare newline.
        if (funcName == "print" && argRes.empty() && kwArgs.empty()) {
            std::string res = "$t" + std::to_string(tempCounter++);
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
            std::string sizeConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
            std::string argList = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sizeConst}, argList);
            for (size_t i = 0; i < n; ++i) {
                std::string idx = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idx);
                std::string dummy = "$t" + std::to_string(tempCounter++);
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
                sepArg = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "nconst", {}, sepArg, "none");
            }
            std::string endArg;
            if (endGiven) {
                endArg = endVal;
            } else {
                endArg = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "nconst", {}, endArg, "none");
            }
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"pyc_print", argList, sepArg, endArg}, res);
            return res;
        }

        // len(obj) → PyBuiltin_Len(obj)
        if (funcName == "len") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Open", path, mode}, res);
            // "file", not "dict" — see the typeOf(obj)=="file" branch in
            // lowerMethodCall for why plain "dict" typing broke .write().
            noteType(res, "file");
            return res;
        }

        // min/max — fold pairwise; single list arg uses list variant
        if (funcName == "min" || funcName == "max") {
            std::string fn2  = (funcName == "min") ? "PyBuiltin_Min2"    : "PyBuiltin_Max2";
            std::string fnLst = (funcName == "min") ? "PyBuiltin_MinList" : "PyBuiltin_MaxList";
            // key= — found completely unsupported (silently ignored;
            // min/max have no funcParamNames entry, so the generic
            // kwarg-append fallback stuffed key's value onto the end of
            // argRes where it was then misread as a second positional
            // value to compare against, e.g. min([3,1,2], key=lambda
            // x: -x) printed the lambda itself) while hunting for more
            // bugs. Use posArgCount, not argRes.size(), to find the true
            // positional args (argRes may have kwarg values appended).
            std::string keyName;
            std::string defaultName;
            for (const auto& kv : kwArgs) {
                if (kv.first == "key") keyName = kv.second;
                if (kv.first == "default") defaultName = kv.second;
            }
            std::vector<std::string> posArgs(argRes.begin(),
                argRes.begin() + std::min(posArgCount, argRes.size()));
            if (posArgs.size() == 1) {
                std::string res = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {fnLst, posArgs[0], keyName, defaultName}, res);
                noteType(res, "boxed");
                return res;
            }
            std::string acc = posArgs[0];
            for (size_t i = 1; i < posArgs.size(); ++i) {
                std::string res2 = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {fn2, acc, posArgs[i], keyName}, res2);
                noteType(res2, "boxed");
                acc = res2;
            }
            noteType(acc, "boxed");
            return acc;
        }
        // list(x) → PyBuiltin_List(x)
        if (funcName == "list") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            if (argRes.empty()) {
                std::string sc = "$c" + std::to_string(tempCounter++);
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
            // P0: list(structured) inherits structured element layout
            copyLayoutMaps(arg, res);
            if (structuredElementLayout.count(arg))
                markStructuredList(res, structuredElementLayout[arg]);
            return res;
        }
        // tuple(x) -> PyBuiltin_Tuple(x). Now that pyc has a real tuple
        // type (type 7), tuple() returns a real tuple, matching CPython.
        if (funcName == "tuple") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            if (argRes.empty()) {
                std::string sc = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, sc, "int");
                ir.addInstruction(currentFunc, "call", {"PyTuple_NewBoxed", sc}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Tuple", arg}, res);
            }
            noteType(res, "tuple");
            return res;
        }
        // set() / set(iterable) construction.
        if (funcName == "set") {
            std::string res = "$t" + std::to_string(tempCounter++);
            if (argRes.empty()) {
                ir.addInstruction(currentFunc, "call", {"PySet_New"}, res);
            } else {
                std::string arg = argRes[0];
                ir.addInstruction(currentFunc, "call", {"PySet_New"}, res);
                std::string dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PySet_Update", res, arg}, dummy);
            }
            noteType(res, "set");
            return res;
        }
        // bytes(...)/bytearray(...) construction. arg0 (value) and arg1
        // (encoding, only meaningful when arg0 is a str) are both
        // optional — missing ones pass through as empty operands, which
        // Codegen's getAsPyObject resolves to a null PyObject* (the same
        // "optional PyObject* argument" convention re's flags= extraction
        // uses).
        if (funcName == "bytes" || funcName == "bytearray") {
            std::string arg0 = argRes.size() > 0 ? argRes[0] : "";
            std::string arg1 = argRes.size() > 1 ? argRes[1] : "";
            std::string fn = (funcName == "bytes") ? "PyBuiltin_Bytes" : "PyBuiltin_Bytearray";
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {fn, arg0, arg1}, res);
            noteType(res, funcName == "bytes" ? "bytes" : "bytearray");
            return res;
        }
        // reversed(seq) → PyBuiltin_Reversed(seq)
        if (funcName == "reversed") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
            // Check for start= keyword argument (enumerate(iterable, start=N))
            std::string startName;
            for (const auto& kv : kwArgs) {
                if (kv.first == "start") startName = kv.second;
            }
            // start can also be a second positional argument
            if (startName.empty() && posArgCount >= 2 && argRes.size() >= 2) {
                startName = argRes[1];
            }
            if (!startName.empty()) {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Enumerate2", arg, startName}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Enumerate", arg}, res);
            }
            noteType(res, "list");
            return res;
        }
        // zip(a, b) → PyBuiltin_Zip2(a, b)
        if (funcName == "zip" && argRes.size() >= 2) {
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Zip2", argRes[0], argRes[1]}, res);
            noteType(res, "list");
            // S3: zip returns tuples, always boxed
            return res;
        }

        // sum(iterable) → PyBuiltin_Sum
        if (funcName == "sum") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            // Check for start= keyword argument (sum(iterable, start))
            std::string startName;
            for (const auto& kv : kwArgs) {
                if (kv.first == "start") startName = kv.second;
            }
            // start can also be a second positional argument: sum(iterable, start)
            if (startName.empty() && posArgCount >= 2 && argRes.size() >= 2) {
                startName = argRes[1];
            }
            if (!startName.empty()) {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Sum2", arg, startName}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Sum", arg}, res);
            }
            noteType(res, "boxed");
            return res;
        }
        // cmp_to_key(cmp) → PyBuiltin_CmpToKey(cmp)
        // Returns a dict token that sorted recognizes for the fast-path.
        if (funcName == "cmp_to_key" && argRes.size() >= 1) {
            std::string res = "$t" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
            // Find the key argument (positional or keyword).
            std::string keyName;
            // reverse= — found completely missing (silently ignored,
            // sorted(x, reverse=True) returned the plain ascending sort)
            // while hunting for more bugs; PyBuiltin_Sorted now takes a
            // 3rd reverse-flag argument.
            std::string reverseName;
            for (const auto& kv : kwArgs) {
                if (kv.first == "key") keyName = kv.second;
                else if (kv.first == "reverse") reverseName = kv.second;
            }
            // Use posArgCount (captured before the kwArgs-append fallback
            // runs for builtins with no funcParamNames entry) rather than
            // argRes.size(): sorted() has no registered params, so any
            // kwarg (e.g. reverse=True) gets blindly appended to argRes by
            // that fallback, and argRes.size()>=2 would then wrongly treat
            // reverse's value as a positional key function.
            if (keyName.empty() && posArgCount >= 2) keyName = argRes[1];
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
                // reverse= isn't threaded through the cmp_to_key path —
                // a narrower, documented remaining gap (this combination
                // is rare enough not to have been hit by this bug hunt;
                // the far more common sorted(x, reverse=True) and
                // sorted(x, key=..., reverse=True) forms are fixed above).
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_SortedWithCmp", arg, cmpArg}, res);
            } else {
                std::string keyArg = keyName.empty() ? "" : keyName;
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Sorted", arg, keyArg, reverseName}, res);
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
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Any", arg}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // all(iterable) → PyBuiltin_All (bool result)
        if (funcName == "all") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_All", arg}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // map(func, iterable) → PyBuiltin_Map(func, iterable)
        // map(func, iter1, iter2, ...) → PyBuiltin_MapN(func, [iter1, iter2, ...])
        if (funcName == "map" && argRes.size() >= 2) {
            std::string res = "$t" + std::to_string(tempCounter++);
            if (argRes.size() == 2) {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Map", argRes[0], argRes[1]}, res);
            } else {
                // Build a list of iterables
                std::string zero = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, zero);
                std::string iterList = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, iterList);
                for (size_t i = 1; i < argRes.size(); ++i) {
                    std::string d = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", iterList, argRes[i]}, d);
                }
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_MapN", argRes[0], iterList}, res);
            }
            noteType(res, "list");
            return res;
        }
        // filter(func, iterable) → PyBuiltin_Filter(func, iterable)
        if (funcName == "filter" && argRes.size() >= 2) {
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Filter", argRes[0], argRes[1]}, res);
            noteType(res, "list");
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
                    else if (n == "bytes") typecode = 17;
                    else if (n == "bytearray") typecode = 18;
                    else if (n == "Decimal") typecode = 19;
                    else if (n == "set") typecode = 20;
                    else if (n == "tuple") typecode = 7;
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
            std::string res = "$t" + std::to_string(tempCounter++);
            if (typecode >= 0) {
                std::string tc = "$c" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Float", arg}, res, "float");
            noteType(res, "float");
            return res;
        }
        // complex(x) or complex(x, y) → PyBuiltin_Complex(x, y)
        if (funcName == "complex") {
            std::string res = "$t" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Ord", arg}, res, "int");
            noteType(res, "int");
            return res;
        }
        // chr(i) → PyBuiltin_Chr(i)
        if (funcName == "chr") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Chr", arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // bool(x) → PyBuiltin_Bool(x)
        if (funcName == "bool") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Bool", arg}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // type(x) → PyBuiltin_Type(x)  (returns a string like "<class 'int'>")
        if (funcName == "type") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Type", arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // callable(x) → PyBuiltin_Callable(x)  (returns True/False)
        if (funcName == "callable") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Callable", arg}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // hex/oct/bin(x) — string with 0x/0o/0b prefix
        if (funcName == "hex" || funcName == "oct" || funcName == "bin") {
            std::string helper = (funcName == "hex") ? "PyBuiltin_Hex"
                                : (funcName == "oct") ? "PyBuiltin_Oct"
                                : "PyBuiltin_Bin";
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {helper, arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // id(obj) → PyBuiltin_Id(obj)
        if (funcName == "id") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Id", arg}, res, "int");
            noteType(res, "int");
            return res;
        }
        // divmod(a, b) → PyBuiltin_Divmod(a, b) — returns a 2-element list
        if (funcName == "divmod") {
            std::string a = argRes.size() > 0 ? argRes[0] : "";
            std::string b = argRes.size() > 1 ? argRes[1] : "";
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Divmod", a, b}, res, "list");
            noteType(res, "list");
            return res;
        }
        // repr(obj) → PyBuiltin_Repr(obj) (returns a string)
        if (funcName == "repr") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Repr", arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // format(value[, spec]) → Pyc_FormatValue(value, spec)
        if (funcName == "format") {
            std::string val = argRes.empty() ? "" : argRes[0];
            std::string spec;
            if (argRes.size() > 1) {
                spec = argRes[1];
            } else {
                spec = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"\""}, spec, "str");
            }
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_FormatValue", val, spec}, res);
            noteType(res, "str");
            return res;
        }
        // ascii(obj) — like repr but escapes non-ASCII. pyc strings are ASCII
        // passthrough, so this is equivalent to repr for now.
        if (funcName == "ascii") {
            std::string arg = argRes.empty() ? "" : argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Repr", arg}, res, "str");
            noteType(res, "str");
            return res;
        }
        // round(x [, n]) → PyBuiltin_Round(x, n_or_null)
        if (funcName == "round") {
            std::string x = argRes.empty() ? "" : argRes[0];
            std::string n = argRes.size() > 1 ? argRes[1] : "";
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Round", x, n}, res);
            noteType(res, "boxed");
            return res;
        }
        // getattr(obj, name[, default]) → Pyc_GetAttr(obj, name)
        if (funcName == "getattr" && argRes.size() >= 2) {
            std::string obj = argRes[0];
            std::string name = argRes[1];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetAttr", obj, name}, res);
            // If a default was provided and the attribute is missing, the
            // runtime returns None — we don't have a real AttributeError
            // mechanism, so just return the result (which may be None).
            noteType(res, "boxed");
            return res;
        }
        // hasattr(obj, name) → check if Pyc_GetItem succeeds
        if (funcName == "hasattr" && argRes.size() >= 2) {
            std::string obj = argRes[0];
            std::string name = argRes[1];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_HasAttr", obj, name}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // setattr(obj, name, value) → Pyc_SetItem(obj, name, value)
        if (funcName == "setattr" && argRes.size() >= 3) {
            std::string obj = argRes[0];
            std::string name = argRes[1];
            std::string val = argRes[2];
            std::string dummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", obj, name, val}, dummy);
            noteType(dummy, "none");
            return dummy;
        }
        // delattr(obj, name) → Pyc_DelItem(obj, name)
        if (funcName == "delattr" && argRes.size() >= 2) {
            std::string obj = argRes[0];
            std::string name = argRes[1];
            std::string dummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_DelItem", obj, name}, dummy);
            noteType(dummy, "none");
            return dummy;
        }
        // issubclass(cls, parent) → check class hierarchy at runtime
        if (funcName == "issubclass" && argRes.size() >= 2) {
            std::string cls = argRes[0];
            std::string parent = argRes[1];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_IsSubclass", cls, parent}, res, "bool");
            noteType(res, "bool");
            return res;
        }
        // iter(obj) → returns an iterator for the iterable
        if (funcName == "iter" && argRes.size() >= 1) {
            std::string arg = argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_Iter", arg}, res);
            noteType(res, "boxed");
            return res;
        }
        // next(iter[, default]) → get next item from iterator
        if (funcName == "next" && argRes.size() >= 1) {
            std::string iter = argRes[0];
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_Next", iter}, res);
            noteType(res, "boxed");
            return res;
        }
        // pow(base, exp) → PyBuiltin_Pow(base, exp); pow(base, exp, mod)
        // → PyBuiltin_Pow3(base, exp, mod) — the 3-arg modular form was
        // found completely unimplemented (silently ignored the modulus)
        // while fixing the neverDynamic bug for 2-arg pow(); see
        // PyBuiltin_Pow3's comment in Runtime.cpp.
        if (funcName == "pow") {
            std::string a = argRes.size() > 0 ? argRes[0] : "";
            std::string b = argRes.size() > 1 ? argRes[1] : "";
            std::string res = "$t" + std::to_string(tempCounter++);
            if (argRes.size() > 2) {
                std::string m = argRes[2];
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Pow3", a, b, m}, res, "int");
                noteType(res, "int");
            } else {
                ir.addInstruction(currentFunc, "call", {"PyBuiltin_Pow", a, b}, res);
                noteType(res, "boxed");
            }
            return res;
        }

        // str(obj) → PyStr_FromAny(obj)
        if (funcName == "str") {
            if (argRes.empty()) {
                std::string res = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"\""}, res);
                noteType(res, "str");
                return res;
            }
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyStr_FromAny", argRes[0]}, res);
            noteType(res, "str");
            return res;
        }

        // Normalize range(stop), range(start,stop), range(start,stop,step)
        // → always call PyBuiltin_Range(start, stop, step) with 3 PyObject* args
        if (funcName == "range") {
            std::string startRes, stopRes, stepRes;
            if (argRes.size() == 1) {
                startRes = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, startRes);
                stopRes  = argRes[0];
                stepRes  = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"1"}, stepRes);
            } else if (argRes.size() == 2) {
                startRes = argRes[0];
                stopRes  = argRes[1];
                stepRes  = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"1"}, stepRes);
            } else if (argRes.size() >= 3) {
                startRes = argRes[0];
                stopRes  = argRes[1];
                stepRes  = argRes[2];
            } else {
                // range() with no args → empty list
                startRes = stopRes = stepRes = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, startRes);
            }
            std::string res = "$t" + std::to_string(tempCounter++);
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
            // Skip for plain Names already resolved to a known direct IR function:
            // lowering those would emit a dead pyc_make_func per call site (the
            // function-value temp is never used since the call dispatches directly).
            // This matches the early-skip at line ~3675.
            if (!(node->children[0]->type == "Name" && knownIRFunctions.count(funcName))) {
                calleeVal = lowerExpr(node->children[0].get());
            }
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
                    std::string bc = "$t" + std::to_string(tempCounter++);
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
                std::string bc = "$t" + std::to_string(tempCounter++);
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
                std::string z = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, z);
                argList = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, argList);
                if (!bundleCallee.empty()) {
                    auto dit = descriptorCells.find(bundleCallee);
                    if (dit != descriptorCells.end()) {
                        int k = 0;
                        for (const auto& nm : dit->second) {
                            // bundle[1+k] is the k-th cell (a PyCell* PyObject).
                            std::string ic = "$c" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "const", {std::to_string(1 + k)}, ic);
                            std::string cellObj = "$t" + std::to_string(tempCounter++);
                            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", bundleCallee, ic}, cellObj);
                            std::string d = "$t" + std::to_string(tempCounter++);
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
                    std::string sz = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", indirectArgListTemp}, sz);
                    std::string idx = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, idx);
                    std::string szc = "__ipc_sz_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {sz}, szc);
                    std::string jc = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, jc);
                    std::string jslot = "__ipc_j_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {jc}, jslot);
                    int sc = tempCounter++;
                    std::string lp = "__ipc_lp_" + std::to_string(sc);
                    std::string bd = "__ipc_bd_" + std::to_string(sc);
                    std::string ex = "__ipc_ex_" + std::to_string(sc);
                    ir.addInstruction(currentFunc, "br", {}, lp);
                    ir.addInstruction(currentFunc, "label", {}, lp);
                    std::string cm = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "icmp", {"Lt", jslot, szc}, cm);
                    ir.addInstruction(currentFunc, "br", {cm, bd, ex});
                    ir.addInstruction(currentFunc, "label", {}, bd);
                    std::string el = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", indirectArgListTemp, jslot}, el);
                    std::string d = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, el}, d);
                    std::string oneC = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"1"}, oneC);
                    std::string newJ = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "add", {jslot, oneC}, newJ, "int");
                    ir.addInstruction(currentFunc, "assign", {newJ}, jslot);
                    ir.addInstruction(currentFunc, "br", {}, lp);
                    ir.addInstruction(currentFunc, "label", {}, ex);
                } else {
                    for (auto& v : argRes) {
                        std::string d = "$t" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, v}, d);
                    }
                }
            }
            std::string tok = tokenTempForApply;
            // Bundle callee: extract the token from bundle[0] (a string).
            if (!bundleCallee.empty()) {
                std::string zc = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, zc);
                tok = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", bundleCallee, zc}, tok);
            } else if (tok.empty() && !funcName.empty()) {
                tok = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + funcName + "\""}, tok, "str");
            } else if (!tok.empty() && (namesThatMayHoldCallableTokens.count(tok) || callableTokenTemps.count(tok))) {
                // tok is a temp that may hold a callable token string. Get its value.
                // The value should be the callable token string itself.
                // We need to pass this temp directly to Pyc_Apply since it contains the token string.
                // No additional handling needed - the temp holds the string token.
            }
            std::string res = "$t" + std::to_string(tempCounter++);
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
                        std::string ic = "$c" + std::to_string(tempCounter++);
                        ir.addInstruction(currentFunc, "const", {std::to_string(1 + k)}, ic);
                        std::string cellObj = "$t" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", finalOps, res);
            if (isGenCall) {
                std::string genRes = "$t" + std::to_string(tempCounter++);
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
            // P0: combinations(structured_list) → list of pairs of that structure (nbody PAIRS)
            if ((funcName == "combinations" || funcName.find("combinations") != std::string::npos)
                && !argRes.empty()) {
                auto it = structuredElementLayout.find(argRes[0]);
                if (it != structuredElementLayout.end() && !it->second.empty()) {
                    markPairOfStructured(res, it->second);
                }
            }
            // S5: Propagate container element types from callee return type to call result.
            // If funcName returns a list with known element types, the call result inherits them.
            if (knownIRFunctions.count(funcName)) {
                for (auto& fn : ir.functions) {
                    if (fn.name == funcName) {
                        if (!fn.returnSubscriptElementTypes.empty() || !fn.returnContainerElementTypes.empty()) {
                            for (auto& fnx : ir.functions) {
                                if (fnx.name == currentFunc) {
                                    for (auto& [idx, et] : fn.returnSubscriptElementTypes) {
                                        fnx.subscriptElementTypes[res][idx] = et;
                                    }
                                    for (auto& [idx, et] : fn.returnContainerElementTypes) {
                                        fnx.containerElementTypes[res][idx] = et;
                                    }
                                    // Homogeneous scalar elements → typed list result
                                    if (!fn.returnSubscriptElementTypes.empty()) {
                                        bool allFloat = true, allInt = true;
                                        bool allFloatList = true, allIntList = true;
                                        for (const auto& [idx, et] : fn.returnSubscriptElementTypes) {
                                            if (et != "float") allFloat = false;
                                            if (et != "int" && et != "i64" && et != "bool") allInt = false;
                                            if (et != "list_float" && et != "float_list") allFloatList = false;
                                            if (et != "list_int" && et != "int_list") allIntList = false;
                                        }
                                        if (allFloat) {
                                            noteType(res, "list_float");
                                            knownFloatLists.insert(res);
                                        } else if (allInt) {
                                            noteType(res, "list_int");
                                            knownIntLists.insert(res);
                                        } else if (allFloatList || allIntList) {
                                            noteType(res, "list");
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        if (!fn.returnType.empty() && fn.returnType != "boxed") {
                            // Only set if we didn't already set a more specific list type
                            std::string cur = typeOf(res);
                            if (cur.empty() || cur == "boxed") noteType(res, fn.returnType);
                        }
                        break;
                    }
                }
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

        // B5: closure analysis for lambdas — compute free variables captured
        // from the enclosing scope. This runs before the body is lowered so
        // that isCellBackedHere returns true inside the lambda body. We scan
        // the lambda body for Name nodes that aren't locals/params, then
        // check which are assigned in the enclosing scope (locals of the
        // enclosing function). We can't use funcOwnedCells yet (it's populated
        // after body lowering), so we scan the enclosing function's AST for
        // assignments.
        std::vector<std::string> lamFrees;
        bool isNestedLambda = !savedFunc.empty() && savedFunc != "__module__";
        if (isNestedLambda) {
            // Collect names used in the lambda body (excluding nested defs/lambdas).
            std::unordered_set<std::string> used;
            std::function<void(const ASTNode*)> walk = [&](const ASTNode* n) {
                if (!n) return;
                if (n->type == "FunctionDef" || n->type == "Lambda") return;
                if (n->type == "Name" && !n->id.empty()) used.insert(n->id);
                for (const auto& c : n->children) walk(c.get());
            };
            for (const auto& c : node->children) {
                if (c && c->type != "Default") walk(c.get());
            }

            // Lambda's own locals (params only — lambdas have single-expression bodies).
            std::unordered_set<std::string> localsHere;
            for (auto& a : node->args) {
                std::string b = a;
                while (!b.empty() && b[0] == '*') b = b.substr(1);
                if (!b.empty()) localsHere.insert(b);
            }

            // Simplified approach: any name used in the lambda body that is:
            // 1. Not a lambda param/local
            // 2. Not a module global
            // 3. Not a known builtin
            // ...is a free variable captured from the enclosing scope.
            auto isModGlobal = [&](const std::string& nm) -> bool {
                for (const auto& g : ir.moduleGlobals) if (g == nm) return true;
                return false;
            };
            for (const auto& nm : used) {
                if (localsHere.count(nm)) continue;
                if (isModGlobal(nm)) continue;
                // Skip known builtins and IR function names
                if (knownIRFunctions.count(nm)) continue;
                if (nm == "print" || nm == "len" || nm == "range" || nm == "int" ||
                    nm == "float" || nm == "str" || nm == "list" || nm == "dict" ||
                    nm == "bool" || nm == "tuple" || nm == "set" || nm == "abs" ||
                    nm == "min" || nm == "max" || nm == "sum" || nm == "sorted" ||
                    nm == "enumerate" || nm == "zip" || nm == "reversed" ||
                    nm == "type" || nm == "isinstance" || nm == "callable" ||
                    nm == "round" || nm == "repr" || nm == "ord" || nm == "chr" ||
                    nm == "hex" || nm == "oct" || nm == "bin" || nm == "divmod" ||
                    nm == "pow" || nm == "any" || nm == "all" || nm == "id" ||
                    nm == "True" || nm == "False" || nm == "None") continue;
                if (std::find(lamFrees.begin(), lamFrees.end(), nm) == lamFrees.end())
                    lamFrees.push_back(nm);
            }
            funcFreeCells[lamName] = lamFrees;
        } else {
            funcFreeCells[lamName] = {};
        }

        // B5: if this lambda has free cells, synthesize hidden leading cell
        // parameters on the IRFunction (same as FunctionDef at lines 639-663).
        if (!lamFrees.empty()) {
            for (auto& fnr : ir.functions) if (fnr.name == lamName) {
                fnr.freeCellVars = lamFrees;
                std::vector<std::string> newArgs;
                for (const auto& fc : lamFrees) newArgs.push_back(fc + "_cell");
                newArgs.insert(newArgs.end(), fnr.args.begin(), fnr.args.end());
                fnr.args = newArgs;
                break;
            }
            closureFunctions.insert(lamName);
        }

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
                    std::string bx = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(lamName, "box_i64", {bodyVal}, bx, "int");
                    bodyVal = bx;
                } else if (rt == "float") {
                    std::string bx = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(lamName, "box_f64", {bodyVal}, bx, "float");
                    bodyVal = bx;
                }
                ir.addInstruction(lamName, "ret", {bodyVal});
                emittedRet = true;
                break;  // lambda has exactly one body expression
            }
        }
        if (!emittedRet) {
            std::string z = "$c" + std::to_string(tempCounter++);
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
            std::string r = "$t" + std::to_string(tempCounter++);
            if (opName == "In") {
                ir.addInstruction(currentFunc, "call", {"Pyc_Contains", rhs, lhs}, r);
            } else if (opName == "NotIn") {
                std::string c2 = "$t" + std::to_string(tempCounter++);
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
            std::string truth = "$t" + std::to_string(tempCounter++);
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
        std::string iterSrc = listVal;  // original name/temp before List()/slot (for layouts)
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
        std::string listRes = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", listVal}, listRes);
        if (iterElemType != "boxed") noteType(listRes, "match_list");
        // P0: layouts must survive List() + slot assign or pairs/bodies typing is lost
        copyLayoutMaps(iterSrc, listRes);
        if (structuredElementLayout.count(iterSrc))
            markStructuredList(listRes, structuredElementLayout[iterSrc]);
        if (pairOfStructuredLayout.count(iterSrc))
            markPairOfStructured(listRes, pairOfStructuredLayout[iterSrc]);
        listVal = listRes;
        // Store iterator in a slot so owned refs (e.g. sorted() result) are freed
        // at scope exit instead of leaking when emitDecRefIfOwnedSameBlock is blocked
        // inside the loop body (different block from the iterator definition).
        std::string iterSlot = "__iter_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "assign", {listVal}, iterSlot);
        copyLayoutMaps(listVal, iterSlot);
        if (structuredElementLayout.count(listVal))
            markStructuredList(iterSlot, structuredElementLayout[listVal]);
        if (pairOfStructuredLayout.count(listVal))
            markPairOfStructured(iterSlot, pairOfStructuredLayout[listVal]);
        listVal = iterSlot;

        // Native i64 length + index (avoids boxed PyNumber_Add on idx+1)
        std::string lenI64 = "__len_i64_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_SizeI64", listVal}, lenI64, "i64");
        noteType(lenI64, "i64");
        numericLocals.insert(lenI64);

        std::string idxVar = node->id + "__idx";
        std::string idxInit = "$i" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "i64const", {"0"}, idxInit, "i64");
        noteType(idxInit, "i64");
        ir.addInstruction(currentFunc, "i64assign", {idxInit}, idxVar, "i64");
        noteType(idxVar, "i64");
        numericLocals.insert(idxVar);

        std::string loopLabel = "for_loop_" + std::to_string(tempCounter);
        std::string bodyLabel = "for_body_" + std::to_string(tempCounter);
        std::string contLabel = "for_cont_" + std::to_string(tempCounter);
        std::string exitLabel = "for_exit_" + std::to_string(tempCounter);
        tempCounter++;

        std::string savedCont = loopContinueLabel, savedBreak = loopBreakLabel;
        loopTryDepths.push_back(activeTries.size());
        loopContinueLabel = contLabel;
        loopBreakLabel    = exitLabel;

        ir.addInstruction(currentFunc, "label", {}, loopLabel);
        auto loopEntryTypes = valueTypes;
        std::string cmpRes = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "i64icmp", {"Lt", idxVar, lenI64}, cmpRes, "bool");
        ir.addInstruction(currentFunc, "br", {cmpRes, bodyLabel, exitLabel});

        ir.addInstruction(currentFunc, "label", {}, bodyLabel);
        std::string itemRes = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyList_GetItemI64", listVal, idxVar}, itemRes);
        // P0: Propagate element layouts from iterated list to loop item.
        // - structured list (SYSTEM): item is one body tuple layout
        // - pair-of-structured (PAIRS): item is a pair; each child gets body layout
        // - else: homogeneous float/int element maps
        bool laidOut = false;
        if (structuredElementLayout.count(listVal)) {
            applyTupleLayout(itemRes, structuredElementLayout[listVal]);
            laidOut = true;
        } else if (pairOfStructuredLayout.count(listVal)) {
            childStructuredLayout[itemRes] = pairOfStructuredLayout[listVal];
            laidOut = true;
        }
        if (!laidOut) {
            for (auto& fnx : ir.functions) {
                if (fnx.name != currentFunc) continue;
                auto sel = fnx.structuredElementLayout.find(listVal);
                if (sel != fnx.structuredElementLayout.end() && !sel->second.empty()) {
                    applyTupleLayout(itemRes, sel->second);
                    laidOut = true;
                    break;
                }
                auto pel = fnx.pairOfStructuredLayout.find(listVal);
                if (pel != fnx.pairOfStructuredLayout.end() && !pel->second.empty()) {
                    childStructuredLayout[itemRes] = pel->second;
                    laidOut = true;
                    break;
                }
                auto sit = fnx.subscriptElementTypes.find(listVal);
                if (sit != fnx.subscriptElementTypes.end() && !sit->second.empty()) {
                    fnx.subscriptElementTypes[itemRes] = sit->second;
                    break;
                }
                auto cit = fnx.containerElementTypes.find(listVal);
                if (cit != fnx.containerElementTypes.end() && !cit->second.empty()) {
                    for (const auto& [idx, ctypes] : cit->second) {
                        if (ctypes == "float_list") {
                            std::unordered_map<size_t, std::string> elemTypes;
                            for (size_t i = 0; i <= 20; i++) elemTypes[i] = "float";
                            fnx.subscriptElementTypes[itemRes] = elemTypes;
                            break;
                        } else if (ctypes == "int_list") {
                            std::unordered_map<size_t, std::string> elemTypes;
                            for (size_t i = 0; i <= 20; i++) elemTypes[i] = "int";
                            fnx.subscriptElementTypes[itemRes] = elemTypes;
                            break;
                        }
                    }
                    break;
                }
                break;
            }
        }
        if (node->id == "__unpack__") {
            if (iterIndex == 1) {
                lowerUnpackTarget(node->children[0].get(), itemRes);
            } else {
                for (size_t j = 0; j < node->args.size(); ++j) {
                    std::string ic = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {std::to_string(j)}, ic);
                    std::string elem = "$t" + std::to_string(tempCounter++);
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
                std::string dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyCell_Set", cellSlot, node->id}, dummy);
            }
        }

        for (size_t i = iterIndex + 1; i < node->children.size(); ++i)
            lower(node->children[i].get());

        // Continue label: jump here from `continue` statements — this skips
        // the body but still runs the index increment before looping.
        ir.addInstruction(currentFunc, "label", {}, contLabel);

        // idxVar = idxVar + 1 (native i64)
        std::string oneRes = "$i" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "i64const", {"1"}, oneRes, "i64");
        noteType(oneRes, "i64");
        std::string nextIdx = "$i" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "i64add", {idxVar, oneRes}, nextIdx, "i64");
        noteType(nextIdx, "i64");
        ir.addInstruction(currentFunc, "i64assign", {nextIdx}, idxVar, "i64");

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
            std::string nativeConst = "$i" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "i64const", {std::to_string(constVal)}, nativeConst, "i64");
            noteType(nativeConst, "i64");
            return nativeConst;
        }
        std::string boxed = lowerExpr(arg);
        std::string native = "$i" + std::to_string(tempCounter++);
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
            startRes = "$i" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "i64const", {"0"}, startRes, "i64");
            noteType(startRes, "i64");
            stopRes = lowerRangeI64Arg(call->children[1].get());
            stepRes = "$i" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "i64const", {"1"}, stepRes, "i64");
            noteType(stepRes, "i64");
        } else if (argc >= 2) {
            startRes = lowerRangeI64Arg(call->children[1].get());
            stopRes = lowerRangeI64Arg(call->children[2].get());
            if (argc >= 3) {
                stepSign = constantStepSign(call->children[3].get());
                stepRes = lowerRangeI64Arg(call->children[3].get());
            } else {
                stepRes = "$i" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "i64const", {"1"}, stepRes, "i64");
                noteType(stepRes, "i64");
            }
        } else {
            startRes = "$i" + std::to_string(tempCounter++);
            stopRes = "$i" + std::to_string(tempCounter++);
            stepRes = "$i" + std::to_string(tempCounter++);
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
        std::string cmpRes = "$i" + std::to_string(tempCounter++);
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
        std::string nextIdx = "$i" + std::to_string(tempCounter++);
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
        std::string listRes = "$t" + std::to_string(tempCounter++);
        if (node->type == "Tuple") {
            // Handle tuple literal — emit a real tuple (type 7).
            std::string sizeConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
            ir.addInstruction(currentFunc, "call", {"PyTuple_NewBoxed", sizeConst}, listRes);
            noteType(listRes, "tuple");
            for (size_t i = 0; i < n; ++i) {
                std::string idxConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idxConst);
                ir.addInstruction(currentFunc, "call", {"PyTuple_SetItemBoxed", listRes, idxConst, elems[i]}, "");
            }
        } else {
            // Handle list literal
            if (allInt) {
                std::string sizeConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
                ir.addInstruction(currentFunc, "call", {"PyList_NewIntBoxed", sizeConst}, listRes);
                noteType(listRes, "list_int");
                knownIntLists.insert(listRes);
            } else if (allFloat) {
                std::string sizeConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
                ir.addInstruction(currentFunc, "call", {"PyList_NewFloatBoxed", sizeConst}, listRes);
                noteType(listRes, "list_float");
                knownFloatLists.insert(listRes);
            } else {
                std::string sizeConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(n)}, sizeConst);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", sizeConst}, listRes);
                noteType(listRes, "list");
                for (auto& fn : ir.functions) {
                    if (fn.name == currentFunc) {
                        std::unordered_map<size_t, std::string> idxMap;
                        for (size_t i = 0; i < n; ++i) {
                            idxMap[i] = elemTypeList[i];
                            // P0: also record container kinds for nested list elements
                            if (elemTypeList[i] == "list_float" || elemTypeList[i] == "float_list")
                                fn.containerElementTypes[listRes][i] = "float_list";
                            else if (elemTypeList[i] == "list_int" || elemTypeList[i] == "int_list")
                                fn.containerElementTypes[listRes][i] = "int_list";
                            else if (elemTypeList[i] == "float" || elemTypeList[i] == "int")
                                fn.containerElementTypes[listRes][i] = elemTypeList[i];
                        }
                        fn.subscriptElementTypes[listRes] = idxMap;
                        fn.listElementTypes[listRes] = elemTypeList;
                        break;
                    }
                }
            }

            bool containsTok = false;
            for (size_t i = 0; i < n; ++i) {
                std::string idxConst = "$c" + std::to_string(tempCounter++);
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
        }
        return listRes;
    }

    std::string lowerSet(const ASTNode* node) {
        auto elems = lowerElements(node);
        std::string setRes = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PySet_New"}, setRes);
        noteType(setRes, "set");
        for (const auto& e : elems) {
            ir.addInstruction(currentFunc, "call", {"PySet_Add", setRes, e}, "");
        }
        return setRes;
    }

     std::string lowerDict(const ASTNode* node) {
         std::string dictRes = "$t" + std::to_string(tempCounter++);
         ir.addInstruction(currentFunc, "call", {"PyDict_New"}, dictRes);
         noteType(dictRes, "dict");

         std::string commonValType;
         std::vector<std::string> commonLayout;
         bool layoutAgree = true;
         bool haveLayout = false;

         for (size_t i = 0; i + 1 < node->children.size(); i += 2) {
             const ASTNode* keyNode = node->children[i].get();
             // {**mapping} entry — real bug found and fixed: this whole
             // "DictUnpack" case previously didn't exist, so a None-key
             // pair (real Python's ast.Dict sentinel for `**expr` inside
             // a dict literal) got lowered as if it were a literal
             // key/value pair, producing garbage (`{**d1, **d2}` printed
             // as `{None: {'b': 2}}`, silently losing d1's entries
             // entirely). See PythonParser.cpp's Dict-handling comment.
             if (keyNode && keyNode->type == "DictUnpack") {
                 std::string src = lowerExpr(node->children[i + 1].get());
                 std::string dummy = "$t" + std::to_string(tempCounter++);
                 ir.addInstruction(currentFunc, "call", {"PyDict_Update", dictRes, src}, dummy);
                 continue;
             }
             std::string key = lowerExpr(keyNode);
             std::string val = lowerExpr(node->children[i + 1].get());
             ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", dictRes, key, val}, "");

             std::string valType = typeOf(val);
             if (commonValType.empty()) commonValType = valType;
             else if (commonValType != valType) commonValType = "boxed";

             // P0: track shared structured layout of values (nbody bodies)
             std::vector<std::string> vlayout;
             for (auto& fn : ir.functions) {
                 if (fn.name != currentFunc) continue;
                 auto lit = fn.listElementTypes.find(val);
                 if (lit != fn.listElementTypes.end() && !lit->second.empty())
                     vlayout = lit->second;
                 break;
             }
             if (!vlayout.empty()) {
                 if (!haveLayout) { commonLayout = vlayout; haveLayout = true; }
                 else if (commonLayout != vlayout) layoutAgree = false;
             } else {
                 layoutAgree = false;
             }
         }
         if (commonValType == "boxed") commonValType = "";
         if (!commonValType.empty())
             tempContainerElementTypes[dictRes] = commonValType;
         if (haveLayout && layoutAgree && !commonLayout.empty())
             dictValueLayouts[dictRes] = commonLayout;
         return dictRes;
     }

    std::string lowerAttribute(const ASTNode* node) {
        std::string obj = lowerExpr(node->children.empty() ? nullptr : node->children[0].get());
        std::string res = "$t" + std::to_string(tempCounter++);
        std::string attrNameConst = "$c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"\"" + node->id + "\""}, attrNameConst, "str");
        // Pyc_GetAttr (not the plain Pyc_GetItem every other internal
        // lookup uses) so a @property getter on the looked-up name is
        // invoked automatically instead of returning its raw internal
        // token — see Pyc_GetAttr's comment in Runtime.cpp. This is the
        // one call site that represents a real, user-written `obj.attr`
        // read; every other Pyc_GetItem use in this file is an internal
        // lookup (module dispatch, method-token resolution, class-dict
        // internals) that must NOT trigger property auto-invocation.
        ir.addInstruction(currentFunc, "call", {"Pyc_GetAttr", obj, attrNameConst}, res);
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
                // S5: Propagate container element types from assigned value (e.g., function call results)
                // to the target name, so that subscriptElementTypes from the value is inherited by the name.
                if (!val.empty()) {
                    for (auto& fn : ir.functions) {
                        auto sit = fn.subscriptElementTypes.find(val);
                        if (sit != fn.subscriptElementTypes.end() && !sit->second.empty()) {
                            fn.subscriptElementTypes[name] = sit->second;
                        }
                        auto cit = fn.containerElementTypes.find(val);
                        if (cit != fn.containerElementTypes.end() && !cit->second.empty()) {
                            fn.containerElementTypes[name] = cit->second;
                        }
                        auto lit = fn.listElementTypes.find(val);
                        if (lit != fn.listElementTypes.end() && !lit->second.empty()) {
                            fn.listElementTypes[name] = lit->second;
                        }
                    }
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
            std::string attrConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + attrName + "\""}, attrConst, "str");
            std::string dummy = "$t" + std::to_string(tempCounter++);
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
                std::string dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_SetSlice", obj, start, stop, step, val}, dummy);
                return;
            }
            std::string idx = lowerExpr(idxnode);
            // A4/A7: use native set for proven homogeneous lists with native values.
            // A7: Auto helpers only when the container is known to be a list — never for
            // dict/boxed (d["k"]=99 must go through Pyc_SetItem).
            std::string objType = typeOf(obj);
            std::string valType = typeOf(val);
            bool isIntList = (objType == "list_int");
            bool isFloatList = (objType == "list_float");
            bool objIsList = isIntList || isFloatList || objType == "list" ||
                             knownFloatLists.count(obj) || knownIntLists.count(obj);
            bool valIsInt = (valType == "int" || valType == "i64" || valType == "bool");
            bool valIsFloat = (valType == "float");
            std::string dummy;
            if (isIntList && valIsInt) {
                dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemInt64", obj, idx, val}, dummy);
            } else if (isFloatList && valIsFloat) {
                dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemDouble", obj, idx, val}, dummy);
            } else if (objIsList && valIsFloat) {
                dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemDoubleAuto", obj, idx, val}, dummy);
            } else if (objIsList && valIsInt) {
                dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemInt64Auto", obj, idx, val}, dummy);
            } else {
                dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_SubscriptSetItem", obj, idx, val}, dummy);
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
            // Real pre-existing bug, found via functools.partial(): a
            // lambda lowered anywhere (e.g. as another call's argument,
            // `foo(lambda x: x+1)`) sets lastLambdaSynthetic without it
            // being cleared unless the *very next* thing lowered happens
            // to be a plain assignment — so an unrelated later statement
            // like `add5 = functools.partial(...)` could pick up the
            // stale flag and alias `add5` directly to that earlier,
            // unrelated lambda's IR function (an LLVM arity-mismatch
            // verifier error when later called with a different argument
            // count, not a silent wrong answer, but a real compiler bug
            // regardless). Clearing here, before lowering this
            // assignment's own RHS, means the check below (~15 lines
            // down) only ever sees a value freshly set by *this*
            // statement's own RHS lowering.
            lastLambdaSynthetic.clear();
            std::string val = lowerExpr(node->children[0].get());
            // B5: if the target is cell-backed *in this function* (we own or receive the cell here),
            // emit PyCell_Set instead of a plain assign. A name that is only a nonlocal target in a
            // nested scope should not be routed through a cell at this level.
            if (isCellBackedHere(node->id)) {
                std::string cellSlot = node->id + "_cell";
                std::string dummy = "$t" + std::to_string(tempCounter++);
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
                numericFloatLocals.erase(node->id);
                noteType(node->id, "i64");
            } else if (!isGlob && (numericFloatLocals.count(node->id) || vt == "float")) {
                // Keep mag/b1m/dx/… as native f64 locals for float chains (m1*mag, dt*mag)
                ir.addInstruction(currentFunc, "f64assign", {val}, node->id, "float");
                noteType(node->id, "float");
                numericFloatLocals.insert(node->id);
                numericLocals.erase(node->id);
            } else {
                ir.addInstruction(currentFunc, "assign", {val}, node->id);
                noteType(node->id, vt);
                // S4: For module-level dicts, propagate value types to the global name
                if (isGlob && vt == "dict" && tempContainerElementTypes.count(val)) {
                    dictValueTypes[node->id] = tempContainerElementTypes[val];
                }
                if (vt == "dict" && dictValueLayouts.count(val)) {
                    dictValueLayouts[node->id] = dictValueLayouts[val];
                }
                // P0: propagate structured / pair layouts through assigns (SYSTEM, PAIRS, …)
                copyLayoutMaps(val, node->id);
                if (structuredElementLayout.count(val))
                    markStructuredList(node->id, structuredElementLayout[val]);
                if (pairOfStructuredLayout.count(val))
                    markPairOfStructured(node->id, pairOfStructuredLayout[val]);
                // Non-numeric assign: drop native slots
                killNumericLocal(node->id);
                if (isGlob) {
                    numericLocals.erase(node->id);
                    numericFloatLocals.erase(node->id);
                }
            }
            // S5: Propagate container element types from assigned value to target name.
            // Propagates subscriptElementTypes, containerElementTypes, and listElementTypes
            // so that names assigned from function call results (e.g., PAIRS = combinations(SYSTEM))
            // inherit the element type information for downstream subscript optimization.
            if (!val.empty() && vt != "int" && vt != "i64" && vt != "bool") {
                for (auto& fn : ir.functions) {
                    auto sit = fn.subscriptElementTypes.find(val);
                    if (sit != fn.subscriptElementTypes.end() && !sit->second.empty()) {
                        fn.subscriptElementTypes[node->id] = sit->second;
                    }
                    auto cit = fn.containerElementTypes.find(val);
                    if (cit != fn.containerElementTypes.end() && !cit->second.empty()) {
                        fn.containerElementTypes[node->id] = cit->second;
                    }
                    auto lit = fn.listElementTypes.find(val);
                    if (lit != fn.listElementTypes.end() && !lit->second.empty()) {
                        fn.listElementTypes[node->id] = lit->second;
                    }
                }
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
                std::string vt = typeOf(value);
                if (isCell) {
                    std::string cellSlot = target->id + "_cell";
                    std::string dummy = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyCell_Set", cellSlot, value}, dummy);
                    noteType(target->id, vt);
                    killNumericLocal(target->id);
                    if (!value.empty() && (callableTokenTemps.count(value) || callableTokenToSynthetic.count(value) ||
                                           listsContainingCallableTokens.count(value))) {
                        namesThatMayHoldCallableTokens.insert(target->id);
                    }
                    return;
                }
                ir.addInstruction(currentFunc, "assign", {value}, target->id);
                noteType(target->id, vt);
                if (vt == "list_float") {
                    knownFloatLists.insert(target->id);
                    copyLayoutMaps(value, target->id);
                    killNumericLocal(target->id);
                } else if (vt == "list_int") {
                    knownIntLists.insert(target->id);
                    copyLayoutMaps(value, target->id);
                    killNumericLocal(target->id);
                } else if (vt == "float") {
                    // Typed float for binop resultType; keep boxed storage for unpack
                    // sources (GetItemObj) to avoid breaking offset_momentum / energy.
                    noteType(target->id, "float");
                    killNumericLocal(target->id);
                } else if (vt == "int" || vt == "i64") {
                    noteType(target->id, "int");
                    killNumericLocal(target->id);
                } else {
                    copyLayoutMaps(value, target->id);
                    for (auto& fn : ir.functions) {
                        if (fn.name != currentFunc) continue;
                        auto lit = fn.listElementTypes.find(value);
                        if (lit != fn.listElementTypes.end() && !lit->second.empty())
                            applyTupleLayout(target->id, lit->second);
                        break;
                    }
                    killNumericLocal(target->id);
                }
            }
            if (!value.empty() && (callableTokenTemps.count(value) || callableTokenToSynthetic.count(value) ||
                                   listsContainingCallableTokens.count(value))) {
                namesThatMayHoldCallableTokens.insert(target->id);
            }
            return;
        }
        // obj.attr, obj[key] as an unpacking target — found and fixed
        // while bug hunting: this function only ever handled a "Name"
        // leaf target; an Attribute or Subscript target (the extremely
        // common `self.x, self.y = x, y` idiom in __init__, or
        // `d["k"], other = 1, 2`) fell through to the guard just below
        // and silently did nothing at all — confirmed `self.x, self.y =
        // x, y` left both attributes unset (reading back as None)
        // with no error of any kind.
        if (target->type == "Attribute") {
            std::string obj = lowerExpr(target->children.empty() ? nullptr : target->children[0].get());
            std::string attrConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + target->id + "\""}, attrConst, "str");
            std::string dummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", obj, attrConst, value}, dummy);
            return;
        }
        if (target->type == "Subscript") {
            std::string obj = lowerExpr(target->children.size() > 0 ? target->children[0].get() : nullptr);
            std::string idx = lowerExpr(target->children.size() > 1 ? target->children[1].get() : nullptr);
            std::string dummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_SubscriptSetItem", obj, idx, value}, dummy);
            return;
        }
        if (target->type != "Tuple" && target->type != "List") return;

        // Check for star unpacking: any child is a Starred node.
        int starIndex = -1;
        for (size_t i = 0; i < target->children.size(); ++i) {
            if (target->children[i] && target->children[i]->type == "Starred") {
                starIndex = (int)i;
                break;
            }
        }
        if (starIndex >= 0) {
            // Star unpacking: a, *b, c = value
            // nBefore = starIndex, nAfter = n - starIndex - 1
            const size_t n = target->children.size();
            int nBefore = starIndex;
            int nAfter = (int)n - starIndex - 1;

            // Call PyList_UnpackStar(value, nBefore, nAfter) → result list
            // result = [before0, before1, ..., starList, ..., after0]
            // nBefore/nAfter are passed as raw i64 constants (digit strings in the IR).
            std::string resultList = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_UnpackStar", value, std::to_string(nBefore), std::to_string(nAfter)}, resultList);

            // Extract each element from the result list and assign to targets.
            for (int i = 0; i < nBefore; ++i) {
                std::string idx = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idx, "int");
                std::string elem = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", resultList, idx}, elem);
                lowerUnpackTarget(target->children[i].get(), elem);
            }
            // Star target: the star slot is at index nBefore in the result list.
            {
                std::string idx = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(nBefore)}, idx, "int");
                std::string starSlot = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", resultList, idx}, starSlot);
                noteType(starSlot, "list");
                if (target->children[starIndex] && !target->children[starIndex]->children.empty()) {
                    lowerUnpackTarget(target->children[starIndex]->children[0].get(), starSlot);
                }
            }
            for (int i = 0; i < nAfter; ++i) {
                std::string idx = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(nBefore + 1 + i)}, idx, "int");
                std::string elem = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", resultList, idx}, elem);
                lowerUnpackTarget(target->children[starIndex + 1 + i].get(), elem);
            }
            // DECREF the result list (PyList_GetItemObj returns new refs; the list itself is now dead).
            std::string decRefDummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Py_DECREF", resultList}, decRefDummy);
            return;
        }

        // Resolve per-index element kinds for this value
        std::vector<std::string> idxKinds;
        for (auto& fn : ir.functions) {
            if (fn.name != currentFunc) continue;
            auto lit = fn.listElementTypes.find(value);
            if (lit != fn.listElementTypes.end()) idxKinds = lit->second;
            break;
        }
        std::vector<std::string> childBody;
        if (childStructuredLayout.count(value))
            childBody = childStructuredLayout[value];

        bool parentFloatList = (typeOf(value) == "list_float") || knownFloatLists.count(value);
        bool parentIntList = (typeOf(value) == "list_int") || knownIntLists.count(value);

        const size_t n = target->children.size();
        std::vector<std::string> elems(n);

        // Bulk unpack 2/3-element tuples (nbody pair/body spine) — one runtime call
        if (n == 2 || n == 3) {
            for (size_t i = 0; i < n; ++i)
                elems[i] = "$t" + std::to_string(tempCounter++);
            std::vector<std::string> ops;
            ops.push_back(n == 3 ? "PyList_Unpack3" : "PyList_Unpack2");
            ops.push_back(value);
            for (size_t i = 0; i < n; ++i) ops.push_back(elems[i]);
            ir.addInstruction(currentFunc, "call", ops, "");
        } else {
            for (size_t i = 0; i < n; ++i) {
                elems[i] = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call",
                    {"PyList_GetItemI64", value, std::to_string((long long)i)}, elems[i]);
            }
        }

        for (size_t i = 0; i < n; ++i) {
            const std::string& elem = elems[i];
            std::string kind;
            if (!childBody.empty()) kind = "body";
            else if (i < idxKinds.size()) kind = idxKinds[i];
            else if (parentFloatList) kind = "float";
            else if (parentIntList) kind = "int";

            if (!value.empty() && (callableTokenTemps.count(value) || listsContainingCallableTokens.count(value))) {
                callableTokenTemps.insert(elem);
            }

            if (kind == "list_float" || kind == "float_list") {
                noteType(elem, "list_float");
                knownFloatLists.insert(elem);
                for (auto& fn : ir.functions) {
                    if (fn.name != currentFunc) continue;
                    for (size_t k = 0; k <= 20; k++) fn.subscriptElementTypes[elem][k] = "float";
                    fn.containerElementTypes[elem][0] = "float_list";
                    break;
                }
            } else if (kind == "list_int" || kind == "int_list") {
                noteType(elem, "list_int");
                knownIntLists.insert(elem);
                for (auto& fn : ir.functions) {
                    if (fn.name != currentFunc) continue;
                    for (size_t k = 0; k <= 20; k++) fn.subscriptElementTypes[elem][k] = "int";
                    fn.containerElementTypes[elem][0] = "int_list";
                    break;
                }
            } else if (kind == "body" && !childBody.empty()) {
                applyTupleLayout(elem, childBody);
            } else if (kind == "float" || parentFloatList) {
                noteType(elem, "float");
            } else if (kind == "int" || parentIntList) {
                noteType(elem, "int");
            }

            lowerUnpackTarget(target->children[i].get(), elem);
        }
    }

    // Lower a single `del` target. Supports:
    //   del name        — free the local/global alloca (DECREF the value, mark slot as unowned)
    //   del d[k]        — call Pyc_DelItem(obj, key) (dict key deletion or
    //                     list item removal by index, dispatched at runtime
    //                     on obj's type — previously always called
    //                     PyDict_DelItem directly, so `del lst[i]` on any
    //                     list silently did nothing at all; found while
    //                     hunting for more instances of the truthiness
    //                     bug's underlying pattern, see IMPLEMENTATION.md)
    //   del lst[s:e]    — call Pyc_DelSlice (Pyc_DelItem only accepts int
    //                     keys; empty-replacement SetSlice is not delete
    //                     for an extended slice like [::2])
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
            std::string dummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Py_DECREF", name}, dummy);
            // Emit a real nconst and assign it to the slot.
            std::string noneRes = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "nconst", {}, noneRes, "none");
            ir.addInstruction(currentFunc, "assign", {noneRes}, name);
            noteType(name, "none");
            killNumericLocal(name);
        } else if (target->type == "Subscript") {
            // del d[k]  or  del lst[s:e] / del lst[::step]
            // Slice indices must not go through Pyc_DelItem: that helper
            // only accepts int keys, so del lst[1:3] was a silent no-op
            // on every list (A4 and boxed). Same start/stop/step lowering
            // as get/set slice.
            if (target->children.size() < 2) return;
            std::string obj = lowerExpr(target->children[0].get());
            const ASTNode* idxnode = target->children[1].get();
            if (idxnode && idxnode->type == "Slice") {
                std::string start = lowerExpr(idxnode->children.size() > 0 ? idxnode->children[0].get() : nullptr);
                std::string stop  = lowerExpr(idxnode->children.size() > 1 ? idxnode->children[1].get() : nullptr);
                std::string step  = (idxnode->children.size() > 2 && idxnode->children[2])
                                       ? lowerExpr(idxnode->children[2].get()) : "";
                std::string dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_DelSlice", obj, start, stop, step}, dummy);
                return;
            }
            std::string idx = lowerExpr(idxnode);
            std::string dummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_DelItem", obj, idx}, dummy);
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
            std::string objType = typeOf(obj);
            bool isFloatList = (objType == "list_float") || knownFloatLists.count(obj);
            bool isIntList = (objType == "list_int") || knownIntLists.count(obj);
            std::string cur = "$t" + std::to_string(tempCounter++);
            if (isFloatList) {
                ir.addInstruction(currentFunc, "call", {"Pyc_Subscript", obj, idx}, cur, "float");
                noteType(cur, "float");
                numericFloatLocals.insert(cur);
            } else if (isIntList) {
                ir.addInstruction(currentFunc, "call", {"Pyc_Subscript", obj, idx}, cur, "int");
                noteType(cur, "int");
                numericLocals.insert(cur);
            } else {
                ir.addInstruction(currentFunc, "call", {"Pyc_Subscript", obj, idx}, cur);
            }
            std::string res = "$t" + std::to_string(tempCounter++);
            std::string resultType = numericResultType(op, cur, rhs);
            if (resultType == "boxed" && isFloatList) resultType = "float";
            if (resultType == "boxed" && isIntList) resultType = "int";
            ir.addInstruction(currentFunc, op, {cur, rhs}, res, resultType);
            noteType(res, resultType);
            if (resultType == "float") numericFloatLocals.insert(res);
            if (resultType == "int" || resultType == "i64") numericLocals.insert(res);
            std::string dummy = "$t" + std::to_string(tempCounter++);
            std::string valType = typeOf(res);
            bool valIsFloat = (valType == "float" || resultType == "float");
            bool valIsInt = (valType == "int" || valType == "i64" || resultType == "int");
            if (isFloatList && valIsFloat) {
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemDouble", obj, idx, res}, dummy);
            } else if (isIntList && valIsInt) {
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemInt64", obj, idx, res}, dummy);
            } else if (valIsFloat) {
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemDoubleAuto", obj, idx, res}, dummy);
            } else if (valIsInt) {
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemInt64Auto", obj, idx, res}, dummy);
            } else {
                ir.addInstruction(currentFunc, "call", {"Pyc_SubscriptSetItem", obj, idx, res}, dummy);
            }
        } else if (node->id == "__attr_assign__") {
            // obj.attr op= val — children[0]=Attribute node, children[1]=rhs.
            // Found missing entirely while bug hunting: this case used to
            // fall through the "__subscript__" branch above (the parser
            // didn't distinguish Attribute from Subscript targets for
            // AugAssign), which reads a bogus empty-string "index" from an
            // Attribute node and crashes with a runtime KeyError on the
            // ordinary `obj.attr += x` idiom. obj is lowered once and
            // reused for both the get and the set, matching the
            // "__subscript__" branch's care to avoid double-evaluating an
            // object expression with side effects (e.g. `f().attr += 1`).
            if (node->children.size() < 2) return;
            const ASTNode* attrTarget = node->children[0].get();
            std::string obj = lowerExpr(attrTarget->children.empty() ? nullptr : attrTarget->children[0].get());
            std::string attrName = attrTarget->id;
            std::string attrConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + attrName + "\""}, attrConst, "str");
            std::string cur = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, attrConst}, cur);
            std::string rhs = lowerExpr(node->children[1].get());
            std::string res = "$t" + std::to_string(tempCounter++);
            std::string resultType = numericResultType(op, cur, rhs);
            ir.addInstruction(currentFunc, op, {cur, rhs}, res, resultType);
            noteType(res, resultType);
            std::string dummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_SetItem", obj, attrConst, res}, dummy);
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
                lhsVal = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyCell_Get", cellSlot}, lhsVal);
            } else {
                lhsVal = node->id;
            }
            std::string rhs = lowerExpr(node->children[0].get());
            std::string result = "$t" + std::to_string(tempCounter++);
            std::string resultType = numericResultType(op, lhsVal, rhs);
            ir.addInstruction(currentFunc, op, {lhsVal, rhs}, result, resultType);
            noteType(result, resultType);
            if (isCellBackedHere(node->id)) {
                std::string cellSlot = node->id + "_cell";
                std::string dummy = "$t" + std::to_string(tempCounter++);
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
            std::string res = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetSlice", obj, start, stop, step}, res);
            return res;
        }
        std::string idx = lowerExpr(node->children.size() > 1 ? node->children[1].get() : nullptr);
        std::string res = "$t" + std::to_string(tempCounter++);
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
        }
        // S1: check containerElementTypes / subscriptElementTypes for variable names
        // that have known element types propagated from defaults or globals.
        // This applies to ALL object types (not just "list") - e.g., temps from
        // unpacking nested tuples or intermediate subscripts need element type info.
        //
        // Critical distinction:
        //   "float" / "int"           → scalar element (native get path)
        //   "list_float"/"float_list" → nested float list (boxed get, then native nested)
        //   "list_int"/"int_list"     → nested int list
        auto applyElemType = [&](const std::string& et) {
            if (et == "float") {
                elemType = "float";
                noteType(res, "float");
            } else if (et == "int" || et == "i64" || et == "bool") {
                elemType = "int";
                noteType(res, "int");
            } else if (et == "float_list" || et == "list_float") {
                // Result is a float list container — keep boxed for codegen,
                // but mark type so nested subscripts use the native float path.
                elemType = "boxed";
                noteType(res, "list_float");
                knownFloatLists.insert(res);
                for (auto& fn : ir.functions) {
                    if (fn.name != currentFunc) continue;
                    for (size_t i = 0; i <= 20; i++) fn.subscriptElementTypes[res][i] = "float";
                    fn.containerElementTypes[res][0] = "float_list";
                    break;
                }
            } else if (et == "int_list" || et == "list_int") {
                elemType = "boxed";
                noteType(res, "list_int");
                knownIntLists.insert(res);
                for (auto& fn : ir.functions) {
                    if (fn.name != currentFunc) continue;
                    for (size_t i = 0; i <= 20; i++) fn.subscriptElementTypes[res][i] = "int";
                    fn.containerElementTypes[res][0] = "int_list";
                    break;
                }
            } else if (et == "list" || et == "dict" || et == "str") {
                elemType = "boxed";
                noteType(res, et);
            } else if (!et.empty() && et != "boxed") {
                elemType = "boxed";
                noteType(res, "boxed");
            }
        };
        {
            std::string idxValue = "";
            if (node->children.size() > 1 && node->children[1] &&
                node->children[1]->type == "Constant") {
                // Constant nodes store literal values in node->value
                if (!node->children[1]->value.empty())
                    idxValue = node->children[1]->value;
                else if (node->children[1]->args.size() == 1)
                    idxValue = node->children[1]->args[0];
            }
            size_t idxVal = 0;
            bool hasLiteralIndex = !idxValue.empty();
            if (hasLiteralIndex) {
                try { idxVal = std::stoull(idxValue); } catch (...) { hasLiteralIndex = false; }
            }

            bool foundElem = false;
            for (auto& fn : ir.functions) {
                auto sit = fn.subscriptElementTypes.find(obj);
                if (sit != fn.subscriptElementTypes.end() && !sit->second.empty()) {
                    if (hasLiteralIndex) {
                        auto iit = sit->second.find(idxVal);
                        if (iit != sit->second.end()) {
                            applyElemType(iit->second);
                            foundElem = true;
                        }
                    } else {
                        // Homogeneous containers: all indices share one element type
                        bool homogeneous = true;
                        std::string firstEt;
                        for (const auto& [ikey, et] : sit->second) {
                            if (firstEt.empty()) firstEt = et;
                            else if (et != firstEt) { homogeneous = false; break; }
                        }
                        if (homogeneous && !firstEt.empty()) {
                            applyElemType(firstEt);
                            foundElem = true;
                        }
                    }
                    if (foundElem) break;
                }
            }

            // S3: check listElementTypes for per-index element types from list construction
            if (!foundElem && elemType == "boxed") {
                for (auto& fn : ir.functions) {
                    auto lit = fn.listElementTypes.find(obj);
                    if (lit != fn.listElementTypes.end() && !lit->second.empty()) {
                        if (hasLiteralIndex && idxVal < lit->second.size()) {
                            applyElemType(lit->second[idxVal]);
                            foundElem = true;
                        } else if (!hasLiteralIndex) {
                            bool homogeneous = true;
                            std::string firstEt = lit->second[0];
                            for (const auto& et : lit->second) {
                                if (et != firstEt) { homogeneous = false; break; }
                            }
                            if (homogeneous) {
                                applyElemType(firstEt);
                                foundElem = true;
                            }
                        }
                        break;
                    }
                }
            }

            // Check containerElementTypes for typed element containers (float_list, etc.)
            // Here the map value is the *container kind of each element*, so float_list
            // means "element is a float list" — applyElemType handles that correctly.
            if (!foundElem && elemType == "boxed") {
                for (auto& fn : ir.functions) {
                    auto cit = fn.containerElementTypes.find(obj);
                    if (cit != fn.containerElementTypes.end() && !cit->second.empty()) {
                        std::string matchType;
                        if (hasLiteralIndex) {
                            auto iit = cit->second.find(idxVal);
                            if (iit != cit->second.end()) matchType = iit->second;
                        }
                        if (matchType.empty()) {
                            // Homogeneous fallback
                            bool homogeneous = true;
                            std::string firstEt;
                            for (const auto& [ikey, ctype] : cit->second) {
                                if (firstEt.empty()) firstEt = ctype;
                                else if (ctype != firstEt) { homogeneous = false; break; }
                            }
                            if (homogeneous) matchType = firstEt;
                        }
                        if (!matchType.empty() && matchType != "boxed" && matchType != "boxed_tuple") {
                            applyElemType(matchType);
                            foundElem = true;
                        }
                        if (foundElem) break;
                    }
                }
            }
        }
        // Also check if obj is a temp variable that was previously marked
        // as list_float / list_int via noteType.
        if (elemType == "boxed") {
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
        ir.addInstruction(currentFunc, "call", {"Pyc_Subscript", obj, idx}, res, elemType);
        // B4: if the container is a list we built that contained callable tokens, or the
        // container name is marked as holding tokens, mark the subscript result as a token temp.
        if (listsContainingCallableTokens.count(obj) || namesThatMayHoldCallableTokens.count(obj)) {
            callableTokenTemps.insert(res);
        }
        if (listsContainingBundles.count(obj) || namesThatMayHoldBundles.count(obj) || namesThatMayHoldListsWithBundles.count(obj)) {
            bundleTemps.insert(res);
        }
        // S5: For scalar results only, do not copy parent container maps onto the result.
        // Nested container results already received correct element maps in applyElemType.
        // When returning a whole list (no nested typing applied), copy maps for return tracking.
        if (!obj.empty() && elemType == "boxed" && typeOf(res) != "list_float" && typeOf(res) != "list_int") {
            for (auto& fn : ir.functions) {
                if (fn.name != currentFunc) continue;
                // Only copy if result has no element info yet
                if (!fn.subscriptElementTypes.count(res) || fn.subscriptElementTypes[res].empty()) {
                    auto sit = fn.subscriptElementTypes.find(obj);
                    if (sit != fn.subscriptElementTypes.end() && !sit->second.empty()) {
                        // Homogeneous nested container: if all parent entries agree and are
                        // themselves containers, the result is that container type's elements.
                        // Otherwise leave unset (mixed/unknown).
                    }
                }
                break;
            }
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
            // S4: track return element types when returning a list container.
            // This enables callers to inherit subscriptElementTypes/listElementTypes
            // from the returned list, enabling PyList_GetItemDouble optimization.
            for (auto& fnr : ir.functions) {
                if (fnr.name == currentFunc) {
                    auto sit = fnr.subscriptElementTypes.find(val);
                    if (sit != fnr.subscriptElementTypes.end() && !sit->second.empty()) {
                        fnr.returnSubscriptElementTypes = sit->second;
                        for (auto& [idx, et] : sit->second) {
                            // Preserve nested container types; only tag scalars as *list kinds
                            if (et == "list_float" || et == "float_list") {
                                fnr.returnContainerElementTypes[idx] = "float_list";
                            } else if (et == "list_int" || et == "int_list") {
                                fnr.returnContainerElementTypes[idx] = "int_list";
                            } else if (et == "float") {
                                fnr.returnContainerElementTypes[idx] = "float_list";
                            } else if (et == "int" || et == "i64" || et == "bool") {
                                fnr.returnContainerElementTypes[idx] = "int_list";
                            }
                        }
                    } else if (rt == "list_float" || knownFloatLists.count(val)) {
                        for (size_t i = 0; i <= 20; i++) fnr.returnSubscriptElementTypes[i] = "float";
                        fnr.returnContainerElementTypes[0] = "float_list";
                    } else if (rt == "list_int" || knownIntLists.count(val)) {
                        for (size_t i = 0; i <= 20; i++) fnr.returnSubscriptElementTypes[i] = "int";
                        fnr.returnContainerElementTypes[0] = "int_list";
                    } else {
                        auto lit = fnr.listElementTypes.find(val);
                        if (lit != fnr.listElementTypes.end() && !lit->second.empty()) {
                            for (size_t i = 0; i < lit->second.size(); ++i) {
                                const auto& et = lit->second[i];
                                fnr.returnSubscriptElementTypes[i] = et;
                                if (et == "list_float" || et == "float_list") {
                                    fnr.returnContainerElementTypes[i] = "float_list";
                                } else if (et == "list_int" || et == "int_list") {
                                    fnr.returnContainerElementTypes[i] = "int_list";
                                } else if (et == "float") {
                                    fnr.returnContainerElementTypes[i] = "float_list";
                                } else if (et == "int" || et == "i64" || et == "bool") {
                                    fnr.returnContainerElementTypes[i] = "int_list";
                                }
                            }
                        }
                    }
                    break;
                }
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
        std::string e2 = "$t" + std::to_string(tempCounter++);
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
        std::string excVar = "$t" + std::to_string(tempCounter++);
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
                    std::string nameConst = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + h->args[k] + "\""}, nameConst, "str");
                    std::string m = "$t" + std::to_string(tempCounter++);
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
            obj = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Super"}, obj);
            superProxyTemps.insert(obj);
        } else {
            obj = lowerExpr(attr->children.empty() ? nullptr : attr->children[0].get());
        }

        std::vector<std::string> args;
        bool hasKeywordArgs = false;
        for (size_t i = 1; i < node->children.size(); ++i) {
            if (node->children[i] && node->children[i]->type != "Keyword")
                args.push_back(lowerExpr(node->children[i].get()));
            else if (node->children[i])
                hasKeywordArgs = true;
        }
        // `args` is positional-only; keyword arguments are re-scanned out of
        // the AST by the individual arms that support them. The terminal
        // fallback builds its argument list from `args` alone, so it cannot
        // carry keywords -- which is why arms handling kwargs (split's
        // maxsplit=, format's **kwargs) must keep their fast path when
        // hasKeywordArgs is set, even for an unproven receiver. Falling
        // through would silently drop the keyword.

        // B6: Handle super().method() — look up method on parent class
        if (isSuperCall && superProxyTemps.count(obj) && !currentClass.empty()) {
            // Python's super() uses the MRO of the runtime instance's class.
            // We delegate to a runtime helper that:
            // 1. Gets self.__class__
            // 2. Looks up __mro__ from that class dict
            // 3. Finds currentClass in the MRO
            // 4. Calls the method on the next class in the MRO
            std::string res = "$t" + std::to_string(tempCounter++);
            std::string methodNameConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + methodName + "\""}, methodNameConst, "str");
            std::string definingClassConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + currentClass + "\""}, definingClassConst, "str");
            
            // Build args list: self, definingClass, methodName, [remaining args]
            std::string argList = "$t" + std::to_string(tempCounter++);
            std::string argCount = std::to_string(args.size() + 3); // self + definingClass + methodName + args
            std::string argCountConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {argCount}, argCountConst);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", argCountConst}, argList);
            
            // Add self at index 0
            std::string idxConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxConst);
            std::string setRes = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, "self"}, setRes);
            
            // Add definingClass at index 1
            idxConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, idxConst);
            setRes = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, definingClassConst}, setRes);
            
            // Add methodName at index 2
            idxConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"2"}, idxConst);
            setRes = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, methodNameConst}, setRes);
            
            // Add remaining args at indices 3+
            for (size_t i = 0; i < args.size(); ++i) {
                idxConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(i + 3)}, idxConst);
                setRes = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, args[i]}, setRes);
            }
            
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_SuperMethod", argList}, res);
            return res;
        }

        std::string res = "$t" + std::to_string(tempCounter++);

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
                // Scans node->children for a Keyword argument by name (same
                // technique `count=` already used) — the generic `args`
                // vector excludes keywords entirely, so any keyword-passed
                // value (count=, flags=, maxsplit=) must be pulled from the
                // raw AST directly.
                auto extractKw = [&](const char* name) -> std::string {
                    for (size_t i = 1; i < node->children.size(); ++i) {
                        const auto* ch = node->children[i].get();
                        if (ch && ch->type == "Keyword" && ch->id == name && !ch->children.empty()) {
                            return lowerExpr(ch->children[0].get());
                        }
                    }
                    return "";
                };
                std::string pat = args.size() > 0 ? args[0] : "";
                std::string subj = args.size() > 1 ? args[1] : "";
                if (methodName == "sub") {
                    std::string rep = args.size() > 1 ? args[1] : "";
                    std::string sub = args.size() > 2 ? args[2] : "";
                    std::string cnt = extractKw("count");
                    if (cnt.empty() && args.size() > 3) cnt = args[3];
                    std::string flg = extractKw("flags");
                    if (flg.empty() && args.size() > 4) flg = args[4];
                    ir.addInstruction(currentFunc, "call", {"PyBuiltin_ReSub", pat, rep, sub, cnt, flg}, res);
                    return res;
                } else if (methodName == "split") {
                    std::string maxsplit = extractKw("maxsplit");
                    if (maxsplit.empty() && args.size() > 2) maxsplit = args[2];
                    std::string flg = extractKw("flags");
                    if (flg.empty() && args.size() > 3) flg = args[3];
                    ir.addInstruction(currentFunc, "call", {"PyBuiltin_ReSplit", pat, subj, maxsplit, flg}, res);
                    return res;
                }
                std::string fn;
                if (methodName == "finditer")      fn = "PyBuiltin_ReFinditer";
                else if (methodName == "findall")   fn = "PyBuiltin_ReFindall";
                else if (methodName == "compile")   fn = "PyBuiltin_ReCompile";
                else if (methodName == "search")    fn = "PyBuiltin_ReSearch";
                else if (methodName == "match")     fn = "PyBuiltin_ReMatch";
                if (methodName == "compile") {
                    std::string flg = extractKw("flags");
                    if (flg.empty() && args.size() > 1) flg = args[1];
                    ir.addInstruction(currentFunc, "call", {fn, pat, flg}, res);
                    noteType(res, "regex");
                } else {
                    std::string flg = extractKw("flags");
                    if (flg.empty() && args.size() > 2) flg = args[2];
                    ir.addInstruction(currentFunc, "call", {fn, pat, subj, flg}, res);
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

        // datetime module dispatch. Two AST shapes, both recognized
        // structurally (like re/cmath above) rather than via typeOf,
        // since the receiver at construction time is always the bare
        // module name:
        //   datetime.date(y, m, d) / datetime.datetime(y, m, d, ...) /
        //   datetime.timedelta(...)   -- attr's base is Name("datetime")
        //   datetime.date.today() / datetime.datetime.now()
        //                              -- attr's base is itself an
        //                                 Attribute(Name("datetime"), "date"/"datetime")
        // noteType(res, ...) after construction drives the typeOf-gated
        // method-call dispatch further below (.isoformat()/.weekday()/
        // .total_seconds()) — see IMPLEMENTATION.md for why attribute
        // reads/arithmetic/comparisons/str() don't need this (they're
        // robust to untyped values via runtime-tag dispatch in
        // Runtime.cpp) while these method calls do.
        {
            // True for the literal name "datetime" or any `import datetime
            // as X` alias of it. Unlike datetimeCtorAliases below, no
            // isShadowedLocal check here: `import datetime [as X]` itself
            // calls noteType(X, "dict"), so valueTypes.count(X) is always
            // true right after a legitimate import — isShadowedLocal can't
            // distinguish that from real shadowing for this name.
            auto isDatetimeModuleName = [&](const ASTNode* n) -> bool {
                if (!n || n->type != "Name") return false;
                if (n->id == "datetime") return true;
                auto it = moduleNameAliases.find(n->id);
                return it != moduleNameAliases.end() && it->second == "datetime";
            };
            if (attr->children.size() >= 1 && attr->children[0] &&
                isDatetimeModuleName(attr->children[0].get()) &&
                (methodName == "date" || methodName == "datetime" || methodName == "timedelta")) {
                return lowerDatetimeConstruct(methodName, node, args);
            }
            // `datetime.date.today()` / `datetime.datetime.now()` (including
            // `import datetime as dt; dt.date.today()`), and the equally
            // valid bare-alias form after `from datetime import
            // date`/`datetime`: `date.today()` / `datetime.now()`.
            if (attr->children.size() >= 1 && attr->children[0]) {
                const ASTNode* base = attr->children[0].get();
                std::string innerKind; // "date" or "datetime", if recognized
                if (base->type == "Attribute" && !base->children.empty() && base->children[0] &&
                    isDatetimeModuleName(base->children[0].get())) {
                    innerKind = base->id;
                } else if (base->type == "Name" && datetimeCtorAliases.count(base->id) &&
                           !isShadowedLocal(base->id)) {
                    innerKind = datetimeCtorAliases.at(base->id);
                }
                if (innerKind == "date" && methodName == "today") {
                    ir.addInstruction(currentFunc, "call", {"PyDateTime_Today"}, res);
                    noteType(res, "date");
                    return res;
                }
                if (innerKind == "datetime" && methodName == "now") {
                    ir.addInstruction(currentFunc, "call", {"PyDateTime_Now"}, res);
                    noteType(res, "datetime");
                    return res;
                }
            }
        }
        // pathlib.Path(...) construction — literal "pathlib" name or any
        // `import pathlib as X` alias of it (no isShadowedLocal check,
        // same reasoning as isDatetimeModuleName above: `import pathlib`
        // itself sets valueTypes["pathlib"]="dict", which would always
        // look "shadowed").
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" && methodName == "Path") {
            const std::string& baseId = attr->children[0]->id;
            bool isPathlib = (baseId == "pathlib");
            if (!isPathlib) {
                auto it = moduleNameAliases.find(baseId);
                isPathlib = (it != moduleNameAliases.end() && it->second == "pathlib");
            }
            if (isPathlib) return lowerPathConstruct(node, args);
        }
        // hashlib.md5/sha1/sha256(...) construction — literal "hashlib"
        // name or any `import hashlib as X` alias of it.
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" &&
            (methodName == "md5" || methodName == "sha1" || methodName == "sha256")) {
            const std::string& baseId = attr->children[0]->id;
            bool isHashlib = (baseId == "hashlib");
            if (!isHashlib) {
                auto it = moduleNameAliases.find(baseId);
                isHashlib = (it != moduleNameAliases.end() && it->second == "hashlib");
            }
            if (isHashlib) return lowerHashlibConstruct(methodName, node, args);
        }
        // copy.copy(x)/copy.deepcopy(x) — literal "copy" name or any
        // `import copy as X` alias of it. Must be recognized structurally,
        // here, before the generic method-dispatch chain: the copy
        // module's own dict is itself typeOf=="dict", so it can't be
        // distinguished from a real dict the way os.path.join's fix
        // distinguished os.path (typeOf(obj)!="dict" doesn't help when
        // the receiver genuinely IS a "dict") — see PyCopy_Copy's comment
        // in Runtime.cpp.
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" &&
            (methodName == "copy" || methodName == "deepcopy")) {
            const std::string& baseId = attr->children[0]->id;
            bool isCopyMod = (baseId == "copy");
            if (!isCopyMod) {
                auto it = moduleNameAliases.find(baseId);
                isCopyMod = (it != moduleNameAliases.end() && it->second == "copy");
            }
            if (isCopyMod) {
                std::string arg = args.empty() ? "" : args[0];
                std::string fn = methodName == "copy" ? "PyCopy_Copy" : "PyCopy_Deepcopy";
                ir.addInstruction(currentFunc, "call", {fn, arg}, res);
                return res;
            }
        }
        // csv.writer(f) construction — literal "csv" name or any
        // `import csv as X` alias of it (same structural-recognition
        // rationale as copy.copy above — see PyCsv_Writer's comment in
        // Runtime.cpp for why .writerow() needs this).
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" && methodName == "writer") {
            const std::string& baseId = attr->children[0]->id;
            bool isCsvMod = (baseId == "csv");
            if (!isCsvMod) {
                auto it = moduleNameAliases.find(baseId);
                isCsvMod = (it != moduleNameAliases.end() && it->second == "csv");
            }
            if (isCsvMod) {
                std::string arg = args.empty() ? "" : args[0];
                ir.addInstruction(currentFunc, "call", {"PyCsv_Writer", arg}, res);
                noteType(res, "csvwriter");
                return res;
            }
        }
        // csvwriter.writerow(row) — typeOf-gated direct call, explicit
        // receiver (same lesson as file.write()'s fix: the generic,
        // non-bound dict dispatch can't supply one).
        if (methodName == "writerow" && typeOf(obj) == "csvwriter") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyCsv_Writerow", obj, arg}, res);
            return res;
        }
        // itertools.groupby(iterable, key=...) — needs AST-level
        // recognition (bypassing the generic dict-dispatch entirely,
        // hence PyItertools_Groupby's direct 2-raw-arg signature rather
        // than the usual token+registry boxed-args-list convention)
        // because the generic dict dispatch has no mechanism to read a
        // keyword argument through: `key=` would otherwise be silently
        // dropped, making every keyed groupby() call group by the whole
        // item instead (a real bug found while verifying against real
        // CPython — the keyless form worked, `key=lambda w: w[0]`
        // silently didn't).
        if (methodName == "groupby" && attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name") {
            const std::string& modId = attr->children[0]->id;
            bool isItertoolsMod = (modId == "itertools");
            if (!isItertoolsMod) {
                auto it = moduleNameAliases.find(modId);
                isItertoolsMod = (it != moduleNameAliases.end() && it->second == "itertools");
            }
            if (isItertoolsMod) {
                std::string iterableArg = args.empty() ? "" : args[0];
                std::string keyArg;
                for (size_t i = 1; i < node->children.size(); ++i) {
                    const auto* ch = node->children[i].get();
                    if (ch && ch->type == "Keyword" && ch->id == "key" && !ch->children.empty()) {
                        keyArg = lowerExpr(ch->children[0].get());
                        break;
                    }
                }
                if (keyArg.empty() && args.size() > 1) keyArg = args[1];
                ir.addInstruction(currentFunc, "call", {"PyItertools_Groupby", iterableArg, keyArg}, res, "list");
                noteType(res, "list");
                return res;
            }
        }
        // itertools.chain.from_iterable(...) — a two-level attribute
        // chain (attr's base is itself Attribute(Name("itertools"),
        // "chain")), the same shape as datetime.date.today() above.
        // Real itertools.chain.from_iterable is a classmethod on the
        // chain type; pyc's chain is just a plain function token, so
        // this needs the same dedicated structural recognition rather
        // than falling out of the generic dispatch naturally.
        if (methodName == "from_iterable" && attr->children.size() >= 1 && attr->children[0]) {
            const ASTNode* base = attr->children[0].get();
            if (base->type == "Attribute" && !base->children.empty() && base->children[0] &&
                base->children[0]->type == "Name" && base->id == "chain") {
                const std::string& modId = base->children[0]->id;
                bool isItertoolsMod = (modId == "itertools");
                if (!isItertoolsMod) {
                    auto it = moduleNameAliases.find(modId);
                    isItertoolsMod = (it != moduleNameAliases.end() && it->second == "itertools");
                }
                if (isItertoolsMod) {
                    std::string arg = args.empty() ? "" : args[0];
                    ir.addInstruction(currentFunc, "call", {"PyItertools_ChainFromIterable", arg}, res, "list");
                    noteType(res, "list");
                    return res;
                }
            }
        }
        // collections.deque(...) construction — literal "collections"
        // name or any `import collections as X` alias of it (same
        // structural-recognition rationale as pathlib.Path above — see
        // PyCollections_Deque's comment in Runtime.cpp).
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" && methodName == "deque") {
            const std::string& baseId = attr->children[0]->id;
            bool isCollectionsMod = (baseId == "collections");
            if (!isCollectionsMod) {
                auto it = moduleNameAliases.find(baseId);
                isCollectionsMod = (it != moduleNameAliases.end() && it->second == "collections");
            }
            if (isCollectionsMod) return lowerDequeConstruct(args);
        }
        // decimal.Decimal(...) construction — literal "decimal" name or
        // any `import decimal as X` alias of it (same structural-
        // recognition rationale as collections.deque above — see
        // PyDecimal_Construct's comment in Runtime.cpp).
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" && methodName == "Decimal") {
            const std::string& baseId = attr->children[0]->id;
            bool isDecimalMod = (baseId == "decimal");
            if (!isDecimalMod) {
                auto it = moduleNameAliases.find(baseId);
                isDecimalMod = (it != moduleNameAliases.end() && it->second == "decimal");
            }
            if (isDecimalMod) return lowerDecimalConstruct(args);
        }
        // Decimal.quantize(Decimal('0.01')) — typeOf-gated direct call
        // (rounding to N places, the single most common real-world
        // Decimal method — see PyDecimal_Quantize's comment in Runtime.cpp).
        if (methodName == "quantize" && typeOf(obj) == "decimal") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyDecimal_Quantize", obj, arg}, res);
            noteType(res, "decimal");
            return res;
        }
        // bytes.fromhex(s) — a classmethod-style call on the bare type
        // name (not an instance), same structural-recognition shape as
        // collections.deque(...) above (literal "bytes" name as the
        // Attribute's base).
        if (attr->children.size() >= 1 && attr->children[0] &&
            attr->children[0]->type == "Name" && attr->children[0]->id == "bytes" &&
            methodName == "fromhex") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyBytes_Fromhex", arg}, res);
            noteType(res, "bytes");
            return res;
        }
        // deque.appendleft(x)/.popleft()/.rotate(n) — typeOf-gated direct
        // calls (deque is a plain list at runtime with no dedicated type
        // tag — see PyCollections_Deque's comment in Runtime.cpp).
        if (methodName == "appendleft" && typeOf(obj) == "deque") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyDeque_Appendleft", obj, arg}, res);
            return res;
        }
        if (methodName == "popleft" && typeOf(obj) == "deque") {
            ir.addInstruction(currentFunc, "call", {"PyDeque_Popleft", obj}, res);
            return res;
        }
        if (methodName == "rotate" && typeOf(obj) == "deque") {
            std::string arg = args.empty() ? "" : args[0];
            if (arg.empty()) { arg = "$t" + std::to_string(tempCounter++); ir.addInstruction(currentFunc, "const", {"1"}, arg, "int"); }
            ir.addInstruction(currentFunc, "call", {"PyDeque_Rotate", obj, arg}, res);
            return res;
        }
        // hashobj.hexdigest() — typeOf-gated (same fast-path-only
        // limitation as datetime/pathlib's methods: works after
        // construction/assignment/return, not through an untyped function
        // parameter). Direct call, no Pyc_GetItem/Pyc_Apply — the hash
        // object dict has no "hexdigest" entry at all, sidestepping the
        // receiver-prepending pitfall found and fixed in open()/.write().
        if (methodName == "hexdigest" && typeOf(obj) == "hashobj") {
            ir.addInstruction(currentFunc, "call", {"PyHashlib_Hexdigest", obj}, res, "str");
            noteType(res, "str");
            return res;
        }
        // hashobj.digest() — the raw-bytes form, newly possible now that
        // pyc has a real bytes type (previously documented as missing).
        if (methodName == "digest" && typeOf(obj) == "hashobj") {
            ir.addInstruction(currentFunc, "call", {"PyHashlib_Digest", obj}, res);
            noteType(res, "bytes");
            return res;
        }
        // file.write(x) — pre-existing bug found while investigating this
        // phase's calling conventions: open()'s returned dict used to be
        // typed "dict", so .write() fell through to the generic
        // dict-attribute dispatch a few branches below, which does NOT
        // prepend the receiver to the callee's args list (that dispatch
        // is designed for module-namespace-style calls like
        // `os.path.exists(path)`, where the dict genuinely isn't a bound
        // receiver). pyc_file_write_adapter expects `args->list[0]` to be
        // the file object itself (to look up its FILE* in g_pycFiles) —
        // receiving the write data there instead means the lookup always
        // misses and the function returns immediately without writing
        // anything, while still reporting success. Confirmed empirically:
        // `open(p,"w") as f: f.write("hi")` created an empty file. Fixed
        // by typing open()'s result "file" (not "dict") and handling
        // .write() here with the receiver explicitly prepended, mirroring
        // how the with-statement's own __enter__/__exit__ dispatch
        // (Compiler.cpp's With-lowering) already builds its args list.
        if (typeOf(obj) == "file" && methodName == "write") {
            std::string methodNameConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"write\""}, methodNameConst, "str");
            std::string methodLookup = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, methodNameConst}, methodLookup);
            std::string writeCountConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"2"}, writeCountConst);
            std::string argList = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", writeCountConst}, argList);
            std::string idx0 = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idx0);
            std::string setRes0 = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idx0, obj}, setRes0);
            std::string idx1 = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, idx1);
            std::string arg0 = args.empty() ? "" : args[0];
            std::string setRes1 = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idx1, arg0}, setRes1);
            ir.addInstruction(currentFunc, "call", {"Pyc_Apply", methodLookup, argList}, res);
            return res;
        }
        // file.readlines() — direct call (unlike .write() above, this is
        // new code with no pre-existing token/dict-entry to reuse, so it
        // skips the Pyc_GetItem/Pyc_Apply indirection entirely, matching
        // the pathlib/hashlib method-dispatch pattern).
        if (typeOf(obj) == "file" && methodName == "readlines") {
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_FileReadlines", obj}, res, "list");
            noteType(res, "list");
            return res;
        }
        // Path.exists()/.is_file()/.is_dir()/.mkdir()/.joinpath(*parts) —
        // dispatch on typeOf(obj) == "path" (fast path after construction)
        // OR "boxed" (untyped function parameter). The runtime functions
        // (PyPathlib_Exists etc.) already type-check at runtime via
        // pyc_is_path_like and return safe defaults for non-Path values,
        // so emitting the call unconditionally for "boxed" values is safe
        // and fixes the silent-None bug for Path values passed as params.
        if (typeOf(obj) == "path" || typeOf(obj) == "boxed") {
            if (methodName == "exists" && typeOf(obj) == "path") {
                // Proven Path only. "boxed" here stole user-class
                // .exists() (direct and through a parameter); Path
                // values arriving as parameters fall through to
                // Pyc_CallBuiltinMethod (type 16).
                ir.addInstruction(currentFunc, "call", {"PyPathlib_Exists", obj}, res, "bool");
                noteType(res, "bool");
                return res;
            }
            if (methodName == "is_file") {
                ir.addInstruction(currentFunc, "call", {"PyPathlib_IsFile", obj}, res, "bool");
                noteType(res, "bool");
                return res;
            }
            if (methodName == "is_dir") {
                ir.addInstruction(currentFunc, "call", {"PyPathlib_IsDir", obj}, res, "bool");
                noteType(res, "bool");
                return res;
            }
            if (methodName == "mkdir") {
                ir.addInstruction(currentFunc, "call", {"PyPathlib_Mkdir", obj}, res);
                return res;
            }
            if (methodName == "joinpath") {
                std::string partsList = "$t" + std::to_string(tempCounter++);
                std::string countConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {std::to_string(args.size())}, countConst);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", countConst}, partsList);
                for (size_t i = 0; i < args.size(); ++i) {
                    std::string idxConst = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idxConst);
                    std::string setRes = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", partsList, idxConst, args[i]}, setRes);
                }
                ir.addInstruction(currentFunc, "call", {"PyPathlib_Joinpath", obj, partsList}, res);
                noteType(res, "path");
                return res;
            }
        }
        // .isoformat()/.weekday()/.isoweekday()/.total_seconds() — dispatch
        // when typeOf(obj) is date/datetime/timedelta (fast path after
        // construction) OR "boxed" (untyped function parameter). The runtime
        // functions type-check via pyc_as_datetime/pyc_as_timedelta and return
        // safe defaults for non-matching types, so this is safe.
        if (methodName == "isoformat" && (typeOf(obj) == "date" || typeOf(obj) == "datetime" || typeOf(obj) == "boxed")) {
            ir.addInstruction(currentFunc, "call", {"PyDateTime_Isoformat", obj}, res, "str");
            noteType(res, "str");
            return res;
        }
        if (methodName == "weekday" && (typeOf(obj) == "date" || typeOf(obj) == "datetime" || typeOf(obj) == "boxed")) {
            ir.addInstruction(currentFunc, "call", {"PyDateTime_Weekday", obj}, res, "int");
            noteType(res, "int");
            return res;
        }
        if (methodName == "isoweekday" && (typeOf(obj) == "date" || typeOf(obj) == "datetime" || typeOf(obj) == "boxed")) {
            ir.addInstruction(currentFunc, "call", {"PyDateTime_Isoweekday", obj}, res, "int");
            noteType(res, "int");
            return res;
        }
        if (methodName == "total_seconds" && (typeOf(obj) == "timedelta" || typeOf(obj) == "boxed")) {
            ir.addInstruction(currentFunc, "call", {"PyTimedelta_TotalSeconds", obj}, res, "float");
            noteType(res, "float");
            return res;
        }

        // Match.group(i) — dispatch when typeOf(obj) is "match" (fast path)
        // OR "boxed" (untyped function parameter). PyBuiltin_ReMatchGroup
        // type-checks via asMatchObj and returns None for non-Match values.
        if (methodName == "group" && (typeOf(obj) == "match" || typeOf(obj) == "boxed")) {
            std::string i = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_ReMatchGroup", obj, i}, res);
            return res;
        }

        // List methods (count must come before the string `count` case so
        // that `a.count(x)` for a list dispatches to PyList_Count).
        // Dispatch chain step 5: the (name, receiver kind) table handles
        // every method whose emission is a plain direct call. Consulted
        // here -- after the structural / module-qualified arms above, and
        // before the remaining name-only arms -- so a proven receiver
        // takes its own row and nothing else can claim it.
        if (tryBuiltinMethodTable(methodName, obj, args, res)) {
            return res;
        }

        // bytearray.append(int)/.extend(iterable) — must come before the
        // unconditional list .append()/.extend() branches below, since
        // bytearray (type 18) stores content in `str`, not `list`/`ilist`/
        // `flist`; PyList_Append would corrupt it.
        if (methodName == "append" && typeOf(obj) == "bytearray") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyByteArray_Append", obj, arg}, res);
        } else if (methodName == "extend" && typeOf(obj) == "bytearray") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyByteArray_ExtendOp", obj, arg}, res);
        } else if (methodName == "hex" && (typeOf(obj) == "bytes" || typeOf(obj) == "bytearray")) {
            ir.addInstruction(currentFunc, "call", {"PyBytes_Hex", obj}, res, "str");
            noteType(res, "str");
        } else if (methodName == "decode" && (typeOf(obj) == "bytes" || typeOf(obj) == "bytearray")) {
            std::string enc = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyBytes_Decode", obj, enc}, res, "str");
            noteType(res, "str");
        } else if (methodName == "encode" && typeOf(obj) == "str") {
            std::string enc = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyStr_Encode", obj, enc}, res);
            noteType(res, "bytes");
        // Known list methods
        // Known string methods
        } else if (methodName == "is_integer" && typeOf(obj) == "float") {
            ir.addInstruction(currentFunc, "call", {"PyFloat_IsInteger", obj}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "is_integer" && typeOf(obj) == "boxed") {
            ir.addInstruction(currentFunc, "call", {"PyFloat_IsInteger", obj}, res, "bool");
            noteType(res, "bool");
        } else if ((methodName == "split" || methodName == "rsplit") &&
                   (isProvenStr(obj) || hasKeywordArgs)) {
            // sep=None (the whitespace-run-splitting mode, collapsing
            // consecutive whitespace and dropping empty tokens) must be
            // detected from the AST, not just "no argument given" — a
            // caller can also pass None *explicitly* as a positional
            // argument (`s.split(None)`, `s.rsplit(None, 1)`), which
            // args.empty() alone doesn't catch. Found and fixed while
            // bug hunting (rsplit's implementation, alongside split's
            // pre-existing version of the same gap): without this check,
            // an explicit None fell through to the literal-separator
            // path with sep coerced to a plain single space, producing
            // spurious empty-string elements for any run of more than
            // one whitespace character.
            bool sepIsNone = false;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type != "Keyword") { sepIsNone = (ch->type == "Constant" && ch->is_none); break; }
            }
            if (methodName == "split") {
                // Extract maxsplit (positional arg 2 or maxsplit= keyword).
                std::string maxsplitArg;
                for (size_t i = 1; i < node->children.size(); ++i) {
                    const auto* ch = node->children[i].get();
                    if (ch && ch->type == "Keyword" && ch->id == "maxsplit" && !ch->children.empty()) {
                        maxsplitArg = lowerExpr(ch->children[0].get());
                        break;
                    }
                }
                if (maxsplitArg.empty() && args.size() > 1) maxsplitArg = args[1];
                if (args.empty() || sepIsNone) {
                    if (maxsplitArg.empty()) {
                        ir.addInstruction(currentFunc, "call", {"PyString_SplitWhitespace", obj}, res);
                    } else {
                        // split(None, maxsplit) — whitespace mode with a limit.
                        // Not commonly used; delegate to RSplitWhitespace which
                        // handles maxsplit (the result order differs only for
                        // the exact boundary, which whitespace mode makes rare).
                        ir.addInstruction(currentFunc, "call", {"PyString_RSplitWhitespace", obj, maxsplitArg}, res);
                    }
                } else {
                    if (maxsplitArg.empty()) {
                        ir.addInstruction(currentFunc, "call", {"PyString_Split", obj, args[0]}, res);
                    } else {
                        ir.addInstruction(currentFunc, "call", {"PyString_Split2", obj, args[0], maxsplitArg}, res);
                    }
                }
            } else {
                // rsplit(sep=None, maxsplit=-1) — found entirely
                // unimplemented while bug hunting. maxsplit may be
                // positional or a maxsplit= keyword (scanned the same
                // way sub()'s count= keyword already is elsewhere in
                // this file).
                std::string maxsplitArg;
                for (size_t i = 1; i < node->children.size(); ++i) {
                    const auto* ch = node->children[i].get();
                    if (ch && ch->type == "Keyword" && ch->id == "maxsplit" && !ch->children.empty()) {
                        maxsplitArg = lowerExpr(ch->children[0].get());
                        break;
                    }
                }
                if (maxsplitArg.empty() && args.size() > 1) maxsplitArg = args[1];
                if (maxsplitArg.empty()) {
                    maxsplitArg = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"-1"}, maxsplitArg, "int");
                }
                if (args.empty() || sepIsNone) {
                    ir.addInstruction(currentFunc, "call", {"PyString_RSplitWhitespace", obj, maxsplitArg}, res);
                } else {
                    ir.addInstruction(currentFunc, "call", {"PyString_RSplit", obj, args[0], maxsplitArg}, res);
                }
            }
        } else if (methodName == "format" && (isProvenStr(obj) || hasKeywordArgs)) {
            // str.format(*args, **kwargs) — found entirely unimplemented
            // while bug hunting. `args` here already has only positional
            // arguments (Keyword children are filtered out at the top of
            // this function); scan node->children again to build the
            // kwargs dict PyBuiltin_StrFormat also needs.
            std::string argsListVar = "$t" + std::to_string(tempCounter++);
            {
                std::string z = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, z);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, argsListVar);
            }
            for (auto& a : args) {
                std::string d = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_Append", argsListVar, a}, d);
            }
            std::string kwargsDictVar = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyDict_New"}, kwargsDictVar);
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (ch && ch->type == "Keyword" && !ch->id.empty() && !ch->children.empty()) {
                    std::string keyConst = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"\"" + ch->id + "\""}, keyConst, "str");
                    std::string val = lowerExpr(ch->children[0].get());
                    std::string dummy = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyDict_SetItem", kwargsDictVar, keyConst, val}, dummy);
                }
            }
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_StrFormat", obj, argsListVar, kwargsDictVar}, res);
        } else if (methodName == "find" && isProvenStr(obj)) {
            if (args.size() >= 2) {
                ir.addInstruction(currentFunc, "call", {"PyString_Find3", obj, args[0], args[1]}, res, "int");
            } else {
                std::string arg = args.empty() ? "" : args[0];
                ir.addInstruction(currentFunc, "call", {"PyString_Find", obj, arg}, res, "int");
            }
            noteType(res, "int");
        } else if (methodName == "rfind" && isProvenStr(obj)) {
            if (args.size() >= 3) {
                ir.addInstruction(currentFunc, "call", {"PyString_RFind4", obj, args[0], args[1], args[2]}, res, "int");
            } else if (args.size() == 2) {
                ir.addInstruction(currentFunc, "call", {"PyString_RFind3", obj, args[0], args[1]}, res, "int");
            } else {
                std::string arg = args.empty() ? "" : args[0];
                ir.addInstruction(currentFunc, "call", {"PyString_RFind", obj, arg}, res, "int");
            }
            noteType(res, "int");
        } else if (methodName == "replace" && isProvenStr(obj)) {
            std::string a = args.size() > 0 ? args[0] : "";
            std::string b = args.size() > 1 ? args[1] : "";
            if (args.size() >= 3) {
                ir.addInstruction(currentFunc, "call", {"PyString_ReplaceN", obj, a, b, args[2]}, res);
            } else {
                ir.addInstruction(currentFunc, "call", {"PyString_Replace", obj, a, b}, res);
            }
            noteType(res, "str");
        // Set methods (gated on typeOf == "set" so they don't shadow
        // dict/list method names like pop/copy/update/clear).
        // Dict methods
        } else if (methodName == "get" && (typeOf(obj) == "dict" || typeOf(obj) == "boxed")) {
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
        } else if (methodName == "values" && typeOf(obj) == "dict") {
            ir.addInstruction(currentFunc, "call", {"PyDict_Values", obj}, res);
            noteType(res, "list");
            std::string valueType = dictValueTypes[obj];
            if (!valueType.empty()) {
                tempContainerElementTypes[res] = valueType;
                noteType(res, "list_values_typed");
            }
            noteType(res, "list");
            // P0: values() of a dict of structured bodies → list with that element layout
            auto dlit = dictValueLayouts.find(obj);
            if (dlit != dictValueLayouts.end() && !dlit->second.empty()) {
                markStructuredList(res, dlit->second);
            }
        } else if (methodName == "update" && typeOf(obj) == "dict") {
            std::string arg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"PyDict_Update", obj, arg}, res);
        } else if (methodName == "pop" && typeOf(obj) == "dict") {
            std::string key = args.size() > 0 ? args[0] : "";
            std::string defv = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyDict_Pop", obj, key, defv}, res);
        } else if (methodName == "fromkeys" &&
                   (typeOf(obj) == "dict" ||
                    (attr->children.size() >= 1 && attr->children[0] &&
                     attr->children[0]->type == "Name" &&
                     attr->children[0]->id == "dict"))) {
            // Classmethod: no self. Proven dict, or the AST `dict` type
            // name (`dict.fromkeys(...)`). Do not accept "boxed".
            std::string keys = args.size() > 0 ? args[0] : "";
            std::string defv = args.size() > 1 ? args[1] : "";
            ir.addInstruction(currentFunc, "call", {"PyDict_FromKeys", keys, defv}, res);
        // os.path stub methods — gate on AST `os.path.<name>` (or an
        // `import os as X` alias). The earlier pathlib exists/is_file/
        // is_dir block is a different name set / enclosing typeOf.
        } else if (methodName == "exists" &&
                   attr->children.size() >= 1 && attr->children[0] &&
                   attr->children[0]->type == "Attribute" &&
                   attr->children[0]->id == "path" &&
                   !attr->children[0]->children.empty() &&
                   attr->children[0]->children[0] &&
                   attr->children[0]->children[0]->type == "Name" &&
                   (attr->children[0]->children[0]->id == "os" ||
                    (moduleNameAliases.find(attr->children[0]->children[0]->id) !=
                         moduleNameAliases.end() &&
                     moduleNameAliases.find(attr->children[0]->children[0]->id)->second ==
                         "os"))) {
            std::string pathArg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"Pyc_OsPathExists", pathArg}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "isfile" &&
                   attr->children.size() >= 1 && attr->children[0] &&
                   attr->children[0]->type == "Attribute" &&
                   attr->children[0]->id == "path" &&
                   !attr->children[0]->children.empty() &&
                   attr->children[0]->children[0] &&
                   attr->children[0]->children[0]->type == "Name" &&
                   (attr->children[0]->children[0]->id == "os" ||
                    (moduleNameAliases.find(attr->children[0]->children[0]->id) !=
                         moduleNameAliases.end() &&
                     moduleNameAliases.find(attr->children[0]->children[0]->id)->second ==
                         "os"))) {
            std::string pathArg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"Pyc_OsPathIsFile", pathArg}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "isdir" &&
                   attr->children.size() >= 1 && attr->children[0] &&
                   attr->children[0]->type == "Attribute" &&
                   attr->children[0]->id == "path" &&
                   !attr->children[0]->children.empty() &&
                   attr->children[0]->children[0] &&
                   attr->children[0]->children[0]->type == "Name" &&
                   (attr->children[0]->children[0]->id == "os" ||
                    (moduleNameAliases.find(attr->children[0]->children[0]->id) !=
                         moduleNameAliases.end() &&
                     moduleNameAliases.find(attr->children[0]->children[0]->id)->second ==
                         "os"))) {
            std::string pathArg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"Pyc_OsPathIsDir", pathArg}, res, "bool");
            noteType(res, "bool");
        } else if (methodName == "unlink" &&
                   attr->children.size() >= 1 && attr->children[0] &&
                   attr->children[0]->type == "Name" &&
                   (attr->children[0]->id == "os" ||
                    (moduleNameAliases.find(attr->children[0]->id) !=
                         moduleNameAliases.end() &&
                     moduleNameAliases.find(attr->children[0]->id)->second == "os"))) {
            std::string pathArg = args.empty() ? "" : args[0];
            ir.addInstruction(currentFunc, "call", {"Pyc_OsUnlink", pathArg}, res, "int");
            noteType(res, "int");
        // subprocess stub methods
        } else if (methodName == "call" && !args.empty() &&
                   attr->children.size() >= 1 && attr->children[0] &&
                   attr->children[0]->type == "Name" &&
                   (attr->children[0]->id == "subprocess" ||
                    (moduleNameAliases.find(attr->children[0]->id) !=
                         moduleNameAliases.end() &&
                     moduleNameAliases.find(attr->children[0]->id)->second ==
                         "subprocess"))) {
            // subprocess.call(cmd) -> exit status (<< 8)
            ir.addInstruction(currentFunc, "call", {"Pyc_SubprocessCall", args[0]}, res, "int");
            noteType(res, "int");
        } else if (methodName == "check_output" && !args.empty() &&
                   attr->children.size() >= 1 && attr->children[0] &&
                   attr->children[0]->type == "Name" &&
                   (attr->children[0]->id == "subprocess" ||
                    (moduleNameAliases.find(attr->children[0]->id) !=
                         moduleNameAliases.end() &&
                     moduleNameAliases.find(attr->children[0]->id)->second ==
                         "subprocess"))) {
            // subprocess.check_output(cmd) -> stdout as string
            ir.addInstruction(currentFunc, "call", {"Pyc_SubprocessCheckOutput", args[0]}, res, "str");
            noteType(res, "str");
        // List methods
        } else if (methodName == "sort" && (isProvenListLike(obj) || hasKeywordArgs)) {
            // key=/reverse= — found completely unimplemented (silently
            // ignored) while hunting for more bugs; extracted the same
            // way other keyword-only args are pulled from the raw AST
            // elsewhere in this function (e.g. groupby's key=).
            std::string keyArg, reverseArg;
            for (size_t i = 1; i < node->children.size(); ++i) {
                const auto* ch = node->children[i].get();
                if (!ch || ch->type != "Keyword" || ch->children.empty()) continue;
                if (ch->id == "key") keyArg = lowerExpr(ch->children[0].get());
                else if (ch->id == "reverse") reverseArg = lowerExpr(ch->children[0].get());
            }
            ir.addInstruction(currentFunc, "call", {"PyList_Sort", obj, keyArg, reverseArg}, res);
        } else if (methodName == "pop" && (typeOf(obj) == "list" || typeOf(obj) == "list_int" || typeOf(obj) == "list_float" || typeOf(obj) == "deque")) {
            if (args.empty()) {
                ir.addInstruction(currentFunc, "call", {"PyList_Pop", obj}, res);
            } else {
                std::string idx = args[0];
                ir.addInstruction(currentFunc, "call", {"PyList_PopAt", obj, idx}, res);
            }
        // String method fallbacks (some keys like "find",
        // "replace", "split" are common to both list and string; the
        // list-specific cases above win for lists).
        } else if (attr->children[0] && attr->children[0]->type == "Name" &&
                   knownClasses.count(attr->children[0]->id)) {
            // ClassName.method(...) — a class-level ("unbound") method
            // call. Deliberately not gated by isShadowedLocal(): every
            // class name gets noteType(className, "dict") called at its
            // own definition site, so isShadowedLocal (which treats any
            // valueTypes entry as "shadowed") would always be true for a
            // class name and defeat this branch entirely — found while
            // debugging this exact fix. A local variable that happens to
            // share a class's name is a narrower, pre-existing ambiguity
            // the old "dict"-typeOf fallback below never resolved either
            // (it dispatches on typeOf alone, with the same blind spot),
            // so this isn't a new gap.
            // Must be checked before the generic "dict"-typeOf
            // branch just below: a class reference's typeOf is also
            // noted "dict" (lowerClass tags it that way for module-style
            // dispatch reuse) and would otherwise fall into that branch,
            // which has no notion of @classmethod/@staticmethod at all.
            // Routed through the same Pyc_CallMethod dispatch used for
            // obj.method(...) below — see its comment in Runtime.cpp;
            // Pyc_CallMethod itself distinguishes "receiver is a class"
            // (this call shape) from "receiver is an instance" to decide
            // whether a plain (undecorated) method needs self prepended.
            std::string methodNameConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + methodName + "\""}, methodNameConst, "str");
            std::string methodLookup = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, methodNameConst}, methodLookup);
            std::string argList = "$t" + std::to_string(tempCounter++);
            {
                std::string z = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, z);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, argList);
            }
            for (auto& a : args) {
                std::string d = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, a}, d);
            }
            ir.addInstruction(currentFunc, "call", {"Pyc_CallMethod", methodLookup, obj, argList}, res);
            callableTokenTemps.insert(res);
            return res;
        } else {
            // Counter.most_common([n]) — Counter is a plain dict at runtime.
            // Route to PyCollections_MostCommon with [counter, n] as args.
            // Only fire when the object is NOT the collections module
            // (collections.most_common(c) is a function call handled by
            // the generic dict dispatch below).
            bool isCollectionsModule = false;
            if (methodName == "most_common" && attr->children.size() >= 1 && attr->children[0] &&
                attr->children[0]->type == "Name" && attr->children[0]->id == "collections") {
                isCollectionsModule = true;
            }
            if (methodName == "most_common" && (typeOf(obj) == "dict" || typeOf(obj) == "boxed") && !isCollectionsModule) {
                std::string zero = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, zero);
                std::string argList = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, argList);
                std::string d1 = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, obj}, d1);
                for (auto& a : args) {
                    std::string d = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, a}, d);
                }
                ir.addInstruction(currentFunc, "call", {"PyCollections_MostCommon", argList}, res);
                noteType(res, "list");
                return res;
            }
            // Counter.elements() — returns an iterator over elements.
            if (methodName == "elements" && (typeOf(obj) == "dict" || typeOf(obj) == "boxed") && !isCollectionsModule) {
                ir.addInstruction(currentFunc, "call", {"PyCollections_Elements", obj}, res);
                noteType(res, "list");
                return res;
            }
            // Counter.subtract(other) — mutates the counter in place,
            // returns None. Counter.update() is deliberately absent: the
            // untyped `.update()` arm earlier in this chain resolves
            // first, so anything added here would be unreachable. That
            // arm's PyDict_Update is Counter-aware at runtime instead.
            if (methodName == "subtract" &&
                (typeOf(obj) == "dict" || typeOf(obj) == "boxed") && !isCollectionsModule) {
                std::string zero = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, zero);
                std::string argList = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", zero}, argList);
                std::string d1 = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, obj}, d1);
                for (auto& a : args) {
                    std::string d = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, a}, d);
                }
                ir.addInstruction(currentFunc, "call", {"PyCollections_Subtract", argList}, res);
                return res;
            }
            // Chained module attribute call: `mod.path.func(args)`. The
            // dict-path branch handles the simple case (obj is a dict,
            // e.g. sys.stderr = {"write": "pyc_stderr_write"}). The
            // str-path branch handles the case where chained attribute
            // access already resolved to a string token (e.g.
            // os.path.exists resolves to "PyBuiltin_OsPathExists" via
            // two Pyc_GetItem calls; here the obj's typeOf is "dict"
            // from lowerAttribute, but at runtime the value is a str).
            if (typeOf(obj) == "dict" || typeOf(obj) == "str") {
                std::string methodNameConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"\"" + methodName + "\""}, methodNameConst, "str");
                std::string methodLookup = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, methodNameConst}, methodLookup);
                std::vector<std::string> methodArgs;
                for (auto& a : args) {
                    methodArgs.push_back(a);
                }
                std::string argList = "$t" + std::to_string(tempCounter++);
                std::string argCount = std::to_string(methodArgs.size());
                std::string argCountConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {argCount}, argCountConst);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", argCountConst}, argList);
                for (size_t i = 0; i < methodArgs.size(); ++i) {
                    std::string idxConst = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {std::to_string(i)}, idxConst);
                    std::string setRes = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_SetItemBoxed", argList, idxConst, methodArgs[i]}, setRes);
                }
                ir.addInstruction(currentFunc, "call", {"Pyc_Apply", methodLookup, argList}, res);
                callableTokenTemps.insert(res);
                return res;
            }
            // Try to call as user-defined method
            // For class instances: look up method on class dict
            std::string methodLookup = "$t" + std::to_string(tempCounter++);
            std::string methodNameConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"" + methodName + "\""}, methodNameConst, "str");
            // Get __class__ from instance, then look up method on class dict
            std::string classKeyConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"__class__\""}, classKeyConst, "str");
            std::string classRef = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", obj, classKeyConst}, classRef);
            ir.addInstruction(currentFunc, "call", {"Pyc_GetItem", classRef, methodNameConst}, methodLookup);
            // Build the args list (NOT including self/cls — Pyc_CallMethod
            // below decides whether/what to prepend based on the looked-up
            // method's shape, replacing what used to be an unconditional
            // "prepend obj" here regardless of @staticmethod/@classmethod —
            // see Pyc_CallMethod's comment in Runtime.cpp).
            std::string argList = "$t" + std::to_string(tempCounter++);
            {
                std::string z = "$c" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "const", {"0"}, z);
                ir.addInstruction(currentFunc, "call", {"PyList_NewBoxed", z}, argList);
            }
            for (auto& a : args) {
                std::string d = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_Append", argList, a}, d);
            }
            // Pyc_CallMethodOrBuiltin, not Pyc_CallMethod: methodLookup is
            // null whenever the receiver is not a user-class instance (a
            // builtin has no "__class__" entry), and that used to fall
            // straight through to None. Passing methodName along lets the
            // runtime dispatch on the receiver's real type tag instead --
            // which is what makes a type-gated arm that did not fire (a
            // "boxed" receiver, i.e. any function parameter) still reach
            // the right implementation. Class instances are unaffected:
            // a non-null methodLookup takes the identical path as before.
            ir.addInstruction(currentFunc, "call",
                              {"Pyc_CallMethodOrBuiltin", methodLookup, obj, argList, methodNameConst}, res);
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
        std::string classDictTemp = "$c" + std::to_string(tempCounter++);
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
                // Generate __init__ function with correct params
                std::string initFuncName = className + "__init__";
                // Register defaults for this __init__ so the call site (A() /
                // A(x)) can inject trailing defaults when the user omits args.
                // Mirrors the default-handling block in the FunctionDef
                // lowering (see "Count defaults and collect their values").
                //
                // Real bug found and fixed while bug hunting: the default
                // slot globals used to be named the literal
                // "__default___init__<i>" and funcDefaultValues/
                // funcDefaultCount were keyed by the bare literal
                // "__init__" — shared across EVERY class in the module
                // instead of being per-class. With two or more classes each
                // defining an __init__ with a default at the same
                // positional index, they all pointed at the SAME global
                // storage slot, so whichever class's default assignment ran
                // last at module-init time silently clobbered every earlier
                // class's default value (confirmed: `class A: def __init__
                // (self, n=1)` / `class B: def __init__(self, n=2)` then
                // `A().n` incorrectly printed 2, not 1). It also meant
                // IRFunction::defaultGlobals was never populated for any
                // __init__, so Codegen.cpp's indirect-call adapter (used by
                // super().__init__(), a stored bound-method reference, or
                // dynamically calling a class held in a variable) had no
                // way to find the default value at all and silently passed
                // a null argument instead. Fixed by keying everything
                // (slot names, funcDefaultCount/funcDefaultValues, and the
                // IRFunction's defaultGlobals) by the per-class initFuncName
                // instead of the shared literal "__init__".
                std::vector<std::string> initDefaults;
                {
                    size_t defaultIndex = 0;
                    for (const auto& cc : c->children) {
                        if (cc && cc->type == "Default") {
                            std::string defVal = lowerExpr(cc.get());
                            std::string slot = "__default_" + initFuncName + "_" + std::to_string(defaultIndex++);
                            ir.addModuleGlobal(slot);
                            ir.addInstruction("__module__", "assign", {defVal}, slot);
                            initDefaults.push_back(slot);
                        }
                    }
                    if (!initDefaults.empty()) {
                        funcDefaultCount[initFuncName] = initDefaults.size();
                        funcDefaultValues[initFuncName] = initDefaults;
                    }
                }
                std::vector<std::string> initFuncParams;
                std::stringstream ss(initParams);
                std::string param;
                while (std::getline(ss, param, ',')) {
                    initFuncParams.push_back(param);
                }
                ir.addFunction(initFuncName, initFuncParams);
                if (!initDefaults.empty()) {
                    for (auto& fnr : ir.functions) {
                        if (fnr.name == initFuncName) { fnr.defaultGlobals = initDefaults; break; }
                    }
                }
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
                std::string methodConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + methodName + "\""}, methodConst, "str");
                std::string methodToken = "$c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + initFuncName + "\""}, methodToken, "str");
                knownIRFunctions.insert(initFuncName);
                // Store the function name string in the class dict
                std::string dummy = "$t" + std::to_string(tempCounter++);
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
                // @staticmethod/@classmethod/@property — found and fixed
                // while bug hunting: these decorators used to be
                // silently discarded entirely (this loop only ever
                // skipped Decorator children when lowering the body,
                // never inspecting them). Detected here and, if
                // recognized, the class-dict entry below is tagged as a
                // 2-element [kind, realToken] list instead of a bare
                // token string; Pyc_CallMethod/Pyc_GetAttr (Runtime.cpp)
                // recognize that shape and dispatch accordingly. Any
                // other decorator on a method remains unsupported (still
                // silently discarded, unchanged from before).
                std::string decoKind;
                for (const auto& cc : c->children) {
                    if (cc && cc->type == "Decorator" && !cc->children.empty()) {
                        const ASTNode* d = cc->children[0].get();
                        if (d && d->type == "Name" &&
                            (d->id == "staticmethod" || d->id == "classmethod" || d->id == "property")) {
                            decoKind = d->id;
                            break;
                        }
                    }
                }
                for (size_t i = 0; i < c->children.size(); ++i) {
                    if (c->children[i] && c->children[i]->type != "Default" && c->children[i]->type != "Decorator") {
                        lower(c->children[i].get());
                    }
                }
                // Store method in class dict as a callable token
                std::string methodConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + methodName + "\""}, methodConst, "str");
                std::string methodToken = "$c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + methodFuncName + "\""}, methodToken, "str");
                knownIRFunctions.insert(methodFuncName);
                std::string dummy = "$t" + std::to_string(tempCounter++);
                if (!decoKind.empty()) {
                    std::string kindConst = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction("__module__", "const", {"\"" + decoKind + "\""}, kindConst, "str");
                    std::string tagged = "$t" + std::to_string(tempCounter++);
                    std::string zc = "$c" + std::to_string(tempCounter++);
                    ir.addInstruction("__module__", "const", {"0"}, zc);
                    ir.addInstruction("__module__", "call", {"PyList_NewBoxed", zc}, tagged);
                    std::string a1 = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction("__module__", "call", {"PyList_Append", tagged, kindConst}, a1);
                    std::string a2 = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction("__module__", "call", {"PyList_Append", tagged, methodToken}, a2);
                    ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, methodConst, tagged}, dummy);
                } else {
                    ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, methodConst, methodToken}, dummy);
                }
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
             std::string attrKeyConst = "$c" + std::to_string(tempCounter++);
             ir.addInstruction("__module__", "const", {"\"" + attrName + "\""}, attrKeyConst, "str");
             std::string dummy = "$t" + std::to_string(tempCounter++);
             ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, attrKeyConst, attrValue}, dummy);
         }
         currentClass = savedClass;
        // B6b: Store MRO in class dict for runtime super() support
        const auto& mro = classMRO[className];
        if (!mro.empty()) {
            std::string mroKeyConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "const", {"\"__mro__\""}, mroKeyConst, "str");
            std::string mroList = "$c" + std::to_string(tempCounter++);
            std::string zeroConst = "$c" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "const", {"0"}, zeroConst);
            ir.addInstruction("__module__", "call", {"PyList_NewBoxed", zeroConst}, mroList);
            for (const auto& classNameInMRO : mro) {
                std::string classNameConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + classNameInMRO + "\""}, classNameConst, "str");
                std::string appendRes = "$t" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "call", {"PyList_Append", mroList, classNameConst}, appendRes);
            }
            std::string mroDummy = "$t" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "call", {"Pyc_SetItem", classDictTemp, mroKeyConst, mroList}, mroDummy);
        }
        // Decorators, bottom-up: className = decoN(...(deco1(classDict))...).
        // Each application: lower the decorator expression, wrap classDictTemp
        // in a one-element list, call Pyc_Apply, and update classDictTemp.
        for (auto it = decorators.rbegin(); it != decorators.rend(); ++it) {
            std::string dv = lowerExpr(*it);
            std::string z = "$c" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "const", {"0"}, z);
            std::string argList = "$t" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "call", {"PyList_NewBoxed", z}, argList);
            ir.addInstruction("__module__", "call", {"PyList_Append", argList, classDictTemp}, "");
            classDictTemp = "$t" + std::to_string(tempCounter++);
            ir.addInstruction("__module__", "call", {"Pyc_Apply", dv, argList}, classDictTemp);
        }
        // Store class dict as the class value
        ir.addInstruction("__module__", "assign", {classDictTemp}, className);
        noteType(className, "dict");
        // B6b: register the class in the runtime registry so super() can
        // resolve the class-name strings stored in __mro__ to class dicts.
        {
            std::string regNameConst = "$c" + std::to_string(tempCounter++);
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
                std::string dummy = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(initName, "call", callArgs, dummy);
                ir.addInstruction(initName, "ret", {"self"});
                currentFunc = savedFunc;
                // Store the __init__ wrapper name in the class dict (overrides base __init__)
                std::string initKeyConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"__init__\""}, initKeyConst, "str");
                std::string initValConst = "$c" + std::to_string(tempCounter++);
                ir.addInstruction("__module__", "const", {"\"" + initName + "\""}, initValConst, "str");
                std::string initSet = "$t" + std::to_string(tempCounter++);
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
        std::string initVal = "$c" + std::to_string(tempCounter++);
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
            std::string truthVal = "$t" + std::to_string(tempCounter++);
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

        std::string res = "$t" + std::to_string(tempCounter++);
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
        // !r / !s / !a conversion — found and fixed while bug hunting
        // (previously captured by the parser into node->op but never
        // read here, so f"{x!r}" and f"{x}" produced identical output).
        // !s and the no-conversion default are treated identically
        // (both just pass the value through to Pyc_FormatValue, which
        // dispatches on the value's runtime type) since pyc has no
        // __format__ protocol for the two to meaningfully diverge on;
        // !a (ascii) is approximated as repr — pyc's str values are
        // already assumed ASCII/UTF-8 passthrough, so there's no
        // separate non-ASCII-escaping behavior to implement.
        int conv = -1;
        try { conv = node->op.empty() ? -1 : std::stoi(node->op); } catch (...) {}
        std::string convertedVal = exprVal;
        if (conv == 114 || conv == 97) { // ord('r'), ord('a')
            std::string r = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_Repr", exprVal}, r);
            convertedVal = r;
        }
        // format_spec (e.g. :.2f) — found and fixed while bug hunting
        // (previously a deliberate MVP-era scope cut, silently ignored;
        // see the parser's comment on why format_spec is captured as a
        // full nested JoinedStr subtree rather than assumed to always be
        // a static literal). Lowered like any other f-string part when
        // present, producing a plain runtime string; Pyc_FormatValue
        // implements Python's Format Specification Mini-Language.
        std::string specVal;
        if (node->children.size() > 1 && node->children[1]) {
            specVal = lowerExpr(node->children[1].get());
        } else {
            specVal = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"\""}, specVal, "str");
        }
        std::string res = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"Pyc_FormatValue", convertedVal, specVal}, res);
        return res;
    }

    std::string lowerJoinedStr(const ASTNode* node) {
        if (node->children.empty()) {
            std::string res = "$c" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"\"\""}, res);
            return res;
        }
        std::string acc = lowerFStrPart(node->children[0].get());
        for (size_t i = 1; i < node->children.size(); ++i) {
            std::string part = lowerFStrPart(node->children[i].get());
            std::string newAcc = "$t" + std::to_string(tempCounter++);
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
        std::string sc = "$c" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "const", {"0"}, sc);
        std::string listVar = "$t" + std::to_string(tempCounter++);
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
            const ASTNode* targetNode;
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
            g.targetNode = genNode->children[0].get();
            for (size_t ci = 2; ci < genNode->children.size(); ++ci) {
                g.conds.push_back(genNode->children[ci].get());
            }
            gens.push_back(g);
        }
        if (gens.empty()) {
            // No usable generator: the comprehension is empty.
            return listSlot;
        }

        // Evaluate gen[0]'s iterator up front (CPython evaluates the
        // outermost iterable once at comprehension start). Nested
        // generators' iterables are evaluated lazily inside the parent's
        // body, because they may reference the outer loop variable
        // (e.g. [... for row in a for x in [(row[0], row[1])]]).
        std::vector<std::string> iterSlots(gens.size());
        std::vector<std::string> iterSrcs(gens.size());
        std::vector<std::string> lenSlots(gens.size());
        {
            const ASTNode* genNode = node->children[1].get();
            std::string iterVal = lowerExpr(genNode->children[1].get());
            iterSrcs[0] = iterVal;
            std::string iterSlot = "__lci_0_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {iterVal}, iterSlot);
            copyLayoutMaps(iterVal, iterSlot);
            std::string listified = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", iterSlot}, listified);
            copyLayoutMaps(iterSlot, listified);
            std::string listifiedSlot = "__lcmat_0_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {listified}, listifiedSlot);
            copyLayoutMaps(listified, listifiedSlot);
            iterSlots[0] = listifiedSlot;
        }

        // Emit each generator's index / loop / body / cont / exit blocks.
        std::vector<std::string> idxVars;
        idxVars.reserve(gens.size());
        for (size_t gi = 0; gi < gens.size(); ++gi) {
            std::string idxVar  = "lc_i_" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            std::string idxInit = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxInit);
            ir.addInstruction(currentFunc, "assign", {idxInit}, idxVar);
            idxVars.push_back(idxVar);

            if (gi == 0) {
                std::string lenRes = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", iterSlots[0]}, lenRes);
                std::string lenSlot = "__lcsl_0_" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlot);
                lenSlots[0] = lenSlot;
            }
            // Nested gens' iterSlots/lenSlots are filled lazily inside
            // the parent's body (see below).
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
            std::string cmpR = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", idxVars[gi], lenSlots[gi]}, cmpR);
            ir.addInstruction(currentFunc, "br", {cmpR, g.bodyL, g.exitL});

            ir.addInstruction(currentFunc, "label", {}, g.bodyL);
            std::string item = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", iterSlots[gi], idxVars[gi]}, item);
            // target is the first child of the comprehension (Name or unpack pattern)
            if (g.targetNode) {
                if (g.targetNode->type == "Name") {
                    ir.addInstruction(currentFunc, "assign", {item}, g.targetNode->id);
                    const std::string& src = !iterSrcs[gi].empty() ? iterSrcs[gi] : iterSlots[gi];
                    propagateCompLoopVarTypes(src, g.targetNode->id);
                } else {
                    // tuple/list target unpack (reuse the unpack helper)
                    lowerUnpackTarget(g.targetNode, item);
                }
            }

            // Conditions: if any is false, jump to contL (skip append / recurse).
            for (const auto* cond : g.conds) {
                std::string trueL = "lc_ci_" + std::to_string(tempCounter++);
                std::string condV = lowerExpr(cond);
                ir.addInstruction(currentFunc, "br", {condV, trueL, g.contL});
                ir.addInstruction(currentFunc, "label", {}, trueL);
            }

            if (gi + 1 < gens.size()) {
                // Recurse into the next generator. Evaluate the nested
                // gen's iterable HERE (inside the parent body) so it can
                // reference the outer loop variable. CPython re-evaluates
                // each nested iterable per iteration of the enclosing loop.
                for (size_t gj = gi + 1; gj < gens.size(); ++gj) {
                    const ASTNode* genNode = node->children[gj + 1].get();
                    std::string iterVal = lowerExpr(genNode->children[1].get());
                    iterSrcs[gj] = iterVal;
                    std::string iterSlot = "__lci_" + std::to_string(gj) + "_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {iterVal}, iterSlot);
                    copyLayoutMaps(iterVal, iterSlot);
                    std::string listified = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", iterSlot}, listified);
                    copyLayoutMaps(iterSlot, listified);
                    std::string listifiedSlot = "__lcmat_" + std::to_string(gj) + "_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {listified}, listifiedSlot);
                    copyLayoutMaps(listified, listifiedSlot);
                    iterSlots[gj] = listifiedSlot;

                    std::string lenRes = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", iterSlots[gj]}, lenRes);
                    std::string lenSlot = "__lcsl_" + std::to_string(gj) + "_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlot);
                    lenSlots[gj] = lenSlot;

                    // Reset the nested gen's index to 0 for this iteration.
                    std::string zeroR = "$t" + std::to_string(tempCounter++);
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
            std::string one = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, one);
            std::string nxt = "$t" + std::to_string(tempCounter++);
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

    // Set comprehension — same generator/loop structure as lowerListComp,
    // but the result is a set (PySet_New / PySet_Add) so duplicates are
    // deduplicated and the result type is "set". No homogeneous-int/float
    // fast path (sets always store boxed PyObject*).
    std::string lowerSetComp(const ASTNode* node) {
        if (node->children.size() < 2) return "";
        const ASTNode* eltNode = node->children[0].get();

        std::string setVar = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PySet_New"}, setVar);
        noteType(setVar, "set");
        std::string setSlot = "__sc_lst_" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "assign", {setVar}, setSlot);
        noteType(setSlot, "set");

        struct GenCtx {
            std::string loopL, bodyL, contL, exitL;
            const ASTNode* targetNode;
            std::vector<const ASTNode*> conds;
        };
        std::vector<GenCtx> gens;
        gens.reserve(node->children.size() - 1);
        for (size_t gi = 1; gi < node->children.size(); ++gi) {
            const ASTNode* genNode = node->children[gi].get();
            if (!genNode || genNode->type != "comprehension" ||
                genNode->children.size() < 2) continue;
            int c = tempCounter++;
            GenCtx g;
            g.loopL  = "sc_lp_" + std::to_string(c);
            g.bodyL  = "sc_bd_" + std::to_string(c);
            g.contL  = "sc_ct_" + std::to_string(c);
            g.exitL  = "sc_ex_" + std::to_string(c);
            g.targetNode = genNode->children[0].get();
            for (size_t ci = 2; ci < genNode->children.size(); ++ci)
                g.conds.push_back(genNode->children[ci].get());
            gens.push_back(g);
        }
        if (gens.empty()) return setSlot;

        // Evaluate gen[0]'s iterator up front. Nested generators' iterables
        // are evaluated lazily inside the parent's body (they may reference
        // the outer loop variable).
        std::vector<std::string> iterSlots(gens.size());
        std::vector<std::string> lenSlots(gens.size());
        {
            const ASTNode* genNode = node->children[1].get();
            std::string iterVal = lowerExpr(genNode->children[1].get());
            std::string iterSlot = "__sci_0_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {iterVal}, iterSlot);
            std::string listified = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", iterSlot}, listified);
            std::string listifiedSlot = "__scmat_0_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {listified}, listifiedSlot);
            iterSlots[0] = listifiedSlot;
        }

        std::vector<std::string> idxVars;
        idxVars.reserve(gens.size());
        for (size_t gi = 0; gi < gens.size(); ++gi) {
            std::string idxVar  = "sc_i_" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            std::string idxInit = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxInit);
            ir.addInstruction(currentFunc, "assign", {idxInit}, idxVar);
            idxVars.push_back(idxVar);
            if (gi == 0) {
                std::string lenRes = "$t" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", iterSlots[0]}, lenRes);
                std::string lenSlot = "__scsl_0_" + std::to_string(tempCounter++);
                ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlot);
                lenSlots[0] = lenSlot;
            }
        }

        std::string postL = "sc_post_" + std::to_string(tempCounter++);

        for (size_t gi = 0; gi < gens.size(); ++gi) {
            const auto& g = gens[gi];
            ir.addInstruction(currentFunc, "label", {}, g.loopL);
            std::string cmpR = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", idxVars[gi], lenSlots[gi]}, cmpR);
            ir.addInstruction(currentFunc, "br", {cmpR, g.bodyL, g.exitL});

            ir.addInstruction(currentFunc, "label", {}, g.bodyL);
            std::string item = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_GetItemObj", iterSlots[gi], idxVars[gi]}, item);
            if (g.targetNode) {
                if (g.targetNode->type == "Name") {
                    ir.addInstruction(currentFunc, "assign", {item}, g.targetNode->id);
                } else {
                    lowerUnpackTarget(g.targetNode, item);
                }
            }

            for (const auto* cond : g.conds) {
                std::string trueL = "sc_ci_" + std::to_string(tempCounter++);
                std::string condV = lowerExpr(cond);
                ir.addInstruction(currentFunc, "br", {condV, trueL, g.contL});
                ir.addInstruction(currentFunc, "label", {}, trueL);
            }

            if (gi + 1 < gens.size()) {
                // Evaluate nested gens' iterables HERE (inside the parent
                // body) so they can reference the outer loop variable.
                for (size_t gj = gi + 1; gj < gens.size(); ++gj) {
                    const ASTNode* genNode = node->children[gj + 1].get();
                    std::string iterVal = lowerExpr(genNode->children[1].get());
                    std::string iterSlot = "__sci_" + std::to_string(gj) + "_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {iterVal}, iterSlot);
                    std::string listified = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyBuiltin_List", iterSlot}, listified);
                    std::string listifiedSlot = "__scmat_" + std::to_string(gj) + "_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {listified}, listifiedSlot);
                    iterSlots[gj] = listifiedSlot;

                    std::string lenRes = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", iterSlots[gj]}, lenRes);
                    std::string lenSlot = "__scsl_" + std::to_string(gj) + "_" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlot);
                    lenSlots[gj] = lenSlot;

                    std::string zeroR = "$t" + std::to_string(tempCounter++);
                    ir.addInstruction(currentFunc, "const", {"0"}, zeroR);
                    ir.addInstruction(currentFunc, "assign", {zeroR}, idxVars[gj]);
                }
                ir.addInstruction(currentFunc, "br", {}, gens[gi + 1].loopL);
            } else {
                std::string eltVal = lowerExpr(eltNode);
                ir.addInstruction(currentFunc, "call", {"PySet_Add", setSlot, eltVal}, "");
            }
            ir.addInstruction(currentFunc, "label", {}, g.contL);
            std::string one = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, one);
            std::string nxt = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "add", {idxVars[gi], one}, nxt);
            ir.addInstruction(currentFunc, "assign", {nxt}, idxVars[gi]);
            ir.addInstruction(currentFunc, "br", {}, g.loopL);
            ir.addInstruction(currentFunc, "label", {}, g.exitL);
            if (gi + 1 == gens.size()) {
                if (gi > 0) ir.addInstruction(currentFunc, "br", {}, gens[gi - 1].contL);
            } else if (gi == 0) {
                ir.addInstruction(currentFunc, "br", {}, postL);
            } else {
                ir.addInstruction(currentFunc, "br", {}, gens[gi - 1].contL);
            }
        }
        ir.addInstruction(currentFunc, "label", {}, postL);
        return setSlot;
    }

    std::string lowerDictComp(const ASTNode* node) {
        // {key: val for target in iter if conds ...}  (supports multiple generators for product/nested)
        if (node->children.size() < 2) {
            std::string dictRes = "$t" + std::to_string(tempCounter++);
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
            std::string dictRes = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyDict_New"}, dictRes);
            return dictRes;
        }

        // Create result dict
        std::string dictRes = "$t" + std::to_string(tempCounter++);
        ir.addInstruction(currentFunc, "call", {"PyDict_New"}, dictRes);

        // Recursive emitter for nested generators.
        // gi: current generator index; after last, emit the key:val insertion.
        std::function<void(size_t)> emitLevel = [&](size_t gi) {
            if (gi == gens.size()) {
                // innermost: compute key/val and insert
                std::string kVal = lowerExpr(keyNode);
                std::string vVal = lowerExpr(valNode);
                std::string dummy = "$t" + std::to_string(tempCounter++);
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
            std::string lenRes  = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "call", {"PyList_SizeBoxed", iterVal}, lenRes);
            std::string lenSlotDC = "__sl_" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "assign", {lenRes}, lenSlotDC);

            // per-level index
            std::string idxVar  = "dc_i" + std::to_string(gi) + "_" + std::to_string(tempCounter++);
            std::string idxInit = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"0"}, idxInit);
            ir.addInstruction(currentFunc, "assign", {idxInit}, idxVar);

            int dc = tempCounter++;
            std::string loopL = "dc_lp" + std::to_string(gi) + "_" + std::to_string(dc);
            std::string bodyL = "dc_bd" + std::to_string(gi) + "_" + std::to_string(dc);
            std::string contL = "dc_ct" + std::to_string(gi) + "_" + std::to_string(dc);
            std::string exitL = "dc_ex" + std::to_string(gi) + "_" + std::to_string(dc);

            ir.addInstruction(currentFunc, "label", {}, loopL);
            std::string cmpR = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "icmp", {"Lt", idxVar, lenSlotDC}, cmpR);
            ir.addInstruction(currentFunc, "br", {cmpR, bodyL, exitL});

            ir.addInstruction(currentFunc, "label", {}, bodyL);
            std::string item = "$t" + std::to_string(tempCounter++);
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
            std::string one = "$t" + std::to_string(tempCounter++);
            ir.addInstruction(currentFunc, "const", {"1"}, one);
            std::string nxt = "$t" + std::to_string(tempCounter++);
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
               const std::unordered_map<std::string, std::vector<std::string>>& importedModuleGlobals = {},
               const std::string& sourceFile = "",
               const std::string& currentModulePackage = "") {
    if (!node) return;
    LoweringVisitor visitor(ir, compiledModules, importedModuleGlobals);
    if (!sourceFile.empty()) visitor.currentSourceFile = sourceFile;
    visitor.currentModulePackage = currentModulePackage;
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

// B7 package imports: a single discovered compile unit for a dotted import
// name, one per intermediate package level plus the leaf. `filePath` is
// empty for namespace packages (a directory with no __init__.py) — those
// get a synthetic empty-dict module instead of real compiled source.
struct DiscoveredModule {
    std::string dottedName;         // true Python dotted path, e.g. "package_a.subpkg.mod_b1"
    std::string filePath;           // .py source, or empty for a namespace package
    std::string parentDottedName;   // dottedName minus its last component, or "" for top-level
    bool isNamespacePackage = false;
    // True when this compile unit is a package's own __init__.py (as
    // opposed to a plain leaf submodule). A package's own __init__.py has
    // __package__ == its own dottedName; a leaf submodule's __package__ is
    // its parentDottedName. Needed to resolve *this module's own* relative
    // imports (see resolveRelativeImport / packageContextOf below).
    bool isPackageInit = false;
};

// The package context used to resolve a module's own relative imports
// (its __package__ equivalent): a package's own __init__.py resolves
// relative to itself; a leaf submodule resolves relative to its parent.
static std::string packageContextOf(const DiscoveredModule& dm) {
    return dm.isPackageInit ? dm.dottedName : dm.parentDottedName;
}

// Resolve a relative ImportFrom (level >= 1) to an absolute dotted name,
// given the importing module's own package context. Mirrors CPython's
// importlib._bootstrap._resolve_name. Returns nullopt when level exceeds
// the available package depth ("attempted relative import beyond
// top-level package") or packageContext is empty (a directly-executed
// main script has no package — matches CPython raising ImportError for
// relative imports in __main__).
static std::optional<std::string> resolveRelativeImport(
        const std::string& packageContext, int level, const std::string& module) {
    if (level <= 0) return module;  // not actually relative
    if (packageContext.empty()) return std::nullopt;

    std::vector<std::string> parts;
    {
        std::stringstream ss(packageContext);
        std::string tok;
        while (std::getline(ss, tok, '.')) if (!tok.empty()) parts.push_back(tok);
    }
    if (static_cast<size_t>(level) > parts.size()) return std::nullopt;

    parts.resize(parts.size() - (level - 1));
    std::string base;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) base += ".";
        base += parts[i];
    }
    if (module.empty()) return base;
    if (base.empty()) return module;
    return base + "." + module;
}

// Dots aren't valid in C/LLVM identifiers. Used only for the generated
// __module__<ident> function symbol — the registry's string lookup key
// (and every user-visible Python name) always keeps the true dotted form.
static std::string sanitizeModuleIdent(const std::string& dottedName) {
    std::string s = dottedName;
    for (char& c : s) if (c == '.') c = '_';
    return s;
}

// The exported (non-underscore, top-level) names for each synthetic/
// built-in module pyc implements in the runtime — see the corresponding
// dict-building code in pyc_import_failed (src/runtime/Runtime.cpp). Used
// to (a) expand `from X import *` for these modules (LoweringVisitor's
// ImportFrom handling, the not-found/synthetic branch) and (b) derive
// kSyntheticModules below, so the two never drift apart. `sys` is
// deliberately absent — `from sys import *` isn't meaningful (argv/stderr/
// stdout aren't typically star-imported) and sys is handled outside
// pyc_import_failed's normal dict-building path.
static const std::unordered_map<std::string, std::vector<std::string>>& syntheticModuleExports() {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        {"re",         {"finditer", "findall", "compile", "match", "search", "sub", "split",
                        "IGNORECASE", "MULTILINE", "DOTALL"}},
        {"os",         {"environ", "path", "unlink", "remove", "rename", "getcwd",
                        "listdir", "makedirs"}},
        {"subprocess", {"call", "check_output"}},
        {"functools",  {"cmp_to_key", "reduce", "partial", "wraps", "lru_cache"}},
        {"operator",   {"add", "sub", "mul", "truediv", "mod", "eq", "ne", "lt", "gt",
                        "le", "ge", "not_", "neg", "itemgetter", "attrgetter"}},
        {"cmath",      {"sqrt", "log", "exp", "sin", "cos", "tan"}},
        {"time",       {"perf_counter"}},
        {"math",       {"sqrt", "floor", "ceil", "trunc", "pow", "log", "log2", "log10",
                         "exp", "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
                         "hypot", "fabs", "fmod", "degrees", "radians", "isnan", "isinf",
                         "isfinite", "gcd", "factorial", "pi", "e", "tau", "inf", "nan"}},
        {"json",       {"dumps", "loads"}},
        {"random",     {"seed", "random", "randrange", "randint", "uniform", "choice", "shuffle"}},
        {"itertools",  {"chain", "product", "combinations", "permutations", "starmap",
                         "islice", "zip_longest", "accumulate", "takewhile", "dropwhile",
                         "compress", "groupby"}},
        {"collections", {"Counter", "most_common", "deque", "namedtuple", "defaultdict"}},
        {"datetime",   {"date", "datetime", "timedelta"}},
        {"pathlib",    {"Path"}},
        {"hashlib",    {"md5", "sha1", "sha256"}},
        {"base64",     {"b64encode", "b64decode"}},
        {"struct",     {"pack", "unpack"}},
        {"heapq",      {"heapify", "heappush", "heappop", "heappushpop", "heapreplace",
                        "nlargest", "nsmallest"}},
        {"bisect",     {"bisect_left", "bisect_right", "bisect", "insort_left",
                        "insort_right", "insort"}},
        {"statistics", {"mean", "median", "median_low", "median_high", "mode",
                        "stdev", "variance", "pstdev", "pvariance"}},
        {"string",     {"ascii_lowercase", "ascii_uppercase", "ascii_letters", "digits",
                        "hexdigits", "octdigits", "punctuation", "whitespace", "printable"}},
        {"textwrap",   {"wrap", "fill"}},
        {"uuid",       {"uuid4"}},
        {"copy",       {"copy", "deepcopy"}},
        {"shutil",     {"copyfile", "move", "rmtree"}},
        {"glob",       {"glob"}},
        {"csv",        {"reader", "writer"}},
        {"decimal",    {"Decimal"}},
    };
    return table;
}

// Resolve one dotted import name (e.g. "package_a.subpkg.mod_b1") to its
// full chain of compile units — every intermediate package level plus the
// leaf — searching the local source directory, then venv site-packages,
// then the stdlib path, in that order (matching CPython's sys.path
// resolution: the top-level component picks the root, then every
// subsequent component is resolved strictly beneath it). Newly-discovered
// units are appended to `out`; `seen` dedups across multiple calls so
// imports that share a package prefix (e.g. package_a.mod_a1 and
// package_a.mod_a2) only add "package_a" once. A name that can't be
// resolved at all is silently skipped — the caller's existing
// pyc_import_failed fallback handles it unchanged.
static void discoverDottedModule(const std::string& dottedName,
                                  const std::string& sourceDir,
                                  const std::string& venvModuleDir,
                                  const std::string& stdlibPath,
                                  std::vector<DiscoveredModule>& out,
                                  std::unordered_set<std::string>& seen) {
    if (dottedName.empty() || seen.count(dottedName)) return;

    std::vector<std::string> parts;
    {
        std::stringstream ss(dottedName);
        std::string tok;
        while (std::getline(ss, tok, '.')) if (!tok.empty()) parts.push_back(tok);
    }
    if (parts.empty()) return;

    // Names pyc_import_failed already implements as synthetic/built-in
    // modules (see src/runtime/Runtime.cpp). These must never resolve to a
    // real system stdlib file even if one happens to exist on disk — pyc
    // can't compile arbitrary real CPython stdlib source (it uses language
    // features and C extensions outside pyc's supported subset), so
    // `import re` etc. always needs to fall through to the hand-written
    // PCRE2-backed (etc.) runtime implementation instead. Derived from
    // syntheticModuleExports() plus "sys" (which has no star-import
    // exports but is still a synthetic module name).
    static const std::unordered_set<std::string> kSyntheticModules = [] {
        std::unordered_set<std::string> s{"sys"};
        for (const auto& kv : syntheticModuleExports()) s.insert(kv.first);
        return s;
    }();
    if (kSyntheticModules.count(parts[0])) return;

    std::vector<std::string> roots;
    if (!sourceDir.empty()) roots.push_back(sourceDir);
    if (!venvModuleDir.empty()) roots.push_back(venvModuleDir);
    if (!stdlibPath.empty()) roots.push_back(stdlibPath);

    std::string chosenRoot;
    for (auto& root : roots) {
        fs::path base = fs::path(root) / parts[0];
        fs::path leaf = base; leaf += ".py";
        if (fs::exists(base) || fs::exists(leaf)) { chosenRoot = root; break; }
    }
    if (chosenRoot.empty()) return;

    fs::path curPath(chosenRoot);
    std::string curDotted;
    for (size_t i = 0; i < parts.size(); ++i) {
        curPath /= parts[i];
        curDotted = curDotted.empty() ? parts[i] : curDotted + "." + parts[i];
        if (seen.count(curDotted)) continue;
        bool isLeaf = (i + 1 == parts.size());
        std::string parentDotted;
        {
            auto dot = curDotted.rfind('.');
            parentDotted = (dot == std::string::npos) ? "" : curDotted.substr(0, dot);
        }

        fs::path initPy = curPath / "__init__.py";
        fs::path leafPy = curPath; leafPy += ".py";
        if (isLeaf && fs::exists(leafPy) && fs::is_regular_file(leafPy)) {
            out.push_back({curDotted, leafPy.string(), parentDotted, false, false});
            seen.insert(curDotted);
        } else if (fs::exists(initPy) && fs::is_regular_file(initPy)) {
            out.push_back({curDotted, initPy.string(), parentDotted, false, true});
            seen.insert(curDotted);
        } else if (fs::is_directory(curPath)) {
            // Namespace package (PEP 420): no __init__.py, synthesize an
            // empty module dict for this level (see namespace synthesis).
            // It's still package-like (a submodule beneath it resolves
            // relative imports against it), so isPackageInit=true too.
            out.push_back({curDotted, "", parentDotted, true, true});
            seen.insert(curDotted);
        } else {
            return; // this component doesn't exist — stop, leave unresolved
        }
    }
}

bool Compiler::compile(const std::string& inputPath, const std::string& outputPath,
                        bool useStatic, int optLevel, bool emitLLVM, bool emitASM,
                        bool verbose, bool debugInfo,
                         const std::string& pgoInstrument,
                         const std::string& pgoProfile,
                         const std::string& pgoGenerateProfile,
                         const std::string& mcpu,
                         const std::string& march,
                         const std::string& targetArch,
                          const std::string& venvPath,
                          bool dynamicLink,
                          const std::string& pythonPath) {
    if (verbose) std::cout << "DEBUG: compile called with pythonPath=" << pythonPath << "\n";
    
    // Handle virtual environment path
    std::string venvLib = "";
    if (!venvPath.empty()) {
        if (verbose) {
            std::cout << "Using virtual environment: " << venvPath << "\n";
        }
        
        // Construct path to site-packages or dist-packages
        fs::path venvDir(venvPath);
        if (fs::exists(venvDir / "lib")) {
            // Unix-style venv
            fs::path libDir = venvDir / "lib";
            fs::path pythonDir;
            
            // Find the Python version directory (e.g., python3.11)
            if (fs::is_directory(libDir)) {
                for (const auto& entry : fs::directory_iterator(libDir)) {
                    if (entry.is_directory() && entry.path().filename().string().find("python") != std::string::npos) {
                        pythonDir = entry.path();
                        break;
                    }
                }
            }
            
            // Look for site-packages
            if (!pythonDir.empty()) {
                if (fs::exists(pythonDir / "site-packages")) {
                    venvLib = (pythonDir / "site-packages").string();
                } else if (fs::exists(pythonDir / "dist-packages")) {
                    venvLib = (pythonDir / "dist-packages").string();
                }
            }
        } else if (fs::exists(venvDir / "Lib")) {
            // Windows-style venv
            fs::path libDir = venvDir / "Lib";
            if (fs::exists(libDir / "site-packages")) {
                venvLib = (libDir / "site-packages").string();
            }
        }
        
        if (venvLib.empty()) {
            std::cerr << "Warning: Could not find site-packages in venv: " << venvPath << "\n";
        } else if (verbose) {
            std::cout << "Venv site-packages: " << venvLib << "\n";
        }
    }
    
    // PGO profile generation: compile instrumented binary, run test script, merge .profraw → .profdata
    if (!pgoGenerateProfile.empty()) {
        if (verbose)
            std::cout << "PGO: generating profile with test script: " << pgoGenerateProfile << "\n";

        // Step 1: Compile with instrumentation
        std::string instrOutput = outputPath + "_instr";
        std::string instrPgoInstrument = "instrument";
        std::string instrPgoProfile = "";

        // Parse and lower AST
        PythonParser mainParser;
        auto mainAst = mainParser.parseFile(inputPath);
        if (!mainAst) {
            std::cerr << "Parse error for " << inputPath << std::endl;
            return false;
        }

        std::unordered_set<std::string> importNames;
        std::function<void(const ASTNode*)> collectImports = [&](const ASTNode* node) {
            if (!node) return;
            if (node->type == "Import") {
                std::stringstream ss(node->id);
                std::string tok;
                while (ss >> tok) importNames.insert(tok);
            } else if (node->type == "ImportFrom") {
                if (!node->id.empty()) {
                    importNames.insert(node->id);
                    for (size_t i = 0; i + 1 < node->importNames.size(); i += 2) {
                        const std::string& orig = node->importNames[i];
                        if (!orig.empty() && orig != "*") {
                            importNames.insert(node->id + "." + orig);
                        }
                    }
                }
            }
            for (const auto& c : node->children) collectImports(c.get());
        };
        collectImports(mainAst.get());
        
        // Install missing modules from venv if specified
        std::vector<std::string> missingModules;
        for (auto& moduleName : importNames) {
            bool found = false;
            
            // Check if module exists in venv site-packages
            if (!venvLib.empty()) {
                fs::path modulePath = fs::path(venvLib) / moduleName;
                if (fs::exists(modulePath) && 
                    (fs::is_directory(modulePath) || 
                     (fs::is_regular_file(modulePath) && modulePath.extension() == ".py"))) {
                    found = true;
                }
            }
            
            // Check if module exists in source directory
            std::string dir = fs::path(inputPath).parent_path().string();
            if (dir.empty()) dir = ".";
            std::string modulePath = dir + "/" + moduleName + ".py";
            if (fs::exists(modulePath) && fs::is_regular_file(modulePath)) {
                found = true;
            }
            
            if (!found) {
                missingModules.push_back(moduleName);
            }
        }
        
        // Install missing modules using pip or uv
        if (!missingModules.empty() && !venvLib.empty()) {
            if (verbose) {
                std::cout << "Installing missing modules: ";
                for (size_t i = 0; i < missingModules.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << missingModules[i];
                }
                std::cout << "\n";
            }
            
            // Try uv first, then pip
            std::string installer = "uv";
            std::string installCmd;
            
            // Check if uv is available
            FILE* check = popen("which uv 2>/dev/null", "r");
            if (check) {
                char buffer[256];
                if (fgets(buffer, sizeof(buffer), check) != nullptr) {
                    // uv found
                } else {
                    installer = "pip";
                }
                pclose(check);
            } else {
                installer = "pip";
            }
            
            if (installer == "uv") {
                installCmd = "uv pip install --no-index --find-links=" + venvLib + " -t " + venvLib + " ";
                for (auto& mod : missingModules) {
                    installCmd += mod + " ";
                }
            } else {
                installCmd = "pip install -t " + venvLib + " ";
                for (auto& mod : missingModules) {
                    installCmd += mod + " ";
                }
            }
            
            if (verbose) {
                std::cout << "Running: " << installCmd << "\n";
            }
            
            int ret = system(installCmd.c_str());
            if (ret != 0) {
                std::cerr << "Warning: Module installation failed (exit code: " << ret << ")\n";
            }
        }
        
        std::string dir = fs::path(inputPath).parent_path().string();
         if (dir.empty()) dir = ".";
         
         // Get Python version for standard library path
         std::string pythonVersion;
         if (!pythonPath.empty()) {
             // Try to detect version from pythonPath/include directory
             fs::path includeDir = fs::path(pythonPath) / "include";
             if (fs::exists(includeDir)) {
                 for (const auto& entry : fs::directory_iterator(includeDir)) {
                     std::string filename = entry.path().filename().string();
                     if (filename.find("python") != std::string::npos) {
                         // Extract version from directory name like "python3.11"
                         size_t pos = filename.find("python");
                         if (pos != std::string::npos) {
                             pythonVersion = filename.substr(pos + 6);
                             // Remove any trailing non-numeric characters
                             while (!pythonVersion.empty() && !isdigit(pythonVersion.back())) {
                                 pythonVersion.pop_back();
                             }
                             break;
                         }
                     }
                 }
             }
             // Fallback: try python3-config if available
             if (pythonVersion.empty()) {
                 std::string configCmd = pythonPath + "/bin/python3-config --version 2>/dev/null";
                 FILE* pipe = popen(configCmd.c_str(), "r");
                 if (pipe) {
                     char buf[64];
                     if (fgets(buf, sizeof(buf), pipe)) {
                         // Parse version like "Python 3.11.5"
                         for (size_t i = 0; i < strlen(buf); ++i) {
                             if (isdigit(buf[i])) {
                                 pythonVersion = buf + i;
                                 // Keep only major.minor
                                 size_t dotCount = 0;
                                 for (size_t j = 0; j < pythonVersion.length(); ++j) {
                                     if (pythonVersion[j] == '.') {
                                         dotCount++;
                                         if (dotCount > 1) {
                                             pythonVersion = pythonVersion.substr(0, j);
                                             break;
                                         }
                                     } else if (!isdigit(pythonVersion[j]) && pythonVersion[j] != '.') {
                                         pythonVersion = pythonVersion.substr(0, j);
                                         break;
                                     }
                                 }
                                 break;
                             }
                         }
                     }
                     pclose(pipe);
                 }
             }
         } else {
             FILE* pipe = popen("python3 -c 'import sys; print(f\"{sys.version_info.major}.{sys.version_info.minor}\")' 2>/dev/null", "r");
             if (pipe) {
                 char buf[64];
                 if (fgets(buf, sizeof(buf), pipe)) {
                     pythonVersion = buf;
                     pythonVersion.erase(std::remove(pythonVersion.begin(), pythonVersion.end(), '\n'), pythonVersion.end());
                  }
                  pclose(pipe);
              }
          }
          
        // Use provided pythonPath if specified, otherwise use default location
        std::string stdlibPath = "";
        if (!pythonPath.empty()) {
            if (!pythonVersion.empty()) {
                stdlibPath = pythonPath + "/lib/python" + pythonVersion;
                if (verbose) std::cout << "DEBUG: stdlibPath=" << stdlibPath << "\n";
            } else {
                stdlibPath = pythonPath;
                if (verbose) std::cout << "DEBUG: stdlibPath (fallback)=" << stdlibPath << "\n";
            }
          } else if (!pythonVersion.empty()) {
              stdlibPath = "/usr/lib/python" + pythonVersion;
          }
         std::string mainBasename = fs::path(inputPath).stem().string();

        std::string venvModuleDir = "";
        if (!venvPath.empty()) venvModuleDir = venvLib;

        std::vector<DiscoveredModule> discovered;
        std::unordered_set<std::string> seenModules;
        for (auto& name : importNames) {
            discoverDottedModule(name, dir, venvModuleDir, stdlibPath, discovered, seenModules);
        }

        // B7: pre-parse each discovered module, both to collect its exported
        // globals and to transitively discover the imports *it* makes —
        // absolute or relative. See the default (non-PGO) compile path for
        // the full explanation; kept in sync here.
        std::unordered_map<std::string, std::vector<std::string>> importedModuleGlobals;
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
        auto scanModuleImports = [&](const ASTNode* modNode, const std::string& packageContext) {
            std::function<void(const ASTNode*)> walk = [&](const ASTNode* n) {
                if (!n) return;
                if (n->type == "Import") {
                    std::stringstream ss(n->id);
                    std::string tok;
                    while (ss >> tok) discoverDottedModule(tok, dir, venvModuleDir, stdlibPath, discovered, seenModules);
                } else if (n->type == "ImportFrom") {
                    auto resolved = resolveRelativeImport(packageContext, n->level, n->id);
                    if (resolved) {
                        if (!resolved->empty())
                            discoverDottedModule(*resolved, dir, venvModuleDir, stdlibPath, discovered, seenModules);
                        for (size_t i = 0; i + 1 < n->importNames.size(); i += 2) {
                            const std::string& orig = n->importNames[i];
                            if (orig.empty() || orig == "*") continue;
                            std::string sub = resolved->empty() ? orig : (*resolved + "." + orig);
                            discoverDottedModule(sub, dir, venvModuleDir, stdlibPath, discovered, seenModules);
                        }
                    }
                }
                for (const auto& c : n->children) walk(c.get());
            };
            walk(modNode);
        };
        for (size_t i = 0; i < discovered.size(); ++i) {
            std::string filePath = discovered[i].filePath;
            std::string dottedName = discovered[i].dottedName;
            std::string packageContext = packageContextOf(discovered[i]);
            if (filePath.empty()) continue;
            PythonParser pp;
            auto ast = pp.parseFile(filePath);
            if (!ast) continue;
            importedModuleGlobals[dottedName] = collectTopLevelNames(ast.get());
            scanModuleImports(ast.get(), packageContext);
        }

        std::vector<std::string> moduleNames;
        std::unordered_set<std::string> compiledModules;
        for (auto& dm : discovered) {
            moduleNames.push_back(dm.dottedName);
            compiledModules.insert(dm.dottedName);
        }

        std::vector<std::unique_ptr<llvm::Module>> modules;
        llvm::LLVMContext context;

        {
            PythonParser parser;
            auto ast = parser.parseFile(inputPath);
            if (!ast) {
                std::cerr << "Warning: Failed to parse " << inputPath << ", skipping\n";
                return false;
            }
            ModuleIR ir;
            ir.moduleName = mainBasename;
            lowerAST(ast.get(), ir, compiledModules, importedModuleGlobals, inputPath);
            Codegen codegen;
            auto module = codegen.generate(ir, context, "pyc_" + mainBasename, debugInfo);
            if (!module) {
                std::cerr << "Warning: Codegen failed for " << inputPath << "\n";
                return false;
            }
            llvm::Function* entryFunc = module->getFunction("__module__");
            if (entryFunc) entryFunc->setName("pyc_user_main");
            modules.push_back(std::move(module));
        }

        for (auto& dm : discovered) {
            std::string sanitized = sanitizeModuleIdent(dm.dottedName);
            std::unique_ptr<llvm::Module> module;

            if (dm.isNamespacePackage) {
                ModuleIR ir;
                ir.moduleName = sanitized;
                ir.addFunction("__module__", {});
                std::string modDict = "t0";
                ir.addInstruction("__module__", "call", {"PyDict_New"}, modDict);
                ir.addInstruction("__module__", "ret", {modDict});
                Codegen codegen;
                module = codegen.generate(ir, context, "pyc_" + sanitized, debugInfo);
                if (!module) {
                    std::cerr << "Warning: Codegen failed for namespace package " << dm.dottedName << ", skipping\n";
                    continue;
                }
            } else {
                PythonParser parser;
                auto ast = parser.parseFile(dm.filePath);
                if (!ast) {
                    std::cerr << "Warning: Failed to parse " << dm.filePath << ", skipping\n";
                    continue;
                }
                ModuleIR ir;
                ir.moduleName = sanitized;
                lowerAST(ast.get(), ir, compiledModules, importedModuleGlobals, dm.filePath, packageContextOf(dm));
                Codegen codegen;
                module = codegen.generate(ir, context, "pyc_" + sanitized, debugInfo);
                if (!module) {
                    std::cerr << "Warning: Codegen failed for " << dm.filePath << ", skipping\n";
                    continue;
                }
            }

            llvm::Function* entryFunc = module->getFunction("__module__");
            if (entryFunc) entryFunc->setName("__module__" + sanitized);

            modules.push_back(std::move(module));
        }

        if (modules.empty()) {
            std::cerr << "Error: No modules generated\n";
            return false;
        }

        std::string b7CSource = "#include <string.h>\n";
        b7CSource += "#include <stdio.h>\n";
        b7CSource += "#include <stdlib.h>\n";
        b7CSource += "#include <unistd.h>\n\n";
        for (auto& name : moduleNames) {
            b7CSource += "extern void* __module__" + sanitizeModuleIdent(name) + "(void);\n";
        }
        b7CSource += "\n";
        b7CSource += "typedef struct {\n";
        b7CSource += "    const char* name;\n";
        b7CSource += "    const char* parent;\n";
        b7CSource += "    void* (*entry)(void);\n";
        b7CSource += "    int loading;\n";
        b7CSource += "} pyc_module_entry;\n\n";
        b7CSource += "static pyc_module_entry pyc_modules[] = {\n";
        for (auto& dm : discovered) {
            b7CSource += "    {\"" + dm.dottedName + "\", \"" + dm.parentDottedName + "\", __module__" +
                          sanitizeModuleIdent(dm.dottedName) + ", 0},\n";
        }
        b7CSource += "    {NULL, NULL, NULL, 0}\n";
        b7CSource += "};\n\n";
        b7CSource += "extern const char* PyStr_AsUTF8(void* obj);\n";
        b7CSource += "extern void* PyUnicode_FromString(const char* s);\n";
        b7CSource += "extern void* pyc_get_sys_modules(void);\n";
        b7CSource += "extern void* PyDict_GetItem(void* dict, void* key);\n";
        b7CSource += "extern void  Py_DECREF(void* obj);\n";
        b7CSource += "extern void  pyc_register_module(const char* name, void* module_dict);\n";
        b7CSource += "extern void* Pyc_SetItem(void* obj, void* key, void* val);\n";
        b7CSource += "void* pyc_run_module(void* moduleNameObj) {\n";
        b7CSource += "    const char* moduleName = PyStr_AsUTF8(moduleNameObj);\n";
        b7CSource += "    if (!moduleName) return NULL;\n";
        b7CSource += "    // sys.modules-backed cache: a module's top-level code runs\n";
        b7CSource += "    // at most once, matching CPython import semantics.\n";
        b7CSource += "    void* modules = pyc_get_sys_modules();\n";
        b7CSource += "    if (modules) {\n";
        b7CSource += "        void* nameKey = PyUnicode_FromString(moduleName);\n";
        b7CSource += "        void* cached = PyDict_GetItem(modules, nameKey);\n";
        b7CSource += "        Py_DECREF(nameKey);\n";
        b7CSource += "        Py_DECREF(modules);\n";
        b7CSource += "        if (cached) return cached;\n";
        b7CSource += "    }\n";
        b7CSource += "    for (int i = 0; pyc_modules[i].name != NULL; i++) {\n";
        b7CSource += "        if (strcmp(pyc_modules[i].name, moduleName) == 0) {\n";
        b7CSource += "            if (pyc_modules[i].loading) return NULL;\n";
        b7CSource += "            pyc_modules[i].loading = 1;\n";
        b7CSource += "            void* result = pyc_modules[i].entry();\n";
        b7CSource += "            pyc_modules[i].loading = 0;\n";
        b7CSource += "            pyc_register_module(moduleName, result);\n";
        b7CSource += "            if (pyc_modules[i].parent[0] != '\\0') {\n";
        b7CSource += "                void* parentNameObj = PyUnicode_FromString(pyc_modules[i].parent);\n";
        b7CSource += "                void* parentDict = pyc_run_module(parentNameObj);\n";
        b7CSource += "                Py_DECREF(parentNameObj);\n";
        b7CSource += "                if (parentDict) {\n";
        b7CSource += "                    const char* leaf = pyc_modules[i].name + strlen(pyc_modules[i].parent) + 1;\n";
        b7CSource += "                    void* leafKey = PyUnicode_FromString(leaf);\n";
        b7CSource += "                    Pyc_SetItem(parentDict, leafKey, result);\n";
        b7CSource += "                    Py_DECREF(leafKey);\n";
        b7CSource += "                }\n";
        b7CSource += "            }\n";
        b7CSource += "            return result;\n";
        b7CSource += "        }\n";
        b7CSource += "    }\n";
        b7CSource += "    // Module not found - unsupported\n";
        b7CSource += "    return NULL;\n";
        b7CSource += "}\n";
        b7CSource += "\n";
        b7CSource += "void __module__sys(void) {\n";
        b7CSource += "}\n";
        b7CSource += "\n";
        b7CSource += "void __module__os(void) {\n";
        b7CSource += "}\n";
        b7CSource += "\n";
        b7CSource += "void __module__subprocess(void) {\n";
        b7CSource += "}\n";

        std::string b7CFile = instrOutput + "_b7_modules.c";
        std::ofstream b7Out(b7CFile);
        if (b7Out.is_open()) {
            b7Out << b7CSource;
            b7Out.close();
        }

        Codegen codegen;
        auto module = Codegen::mergeModules(modules, context, "pyc_module");
        if (!module) return false;

        const bool useLTO = (optLevel >= 1);
        if (useLTO) {
            std::string rtBitcodePath = PYC_RUNTIME_BC;
            codegen.linkRuntimeBitcode(module.get(), rtBitcodePath);
        }

        codegen.optimize(module.get(), optLevel, mcpu, march, instrPgoInstrument, instrPgoProfile);

        if (codegen.emitObject(module.get(), instrOutput + ".o", mcpu, march)) {
            std::string linkCmd = "clang++ ";
            if (useStatic) linkCmd += "-static ";
            linkCmd += "-s -Wl,--gc-sections -Wl,--strip-all ";

             std::string sourceDir = PYC_SOURCE_DIR;
             std::string runtimeLink = " " + sourceDir + "/src/runtime/Runtime.cpp";
             for (const auto& libdir : {"./build", "../build", ".", "/usr/local/lib", "/usr/lib"}) {
                 std::string libpath = std::string(libdir) + "/libpycrt.a";
                 if (std::ifstream(libpath).good()) {
                     runtimeLink = " -L" + std::string(libdir) + " -lpycrt";
                     break;
                 }
             }
             std::string pythonIncludes = "";
             std::string pythonLibLink = "";
             
             if (!pythonPath.empty()) {
                 if (!pythonVersion.empty()) {
                     pythonIncludes = "-I" + pythonPath + "/include/python" + pythonVersion;
                     std::string libDir = pythonPath + "/lib";
                     if (fs::exists(libDir)) {
                         pythonLibLink = "-L" + libDir + " -lpython" + pythonVersion;
                     }
                 } else {
                     pythonIncludes = "-I" + pythonPath + "/include";
                     std::string libDir = pythonPath + "/lib";
                     if (fs::exists(libDir)) {
                         pythonLibLink = "-L" + libDir + " -lpython";
                     }
                 }
             } else {
                 FILE* pipe = popen("python3-config --includes 2>/dev/null | grep -o '\\-I[^ ]*' | head -1", "r");
                 if (pipe) {
                     char buf[256];
                     if (fgets(buf, sizeof(buf), pipe)) {
                         buf[strcspn(buf, "\n")] = 0;
                         pythonIncludes = buf;
                     }
                     pclose(pipe);
                 }
                 FILE* pipe2 = popen("python3-config --ldflags 2>/dev/null | grep -o '\\-lpython[^ ]*' | head -1", "r");
                 if (pipe2) {
                     char buf[256];
                     if (fgets(buf, sizeof(buf), pipe2)) {
                         buf[strcspn(buf, "\n")] = 0;
                         pythonLibLink = buf;
                     }
                     pclose(pipe2);
                 }
             }

             if (useLTO) {
                 linkCmd += instrOutput + ".o -flto=thin -Wl,--allow-multiple-definition ";
             } else {
                 linkCmd += instrOutput + ".o ";
             }
             linkCmd += "-x c " + b7CFile + " -x none -I" + sourceDir + "/include " +
                 pythonIncludes + " " + sourceDir + "/src/runtime/MainWrapper.cpp" +
                 runtimeLink + " " + pythonLibLink + " -lpcre2-8 -lmpdec -o " + instrOutput + " -O3";
            linkCmd += " -fprofile-instr-generate=" + instrOutput + ".profraw";
            if (debugInfo) linkCmd += " -g";

            if (verbose)
                std::cout << "Compiling instrumented binary: " << linkCmd << "\n";

            int result = std::system(linkCmd.c_str());
            if (result != 0) {
                std::cerr << "PGO instrumentation compilation failed\n";
                return false;
            }

            // Step 2: Run the binary with the test script
            std::string runCmd = instrOutput;
            if (!pgoGenerateProfile.empty()) {
                runCmd += " " + pgoGenerateProfile;
            }
            if (verbose)
                std::cout << "Running instrumented binary: " << runCmd << "\n";

            result = std::system(runCmd.c_str());
            if (result != 0) {
                std::cerr << "Warning: instrumented binary exited with non-zero status (may be normal)\n";
            }

            // Step 3: Merge .profraw to .profdata
            std::string profdataOutput = outputPath + ".profdata";
            std::string mergeCmd = "llvm-profdata merge -output=" + profdataOutput + " " + instrOutput + ".profraw";
            if (verbose)
                std::cout << "Merging profile: " << mergeCmd << "\n";

            result = std::system(mergeCmd.c_str());
            if (result != 0) {
                std::cerr << "Warning: llvm-profdata merge failed, profile may not be optimal\n";
            } else {
                std::cout << "PGO profile generated: " << profdataOutput << "\n";
                std::cout << "Re-compile with: --pgo-use=" << profdataOutput << "\n";
            }

            // Cleanup temporary files
            std::filesystem::remove(instrOutput);
            std::filesystem::remove(instrOutput + ".o");
            std::filesystem::remove(instrOutput + "_b7_modules.c");
            std::filesystem::remove(instrOutput + ".profraw");

            return true;
        }
        return false;
    }

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
            if (!node->id.empty()) {
                importNames.insert(node->id);
                // Also probe `<module>.<name>` — `name` may itself be a
                // submodule (e.g. `from package_a import mod_a1`), not just
                // an attribute already present in the package's __init__.py.
                for (size_t i = 0; i + 1 < node->importNames.size(); i += 2) {
                    const std::string& orig = node->importNames[i];
                    if (!orig.empty() && orig != "*") {
                        importNames.insert(node->id + "." + orig);
                    }
                }
            }
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
      
      // Get Python version for standard library path
      std::string pythonVersion;
      if (!pythonPath.empty()) {
          // Try to detect version from pythonPath/include directory
          fs::path includeDir = fs::path(pythonPath) / "include";
          if (fs::exists(includeDir)) {
              for (const auto& entry : fs::directory_iterator(includeDir)) {
                  std::string filename = entry.path().filename().string();
                  if (filename.find("python") != std::string::npos) {
                      // Extract version from directory name like "python3.11"
                      size_t pos = filename.find("python");
                      if (pos != std::string::npos) {
                          pythonVersion = filename.substr(pos + 6);
                          // Remove any trailing non-numeric characters
                          while (!pythonVersion.empty() && !isdigit(pythonVersion.back())) {
                              pythonVersion.pop_back();
                          }
                          break;
                      }
                  }
              }
          }
          // Fallback: try python3-config if available
          if (pythonVersion.empty()) {
              std::string configCmd = pythonPath + "/bin/python3-config --version 2>/dev/null";
              FILE* pipe = popen(configCmd.c_str(), "r");
              if (pipe) {
                  char buf[64];
                  if (fgets(buf, sizeof(buf), pipe)) {
                      // Parse version like "Python 3.11.5"
                      for (size_t i = 0; i < strlen(buf); ++i) {
                          if (isdigit(buf[i])) {
                              pythonVersion = buf + i;
                              // Keep only major.minor
                              size_t dotCount = 0;
                              for (size_t j = 0; j < pythonVersion.length(); ++j) {
                                  if (pythonVersion[j] == '.') {
                                      dotCount++;
                                      if (dotCount > 1) {
                                          pythonVersion = pythonVersion.substr(0, j);
                                          break;
                                      }
                                  } else if (!isdigit(pythonVersion[j]) && pythonVersion[j] != '.') {
                                      pythonVersion = pythonVersion.substr(0, j);
                                      break;
                                  }
                              }
                              break;
                          }
                      }
                  }
                  pclose(pipe);
              }
          }
      } else {
          FILE* pipe = popen("python3 -c 'import sys; print(f\"{sys.version_info.major}.{sys.version_info.minor}\")' 2>/dev/null", "r");
          if (pipe) {
              char buf[64];
              if (fgets(buf, sizeof(buf), pipe)) {
                  pythonVersion = buf;
                   pythonVersion.erase(std::remove(pythonVersion.begin(), pythonVersion.end(), '\n'), pythonVersion.end());
               }
               pclose(pipe);
           }
       }
       
        // Use provided pythonPath if specified, otherwise use default location
        std::string stdlibPath = "";
        if (verbose) std::cout << "DEBUG: pythonPath=" << pythonPath << ", pythonVersion=" << pythonVersion << "\n";
        if (!pythonPath.empty()) {
            if (!pythonVersion.empty()) {
                stdlibPath = pythonPath + "/lib/python" + pythonVersion;
                if (verbose) std::cout << "DEBUG: stdlibPath from pythonPath=" << stdlibPath << "\n";
            } else {
                stdlibPath = pythonPath;
                if (verbose) std::cout << "DEBUG: stdlibPath fallback=" << stdlibPath << "\n";
            }
        } else if (!pythonVersion.empty()) {
            stdlibPath = "/usr/lib/python" + pythonVersion;
            if (verbose) std::cout << "DEBUG: stdlibPath from config=" << stdlibPath << "\n";
        }
        
        if (verbose) std::cout << "DEBUG: Final stdlibPath=" << stdlibPath << "\n";
    
    // Build list of files to compile: main file + imported modules
    std::string dir = fs::path(inputPath).parent_path().string();
    if (dir.empty()) dir = ".";

    std::string mainBasename = fs::path(inputPath).stem().string();

    std::string venvModuleDir = "";
    if (!venvPath.empty()) {
        venvModuleDir = venvLib;
    }

    // B7: discover every compile unit transitively needed by the imports
    // found above — every intermediate package level plus the leaf module,
    // for each dotted import name (see discoverDottedModule / DiscoveredModule).
    std::vector<DiscoveredModule> discovered;
    std::unordered_set<std::string> seenModules;
    for (auto& name : importNames) {
        discoverDottedModule(name, dir, venvModuleDir, stdlibPath, discovered, seenModules);
    }

    // B7: pre-parse each discovered module, both to collect its exported
    // globals (so `from X import *` can be expanded statically) and to
    // transitively discover the imports *it* makes — absolute or relative
    // (e.g. a package's __init__.py doing `from .helper import value`).
    // Newly-discovered modules are appended to `discovered`, which this
    // loop keeps scanning (index-based, not range-for, so appends made
    // during iteration are picked up — `discoverDottedModule`'s `seen` set
    // guarantees termination even with circular relative imports).
    std::unordered_map<std::string, std::vector<std::string>> importedModuleGlobals;
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
    auto scanModuleImports = [&](const ASTNode* modNode, const std::string& packageContext) {
        std::function<void(const ASTNode*)> walk = [&](const ASTNode* n) {
            if (!n) return;
            if (n->type == "Import") {
                std::stringstream ss(n->id);
                std::string tok;
                while (ss >> tok) discoverDottedModule(tok, dir, venvModuleDir, stdlibPath, discovered, seenModules);
            } else if (n->type == "ImportFrom") {
                auto resolved = resolveRelativeImport(packageContext, n->level, n->id);
                if (resolved) {
                    if (!resolved->empty())
                        discoverDottedModule(*resolved, dir, venvModuleDir, stdlibPath, discovered, seenModules);
                    for (size_t i = 0; i + 1 < n->importNames.size(); i += 2) {
                        const std::string& orig = n->importNames[i];
                        if (orig.empty() || orig == "*") continue;
                        std::string sub = resolved->empty() ? orig : (*resolved + "." + orig);
                        discoverDottedModule(sub, dir, venvModuleDir, stdlibPath, discovered, seenModules);
                    }
                }
            }
            for (const auto& c : n->children) walk(c.get());
        };
        walk(modNode);
    };
    for (size_t i = 0; i < discovered.size(); ++i) {
        std::string filePath = discovered[i].filePath;
        std::string dottedName = discovered[i].dottedName;
        std::string packageContext = packageContextOf(discovered[i]);
        if (filePath.empty()) continue; // namespace package: no source to scan
        PythonParser pp;
        auto ast = pp.parseFile(filePath);
        if (!ast) continue;
        importedModuleGlobals[dottedName] = collectTopLevelNames(ast.get());
        scanModuleImports(ast.get(), packageContext);
    }

    if (verbose) {
        std::cout << "B7: Compiling " << discovered.size() << " module(s)\n";
        for (auto& dm : discovered) {
            std::cout << "  - " << dm.dottedName
                       << (dm.isNamespacePackage ? " (namespace package)" : (": " + dm.filePath)) << "\n";
        }
    }

    // Collect module names for B7 runtime support (true dotted Python names,
    // not lossy filesystem stems — package_a/__init__.py and
    // package_a/subpkg/__init__.py must stay distinct). Built after the
    // transitive-scan loop above so it reflects every discovered module,
    // not just the ones directly imported by the main script.
    std::vector<std::string> moduleNames;
    std::unordered_set<std::string> compiledModules;
    for (auto& dm : discovered) {
        moduleNames.push_back(dm.dottedName);
        compiledModules.insert(dm.dottedName);
    }

    // Compile each .py file to an LLVM module: main module first, then every
    // discovered import (package levels, submodules, namespace packages).
    std::vector<std::unique_ptr<llvm::Module>> modules;
    llvm::LLVMContext context;

    {
        PythonParser parser;
        auto ast = parser.parseFile(inputPath);
        if (!ast) {
            std::cerr << "Warning: Failed to parse " << inputPath << ", skipping\n";
            return false;
        }
        ModuleIR ir;
        ir.moduleName = mainBasename;
        lowerAST(ast.get(), ir, compiledModules, importedModuleGlobals, inputPath);
        Codegen codegen;
        auto module = codegen.generate(ir, context, "pyc_" + mainBasename, debugInfo);
        if (!module) {
            std::cerr << "Warning: Codegen failed for " << inputPath << "\n";
            return false;
        }
        llvm::Function* entryFunc = module->getFunction("__module__");
        if (entryFunc) entryFunc->setName("pyc_user_main");
        if (verbose) std::cout << "  Generated LLVM module for " << mainBasename << " (main)\n";
        modules.push_back(std::move(module));
    }

    for (auto& dm : discovered) {
        std::string sanitized = sanitizeModuleIdent(dm.dottedName);
        std::unique_ptr<llvm::Module> module;

        if (dm.isNamespacePackage) {
            // PEP 420 namespace package: no __init__.py, so there's no
            // source to compile. Synthesize a trivial module whose entry
            // point just returns an empty dict, so pyc_run_module and the
            // parent/child attribute wiring treat it exactly like a real
            // package (e.g. `namespace_pkg.mod_c1` still resolves).
            ModuleIR ir;
            ir.moduleName = sanitized;
            ir.addFunction("__module__", {});
            std::string modDict = "t0";
            ir.addInstruction("__module__", "call", {"PyDict_New"}, modDict);
            ir.addInstruction("__module__", "ret", {modDict});

            Codegen codegen;
            module = codegen.generate(ir, context, "pyc_" + sanitized, debugInfo);
            if (!module) {
                std::cerr << "Warning: Codegen failed for namespace package " << dm.dottedName << ", skipping\n";
                continue;
            }
        } else {
            PythonParser parser;
            auto ast = parser.parseFile(dm.filePath);
            if (!ast) {
                std::cerr << "Warning: Failed to parse " << dm.filePath << ", skipping\n";
                continue;
            }
            ModuleIR ir;
            ir.moduleName = sanitized;
            lowerAST(ast.get(), ir, compiledModules, importedModuleGlobals, dm.filePath, packageContextOf(dm));

            Codegen codegen;
            module = codegen.generate(ir, context, "pyc_" + sanitized, debugInfo);
            if (!module) {
                std::cerr << "Warning: Codegen failed for " << dm.filePath << ", skipping\n";
                continue;
            }
        }

        // B7: rename the module's entry point ("__module__") to a symbol
        // unique across the whole link. Uses the sanitized identifier
        // (dots aren't valid in C/LLVM symbol names) — the module registry
        // below keeps the true dotted name as its string lookup key.
        llvm::Function* entryFunc = module->getFunction("__module__");
        if (entryFunc) entryFunc->setName("__module__" + sanitized);

        if (verbose) std::cout << "  Generated LLVM module for " << dm.dottedName << "\n";
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
    
    // Declare extern functions for each module entry point. The C symbol
    // uses the sanitized identifier (dots aren't valid in C identifiers);
    // the registry's string key below keeps the true dotted Python name.
    for (auto& name : moduleNames) {
        b7CSource += "extern void* __module__" + sanitizeModuleIdent(name) + "(void);\n";
    }
    b7CSource += "\n";

    // Define the module registry. `parent` (dotted name of the enclosing
    // package, or "" for a top-level module) drives pyc_run_module's
    // parent/child attribute wiring — see below.
    b7CSource += "typedef struct {\n";
    b7CSource += "    const char* name;\n";
    b7CSource += "    const char* parent;\n";
    b7CSource += "    void* (*entry)(void);\n";
    b7CSource += "    int loading;\n";
    b7CSource += "} pyc_module_entry;\n\n";

    b7CSource += "static pyc_module_entry pyc_modules[] = {\n";
    for (auto& dm : discovered) {
        b7CSource += "    {\"" + dm.dottedName + "\", \"" + dm.parentDottedName + "\", __module__" +
                      sanitizeModuleIdent(dm.dottedName) + ", 0},\n";
    }
    b7CSource += "    {NULL, NULL, NULL, 0}\n";
    b7CSource += "};\n\n";
    
    // Generate the pyc_run_module function - accepts PyObject* (Python string)
    b7CSource += "extern const char* PyStr_AsUTF8(void* obj);\n";
    b7CSource += "extern void* PyUnicode_FromString(const char* s);\n";
    b7CSource += "extern void* pyc_get_sys_modules(void);\n";
    b7CSource += "extern void* PyDict_GetItem(void* dict, void* key);\n";
    b7CSource += "extern void  Py_DECREF(void* obj);\n";
    b7CSource += "extern void  pyc_register_module(const char* name, void* module_dict);\n";
    b7CSource += "extern void* Pyc_SetItem(void* obj, void* key, void* val);\n";
    b7CSource += "void* pyc_run_module(void* moduleNameObj) {\n";
    b7CSource += "    const char* moduleName = PyStr_AsUTF8(moduleNameObj);\n";
    b7CSource += "    if (!moduleName) return NULL;\n";
    b7CSource += "    // sys.modules-backed cache: a module's top-level code runs\n";
    b7CSource += "    // at most once, matching CPython import semantics.\n";
    b7CSource += "    void* modules = pyc_get_sys_modules();\n";
    b7CSource += "    if (modules) {\n";
    b7CSource += "        void* nameKey = PyUnicode_FromString(moduleName);\n";
    b7CSource += "        void* cached = PyDict_GetItem(modules, nameKey);\n";
    b7CSource += "        Py_DECREF(nameKey);\n";
    b7CSource += "        Py_DECREF(modules);\n";
    b7CSource += "        if (cached) return cached;\n";
    b7CSource += "    }\n";
    b7CSource += "    for (int i = 0; pyc_modules[i].name != NULL; i++) {\n";
    b7CSource += "        if (strcmp(pyc_modules[i].name, moduleName) == 0) {\n";
    b7CSource += "            // A package's __init__.py importing its own submodule\n";
    b7CSource += "            // (directly or via `from .sub import x`) makes the\n";
    b7CSource += "            // submodule's parent-wiring below recurse back into this\n";
    b7CSource += "            // same module while it's still running its own top-level\n";
    b7CSource += "            // code (not yet cached). Without this guard that would\n";
    b7CSource += "            // re-run entry() and execute the module's body twice. On\n";
    b7CSource += "            // the reentrant call, skip straight to returning NULL —\n";
    b7CSource += "            // the parent/child attribute wiring is just skipped for\n";
    b7CSource += "            // this one (rare) case; the outer, original call still\n";
    b7CSource += "            // completes and registers the module normally.\n";
    b7CSource += "            if (pyc_modules[i].loading) return NULL;\n";
    b7CSource += "            pyc_modules[i].loading = 1;\n";
    b7CSource += "            void* result = pyc_modules[i].entry();\n";
    b7CSource += "            pyc_modules[i].loading = 0;\n";
    b7CSource += "            pyc_register_module(moduleName, result);\n";
    b7CSource += "            // Wire this module onto its parent package as an\n";
    b7CSource += "            // attribute (loading \"package_a.mod_a1\" sets\n";
    b7CSource += "            // package_a.mod_a1), matching CPython's submodule\n";
    b7CSource += "            // import behavior. Loads (and caches) the parent first.\n";
    b7CSource += "            if (pyc_modules[i].parent[0] != '\\0') {\n";
    b7CSource += "                void* parentNameObj = PyUnicode_FromString(pyc_modules[i].parent);\n";
    b7CSource += "                void* parentDict = pyc_run_module(parentNameObj);\n";
    b7CSource += "                Py_DECREF(parentNameObj);\n";
    b7CSource += "                if (parentDict) {\n";
    b7CSource += "                    const char* leaf = pyc_modules[i].name + strlen(pyc_modules[i].parent) + 1;\n";
    b7CSource += "                    void* leafKey = PyUnicode_FromString(leaf);\n";
    b7CSource += "                    Pyc_SetItem(parentDict, leafKey, result);\n";
    b7CSource += "                    Py_DECREF(leafKey);\n";
    b7CSource += "                }\n";
    b7CSource += "            }\n";
    b7CSource += "            return result;\n";
    b7CSource += "        }\n";
    b7CSource += "    }\n";
    b7CSource += "    // Module not found - unsupported\n";
    b7CSource += "    return NULL;\n";
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
    
    // LTO: link precompiled runtime bitcode before optimization at -O1 and above.
    // True -O0 skips bitcode LTO so IR stays raw codegen + external runtime.
    // O4+: add PGO instrumentation pass before optimization.
    const bool useLTO = (optLevel >= 1);
    if (useLTO) {
        std::string rtBitcodePath = PYC_RUNTIME_BC;
        codegen.linkRuntimeBitcode(module.get(), rtBitcodePath);
        if (verbose)
            std::cout << "Linked runtime bitcode for LTO optimization\n";
    } else if (verbose) {
        std::cout << "-O0: skipping runtime bitcode LTO (true O0 debug mode)\n";
    }

    codegen.optimize(module.get(), optLevel, mcpu, march, pgoInstrument, pgoProfile);
    if (emitLLVM) {
        if (codegen.emitLLVM(module.get(), outputPath + ".ll")) {
            std::cout << "Emitted LLVM IR " << outputPath << ".ll (-O" << optLevel << ")\n";
            return true;
        }
        return false;
    }
    if (emitASM) {
        if (codegen.emitAssembly(module.get(), outputPath + ".s", mcpu, march)) {
            std::cout << "Emitted assembly " << outputPath << ".s (-O" << optLevel << ")\n";
            return true;
        }
        return false;
    }
    // O5 full LTO: emit bitcode, link with -flto (full LTO)
    if (optLevel >= 5) {
        std::string bitcodePath = outputPath + ".bc";
        if (codegen.emitBitcode(module.get(), bitcodePath)) {
            std::cout << "Emitted bitcode " << bitcodePath << " (-O5 full LTO)\n";
            std::string linkCmd = "clang++-22 ";
            if (useStatic) linkCmd += "-static ";
            linkCmd += (debugInfo ? "-Wl,--gc-sections " : "-s -Wl,--gc-sections -Wl,--strip-all ");

             std::string sourceDir = PYC_SOURCE_DIR;
             std::string runtimeLink = " " + sourceDir + "/src/runtime/Runtime.cpp";
             for (const auto& libdir : {"./build", "../build", ".", "/usr/local/lib", "/usr/lib"}) {
                 std::string libpath = std::string(libdir) + "/libpycrt.a";
                 if (std::ifstream(libpath).good()) {
                     runtimeLink = " -L" + std::string(libdir) + " -lpycrt";
                     break;
                 }
             }
             std::string pythonIncludes = "";
             std::string pythonLibLink = "";
             
             if (!pythonPath.empty()) {
                 if (!pythonVersion.empty()) {
                     pythonIncludes = "-I" + pythonPath + "/include/python" + pythonVersion;
                     std::string libDir = pythonPath + "/lib";
                     if (fs::exists(libDir)) {
                         pythonLibLink = "-L" + libDir + " -lpython" + pythonVersion;
                     }
                 } else {
                     pythonIncludes = "-I" + pythonPath + "/include";
                     std::string libDir = pythonPath + "/lib";
                     if (fs::exists(libDir)) {
                         pythonLibLink = "-L" + libDir + " -lpython";
                     }
                 }
             } else {
                 FILE* pipe = popen("python3-config --includes 2>/dev/null | grep -o '\\-I[^ ]*' | head -1", "r");
                 if (pipe) {
                     char buf[256];
                     if (fgets(buf, sizeof(buf), pipe)) {
                         buf[strcspn(buf, "\n")] = 0;
                         pythonIncludes = buf;
                     }
                     pclose(pipe);
                 }
                 FILE* pipe2 = popen("python3-config --ldflags 2>/dev/null | grep -o '\\-lpython[^ ]*' | head -1", "r");
                 if (pipe2) {
                     char buf[256];
                     if (fgets(buf, sizeof(buf), pipe2)) {
                         buf[strcspn(buf, "\n")] = 0;
                         pythonLibLink = buf;
                     }
                     pclose(pipe2);
                 }
             }
             // Full LTO: -flto (not -flto=thin), link bitcode directly
             // -flto-partitions=0 enables parallel full LTO codegen (ld.lld only, auto-detect CPU count)
             // Use ld.lld-22 for proper LTO support
              linkCmd = "clang++-22 -fuse-ld=lld-22 ";
              linkCmd += (debugInfo ? "-Wl,--gc-sections " : "-s -Wl,--gc-sections -Wl,--strip-all ");
             linkCmd += bitcodePath + " -flto -flto-partitions=0 -Wl,--allow-multiple-definition ";
             linkCmd += "-x c " + b7CFile + " -x none -I" + sourceDir + "/include " +
                 pythonIncludes + " " + sourceDir + "/src/runtime/MainWrapper.cpp" +
                 runtimeLink + " " + pythonLibLink + " -lpcre2-8 -lmpdec -o " + outputPath + " -O3";
            if (!pgoProfile.empty()) {
                linkCmd += " -fprofile-use=" + pgoProfile;
            }
            if (pgoInstrument == "instrument") {
                std::string instrCmd = linkCmd + " -fprofile-instr-generate=" + outputPath + ".profraw";
                if (std::system(instrCmd.c_str()) == 0) {
                    if (verbose)
                        std::cout << "PGO instrumentation linked (run binary to generate profile)\n";
                } else {
                    std::cerr << "PGO instrumentation link failed. Run manually: " << instrCmd << std::endl;
                    return false;
                }
                return true;
            }
            if (debugInfo) linkCmd += " -g";
            if (std::system(linkCmd.c_str()) == 0) {
                std::cout << "Linked with full LTO to " << outputPath << " (-O5)\n";
            } else {
                std::cerr << "Full LTO link failed. Run manually: " << linkCmd << std::endl;
                return false;
            }
            return true;
        }
        return false;
    }
    if (codegen.emitObject(module.get(), outputPath + ".o", mcpu, march)) {
        std::cout << "Generated object " << outputPath << ".o (-O" << optLevel << ")\n";
        std::string linkCmd = "clang++ ";
        if (useStatic) linkCmd += "-static ";
            linkCmd += (debugInfo ? "-Wl,--gc-sections " : "-s -Wl,--gc-sections -Wl,--strip-all ");

             std::string sourceDir = PYC_SOURCE_DIR;
        std::string runtimeLink = " " + sourceDir + "/src/runtime/Runtime.cpp";
        for (const auto& libdir : {"./build", "../build", ".", "/usr/local/lib", "/usr/lib"}) {
            std::string libpath = std::string(libdir) + "/libpycrt.a";
            if (std::ifstream(libpath).good()) {
                runtimeLink = " -L" + std::string(libdir) + " -lpycrt";
                break;
            }
        }
        std::string pythonIncludes = "";
        std::string pythonLibPath = "";
        std::string pythonLibName = "-lpython";
        
        // Compute Python version if pythonPath is specified
        std::string pythonVersion;
        if (!pythonPath.empty()) {
            fs::path includeDir = fs::path(pythonPath) / "include";
            if (fs::exists(includeDir)) {
                for (const auto& entry : fs::directory_iterator(includeDir)) {
                    std::string filename = entry.path().filename().string();
                    if (filename.find("python") != std::string::npos) {
                        size_t pos = filename.find("python");
                        if (pos != std::string::npos) {
                            pythonVersion = filename.substr(pos + 6);
                            while (!pythonVersion.empty() && !isdigit(pythonVersion.back())) {
                                pythonVersion.pop_back();
                            }
                            break;
                        }
                    }
                }
            }
            if (pythonVersion.empty()) {
                std::string configCmd = pythonPath + "/bin/python3-config --version 2>/dev/null";
                FILE* pipe = popen(configCmd.c_str(), "r");
                if (pipe) {
                    char buf[64];
                    if (fgets(buf, sizeof(buf), pipe)) {
                        for (size_t i = 0; i < strlen(buf); ++i) {
                            if (isdigit(buf[i])) {
                                pythonVersion = buf + i;
                                size_t dotCount = 0;
                                for (size_t j = 0; j < pythonVersion.length(); ++j) {
                                    if (pythonVersion[j] == '.') {
                                        dotCount++;
                                        if (dotCount > 1) {
                                            pythonVersion = pythonVersion.substr(0, j);
                                            break;
                                        }
                                    } else if (!isdigit(pythonVersion[j]) && pythonVersion[j] != '.') {
                                        pythonVersion = pythonVersion.substr(0, j);
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }
                    pclose(pipe);
                }
            }
        }
        
        if (!pythonPath.empty()) {
            if (!pythonVersion.empty()) {
                pythonIncludes = "-I" + pythonPath + "/include/python" + pythonVersion;
                std::string libDir = pythonPath + "/lib";
                if (fs::exists(libDir)) {
                    pythonLibPath = "-L" + libDir;
                    pythonLibName = "-lpython" + pythonVersion;
                }
            } else {
                pythonIncludes = "-I" + pythonPath + "/include";
                std::string libDir = pythonPath + "/lib";
                if (fs::exists(libDir)) {
                    pythonLibPath = "-L" + libDir;
                }
            }
        } else {
            FILE* pipe = popen("python3-config --includes 2>/dev/null | grep -o '\\-I[^ ]*' | head -1", "r");
            if (pipe) {
                char buf[256];
                if (fgets(buf, sizeof(buf), pipe)) {
                    buf[strcspn(buf, "\n")] = 0;
                    pythonIncludes = buf;
                }
                pclose(pipe);
            }
        }
        
        std::string pythonLibLink = "";
        if (verbose) std::cout << "DEBUG: pythonLibPath=" << pythonLibPath << ", pythonLibName=" << pythonLibName << "\n";
        if (!pythonLibPath.empty()) {
            pythonLibLink = pythonLibPath + " " + pythonLibName;
            if (verbose) std::cout << "DEBUG: pythonLibLink from path=" << pythonLibLink << "\n";
        } else {
            FILE* pipe2 = popen("python3-config --ldflags 2>/dev/null | grep -o '\\-lpython[^ ]*' | head -1", "r");
            if (pipe2) {
                char buf[256];
                if (fgets(buf, sizeof(buf), pipe2)) {
                    buf[strcspn(buf, "\n")] = 0;
                    pythonLibLink = buf;
                }
                pclose(pipe2);
            }
        }
        
        // Handle dynamic linking if requested
        std::string moduleObjects;
        if (dynamicLink && !moduleNames.empty()) {
            // Create shared objects for each module
            for (const auto& modName : moduleNames) {
                std::string soPath = outputPath + "_" + modName + ".so";
                std::string soCmd = "clang++ -shared -fPIC " + outputPath + ".o -o " + soPath;
                if (std::system(soCmd.c_str()) == 0) {
                    moduleObjects += soPath + " ";
                }
            }
        }
        
        // -O1 and above: -flto=thin so the object (with linked runtime BC) can finalize LTO.
        // -O0: no -flto; link libpycrt/Runtime.cpp as a normal archive/source.
        // --allow-multiple-definition only needed when LTO may duplicate runtime symbols.
        // -O4/O5: add PGO profile-use flag when a profile is provided.
        // -O4: add ThinLTO-specific link flags for better parallelism and optimization.
        if (useLTO) {
            linkCmd += outputPath + ".o -flto=thin -Wl,--allow-multiple-definition ";
            if (optLevel >= 4) {
                linkCmd += "-flto-jobs=0 ";
            }
        } else {
            linkCmd += outputPath + ".o ";
        }
        linkCmd += "-x c " + b7CFile + " -x none -I" + sourceDir + "/include " +
            pythonIncludes + " " + sourceDir + "/src/runtime/MainWrapper.cpp" +
            runtimeLink + " " + moduleObjects + " " + pythonLibLink + " -lpcre2-8 -lmpdec -o " + outputPath + " -O" + std::to_string(std::min(optLevel, 3));
        if (optLevel >= 4 && !pgoProfile.empty()) {
            linkCmd += " -fprofile-use=" + pgoProfile;
        }
        if (optLevel >= 4 && pgoInstrument == "instrument") {
            // PGO instrumentation: re-link with -fprofile-instr-generate to produce .profraw
            std::string instrCmd = linkCmd + " -fprofile-instr-generate=" + outputPath + ".profraw";
            if (std::system(instrCmd.c_str()) == 0) {
                if (verbose)
                    std::cout << "PGO instrumentation linked (run binary to generate profile)\n";
            } else {
                std::cerr << "PGO instrumentation link failed. Run manually: " << instrCmd << std::endl;
                return false;
            }
            return true;
        }
        if (debugInfo) linkCmd += " -g";
        if (std::system(linkCmd.c_str()) == 0) {
            std::cout << "Linked with runtime to " << outputPath
                      << " (static=" << useStatic << ", lto=" << (useLTO ? 1 : 0) << ")\n";
        } else {
            std::cerr << "Link failed. Run manually: " << linkCmd << std::endl;
            return false;
        }
        return true;
    }
    return false;
}

} // namespace pyc
