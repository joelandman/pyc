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

// Clear all loaded modules (for testing/cleanup)
void clear_loaded_modules();

// Set/get sys.path for module search
void set_sys_path(const std::vector<std::string>& paths);
std::vector<std::string> get_sys_path();

} // namespace pyc::runtime
