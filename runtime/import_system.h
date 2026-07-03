#pragma once
#include "runtime/object.h"
#include <string>
#include <vector>
#include <memory>

namespace pyc::runtime {

// Import a module by name
// Returns the module dict (creates and caches it)
PyObject* import_module(const std::string& module_name);

// Import a specific name from a module
// Returns the object or nullptr if not found
PyObject* import_from_module(const std::string& module_name, const std::string& name);

// Import a module using relative import resolution
// module_name: the module to import (may be empty for "from . import x")
// level: number of dots (1 = current package, 2 = parent package, etc.)
// parent_module: the fully qualified name of the module containing the import
// Returns the module dict or a dict with the imported names
PyObject* import_relative(const std::string& module_name, int level, const std::string& parent_module);

// Clear all loaded modules (for testing/cleanup)
void clear_loaded_modules();

// Set/get sys.path for module search
void set_sys_path(const std::vector<std::string>& paths);
std::vector<std::string> get_sys_path();

} // namespace pyc::runtime
