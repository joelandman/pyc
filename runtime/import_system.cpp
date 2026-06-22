// runtime/import_system.cpp - Python import system implementation
// Implements file-based module loading, parsing, and execution

#include "runtime/import_system.h"
#include "runtime/object.h"
#include "runtime/libpyc_runtime.h"
#include "frontend/parser.h"
#include "ir/builder.h"
#include "ir/interpreter.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace pyc::runtime {

// ===== Module Loading State =====

static std::unordered_map<std::string, PyObject*> g_loaded_modules;
static std::unordered_map<std::string, std::shared_ptr<pyc::ir::IRModule>> g_module_irs;

// ===== Helper: Parse Python source code =====

static std::shared_ptr<pyc::ast::Module> parse_source(const std::string& source) {
    auto tokens = pyc::lexer::tokenize(source);
    pyc::parser::Parser parser(tokens);
    return parser.parse();
}

// ===== Helper: Build IR from AST =====

static std::shared_ptr<pyc::ir::IRModule> build_ir(const std::shared_ptr<pyc::ast::Module>& ast) {
    pyc::ir::builder::IRBuilder builder;
    builder.build(*ast);
    return std::shared_ptr<pyc::ir::IRModule>(builder.module.release());
}

// ===== Helper: Execute IR module in a namespace =====

static void execute_module(std::shared_ptr<pyc::ir::IRModule> ir_mod, PyObject* module_dict) {
    pyc::ir::Interpreter interpreter;
    
    // Set up interpreter with module globals
    auto& globals = interpreter.globals();
    if (module_dict && module_dict->dict_entries) {
        for (auto& [key, val] : *module_dict->dict_entries) {
            globals[key] = pyc::ir::PyValue(key);
        }
    }
    
    // Execute the module
    try {
        interpreter.run(ir_mod);
    } catch (const std::exception& e) {
        std::cerr << "Error executing module: " << e.what() << std::endl;
    }
}

// ===== Public API =====

PyObject* import_module(const std::string& module_name) {
    // Check if module is already loaded
    auto it = g_loaded_modules.find(module_name);
    if (it != g_loaded_modules.end()) {
        pyc_ref_inc(it->second);
        return it->second;
    }
    
    // Create a new module as a dict
    auto* module_dict = PyObjectFactory::create_dict(nullptr);
    g_loaded_modules[module_name] = module_dict;
    pyc_ref_inc(module_dict);
    
    // Try to load the module from a file
    std::string filename = module_name + ".py";
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Warning: Cannot open module file: " << filename << std::endl;
        return module_dict;
    }
    
    // Read the file content
    std::string code((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    file.close();
    
    // Parse the source code
    auto ast = parse_source(code);
    if (!ast) {
        std::cerr << "Error: Failed to parse module: " << module_name << std::endl;
        return module_dict;
    }
    
    // Build IR from AST
    auto ir_mod = build_ir(ast);
    if (!ir_mod) {
        std::cerr << "Error: Failed to build IR for module: " << module_name << std::endl;
        return module_dict;
    }
    
    // Store IR for later use (e.g., for "from" imports)
    g_module_irs[module_name] = ir_mod;
    
    // Execute the module in the module namespace
    execute_module(ir_mod, module_dict);
    
    return module_dict;
}

PyObject* import_from_module(const std::string& module_name, const std::string& name) {
    // Import the module first
    auto* module_dict = import_module(module_name);
    if (!module_dict || !module_dict->dict_entries) {
        return nullptr;
    }
    
    // Look up the name in the module dict
    auto it = module_dict->dict_entries->find(name);
    if (it != module_dict->dict_entries->end()) {
        pyc_ref_inc(it->second);
        return it->second;
    }
    
    // Name not found in module
    std::cerr << "Warning: Name '" << name << "' not found in module '" << module_name << "'" << std::endl;
    return nullptr;
}

void clear_loaded_modules() {
    for (auto& [name, obj] : g_loaded_modules) {
        pyc_ref_dec(obj);
    }
    g_loaded_modules.clear();
    g_module_irs.clear();
}

} // namespace pyc::runtime
