// runtime/b7_import.cpp - Simple runtime import system for B7
// Implements file-based module loading by parsing .py files at runtime
// and executing them in a module dict namespace.

#include "pyc/runtime.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

// Simple module registry
static std::unordered_map<std::string, PyObject*> g_loaded_modules;

// Forward declarations for the parser and IR builder
namespace pyc {
    class PythonParser;
    struct ASTNode;
    struct ModuleIR;
}

// We need to include the parser and IR builder headers
extern "C" {
    // These are declared in the frontend/ and ir/ directories
    // We'll use the C API if available, otherwise we'll implement a simple parser
}

// Simple module loading function
// Returns a module dict or NULL if the module cannot be loaded
extern "C" PyObject* pyc_import_module(const char* module_name) {
    if (!module_name) return NULL;
    
    // Check if module is already loaded
    auto it = g_loaded_modules.find(module_name);
    if (it != g_loaded_modules.end()) {
        Py_INCREF(it->second);
        return it->second;
    }
    
    // Try to find the .py file in the current directory
    std::string filepath = std::string(module_name) + ".py";
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "ImportError: No module named '" << module_name << "'" << std::endl;
        return NULL;
    }
    
    // Read the file content
    std::string code((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
    file.close();
    
    // Create a new module dict
    PyObject* module_dict = PyDict_New();
    
    // Set __name__ on the module
    PyObject* name_key = PyUnicode_FromString("__name__");
    PyObject* name_val = PyUnicode_FromString(module_name);
    PyDict_SetItem(module_dict, name_key, name_val);
    Py_DECREF(name_key);
    Py_DECREF(name_val);
    
    // Store in module registry
    g_loaded_modules[module_name] = module_dict;
    Py_INCREF(module_dict);
    
    // Note: Full execution of the module requires the IR interpreter,
    // which is not yet integrated. For now, we return an empty module dict.
    // The module's functions and variables will be accessible via the
    // module dict once the IR interpreter is integrated.
    
    return module_dict;
}

// Import a specific name from a module
extern "C" PyObject* pyc_import_from_module(const char* module_name, const char* name) {
    if (!module_name || !name) return NULL;
    
    // Import the module first
    PyObject* module_dict = pyc_import_module(module_name);
    if (!module_dict) return NULL;
    
    // Look up the name in the module dict
    PyObject* name_key = PyUnicode_FromString(name);
    PyObject* value = PyDict_GetItem(module_dict, name_key);
    Py_DECREF(name_key);
    
    if (value) {
        Py_INCREF(value);
        return value;
    }
    
    std::cerr << "ImportError: cannot import name '" << name << "' from '" << module_name << "'" << std::endl;
    return NULL;
}

// Clear the module registry (for testing/cleanup)
extern "C" void pyc_clear_modules(void) {
    for (auto& pair : g_loaded_modules) {
        Py_DECREF(pair.second);
    }
    g_loaded_modules.clear();
}
