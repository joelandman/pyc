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
static std::vector<std::string> g_sys_path = {".", "./modules", "./lib"};

// ===== Helper: Check if path is a package =====

static bool is_package(const std::string& package_path) {
    std::ifstream init_file(package_path + "/__init__.py");
    return init_file.is_open();
}

// ===== Helper: Find module file in sys.path =====

static std::string find_module_file(const std::string& module_name) {
    // Try as a module file first
    std::string filename = module_name + ".py";
    
    // Search through sys.path
    for (auto& path : g_sys_path) {
        std::string full_path = path + "/" + filename;
        std::ifstream test(full_path);
        if (test.is_open()) {
            test.close();
            return full_path;
        }
    }
    
    // Fall back to current directory
    std::ifstream test(filename);
    if (test.is_open()) {
        test.close();
        return filename;
    }
    
    return "";
}

// ===== Helper: Find package directory in sys.path =====

static std::string find_package_dir(const std::string& package_name) {
    // Search through sys.path
    for (auto& path : g_sys_path) {
        std::string package_path = path + "/" + package_name;
        if (is_package(package_path)) {
            return package_path;
        }
    }
    
    // Fall back to current directory
    if (is_package(package_name)) {
        return package_name;
    }
    
    return "";
}

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
    
    // Register globals() and locals() builtins
    interpreter.register_builtin("globals", pyc::ir::builtin_globals_impl);
    interpreter.register_builtin("locals", pyc::ir::builtin_locals_impl);
    
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

void set_sys_path(const std::vector<std::string>& paths) {
    g_sys_path = paths;
}

std::vector<std::string> get_sys_path() {
    return g_sys_path;
}

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
    
    // Check if this is a package (directory with __init__.py)
    std::string package_dir = find_package_dir(module_name);
    if (!package_dir.empty()) {
        // Load __init__.py from the package directory
        std::string init_file = package_dir + "/__init__.py";
        std::ifstream file(init_file);
        if (!file.is_open()) {
            std::cerr << "Warning: Cannot open package __init__.py: " << init_file << std::endl;
            return module_dict;
        }
        
        std::string code((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
        file.close();
        
        // Parse and execute __init__.py
        auto ast = parse_source(code);
        if (!ast) {
            std::cerr << "Error: Failed to parse package __init__.py: " << module_name << std::endl;
            return module_dict;
        }
        
        auto ir_mod = build_ir(ast);
        if (!ir_mod) {
            std::cerr << "Error: Failed to build IR for package: " << module_name << std::endl;
            return module_dict;
        }
        
        g_module_irs[module_name] = ir_mod;
        execute_module(ir_mod, module_dict);
        
        // Add package to sys.path for submodule imports
        // (This would be handled by the interpreter when accessing submodules)
        
        return module_dict;
    }
    
    // Try to load the module from a file using sys.path
    std::string filepath = find_module_file(module_name);
    if (filepath.empty()) {
        std::cerr << "Warning: Cannot find module file: " << module_name << std::endl;
        return module_dict;
    }
    
    // Read the file content
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Warning: Cannot open module file: " << filepath << std::endl;
        return module_dict;
    }
    
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
    
    // Check if name is a submodule (package.submodule)
    std::string full_module_name = module_name + "." + name;
    auto sub_it = g_loaded_modules.find(full_module_name);
    if (sub_it != g_loaded_modules.end()) {
        pyc_ref_inc(sub_it->second);
        return sub_it->second;
    }
    
    // Try to load the submodule
    auto* submodule_dict = import_module(full_module_name);
    if (submodule_dict) {
        // Store it in the parent module's dict
        if (module_dict->dict_entries) {
            (*module_dict->dict_entries)[name] = submodule_dict;
            pyc_ref_inc(submodule_dict);
        }
        return submodule_dict;
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
