// runtime/libpyc_runtime.cpp - Runtime functions for LLVM codegen
// Implements the runtime functions called from generated LLVM IR

#include "runtime/libpyc_runtime.h"
#include "runtime/object.h"
#include "runtime/import_system.h"
#include <cmath>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <fstream>

namespace pyc::runtime {

// ===== Helper: Convert pyc_obj_t to PyObject* =====

static PyObject* to_pyobj(pyc_obj_t obj) {
    return reinterpret_cast<PyObject*>(obj);
}

static pyc_obj_t from_pyobj(PyObject* obj) {
    return reinterpret_cast<pyc_obj_t>(obj);
}

// ===== Helper: Object to string =====

static std::string py_object_to_string(PyObject* obj) {
    if (!obj) return "None";
    
    uint32_t type_mask = obj->type_object & 0xFF;
    
    switch (type_mask) {
        case TYPE_INT:
            return std::to_string(static_cast<int64_t>(obj->data));
        case TYPE_FLOAT: {
            double val = *reinterpret_cast<double*>(obj->data);
            std::stringstream ss;
            ss << std::fixed << std::setprecision(6) << val;
            return ss.str();
        }
        case TYPE_STR:
            if (obj->str_value) return *obj->str_value;
            return "";
        case TYPE_BOOL:
            return static_cast<bool>(obj->data) ? "True" : "False";
        case TYPE_LIST:
            if (obj->list_elements) {
                std::string result = "[";
                for (size_t i = 0; i < obj->list_elements->size(); ++i) {
                    if (i > 0) result += ", ";
                    result += py_object_to_string((*obj->list_elements)[i]);
                }
                result += "]";
                return result;
            }
            return "[]";
        case TYPE_DICT:
            if (obj->dict_entries) {
                std::string result = "{";
                bool first = true;
                for (auto& [k, v] : *obj->dict_entries) {
                    if (!first) result += ", ";
                    result += "\"" + k + "\": " + py_object_to_string(v);
                    first = false;
                }
                result += "}";
                return result;
            }
            return "{}";
        case TYPE_NONE:
            return "None";
        default:
            return "<object>";
    }
}

// ===== Object Creation =====

pyc_obj_t pyc_codegen_new_object(pyc_type_kind_t type_kind) {
    auto* obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(type_kind);
    obj->data = 0;
    obj->str_value = nullptr;
    obj->func_callable = nullptr;
    obj->list_elements = nullptr;
    obj->dict_entries = nullptr;
    obj->instance_attrs = nullptr;
    obj->next = nullptr;
    PyObjectFactory::register_object(obj);
    return from_pyobj(obj);
}

pyc_obj_t pyc_new_list() {
    auto* obj = PyObjectFactory::create_list(nullptr);
    return from_pyobj(obj);
}

pyc_obj_t pyc_new_dict() {
    auto* obj = PyObjectFactory::create_dict(nullptr);
    return from_pyobj(obj);
}

pyc_obj_t pyc_new_type(pyc_type_kind_t type_kind) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(type_kind);
    obj->data = 0;
    obj->str_value = nullptr;
    obj->func_callable = nullptr;
    obj->list_elements = nullptr;
    obj->dict_entries = nullptr;
    obj->instance_attrs = nullptr;
    obj->next = nullptr;
    PyObjectFactory::register_object(obj);
    return from_pyobj(obj);
}

// ===== List Operations =====

pyc_obj_t pyc_list_get(pyc_obj_t list_obj, int64_t index) {
    auto* lst = to_pyobj(list_obj);
    if (!lst || !lst->list_elements) return nullptr;
    
    if (index < 0) index = lst->list_elements->size() + index;
    if (index < 0 || index >= static_cast<int64_t>(lst->list_elements->size())) {
        return nullptr;
    }
    
    auto* elem = (*lst->list_elements)[static_cast<size_t>(index)];
    pyc_ref_inc(elem);
    return from_pyobj(elem);
}

void pyc_list_set(pyc_obj_t list_obj, int64_t index, pyc_obj_t value) {
    auto* lst = to_pyobj(list_obj);
    if (!lst || !lst->list_elements) return;
    
    if (index < 0) index = lst->list_elements->size() + index;
    if (index >= 0 && index < static_cast<int64_t>(lst->list_elements->size())) {
        (*lst->list_elements)[static_cast<size_t>(index)] = to_pyobj(value);
    }
}

pyc_obj_t pyc_range_list(int64_t start, int64_t stop, int64_t step) {
    auto* result = PyObjectFactory::create_list(nullptr);
    if (!result || !result->list_elements) return from_pyobj(result);
    
    if (step == 0) step = 1;
    
    if (step > 0) {
        for (int64_t i = start; i < stop; i += step) {
            auto* int_obj = PyObjectFactory::create_int(nullptr, i);
            result->list_elements->push_back(int_obj);
        }
    } else {
        for (int64_t i = start; i > stop; i += step) {
            auto* int_obj = PyObjectFactory::create_int(nullptr, i);
            result->list_elements->push_back(int_obj);
        }
    }
    
    return from_pyobj(result);
}

// ===== String Operations =====

const char* pyc_str_value(pyc_obj_t str_obj) {
    auto* obj = to_pyobj(str_obj);
    if (!obj || !obj->str_value) return "";
    return obj->str_value->c_str();
}

pyc_obj_t pyc_type_name(pyc_obj_t obj) {
    auto* py_obj = to_pyobj(obj);
    if (!py_obj) return from_pyobj(PyObjectFactory::create_str(nullptr, "NoneType"));
    
    uint32_t type_mask = py_obj->type_object & 0xFF;
    std::string type_name;
    
    switch (type_mask) {
        case TYPE_INT: type_name = "<class 'int'>"; break;
        case TYPE_FLOAT: type_name = "<class 'float'>"; break;
        case TYPE_STR: type_name = "<class 'str'>"; break;
        case TYPE_BOOL: type_name = "<class 'bool'>"; break;
        case TYPE_LIST: type_name = "<class 'list'>"; break;
        case TYPE_DICT: type_name = "<class 'dict'>"; break;
        case TYPE_TUPLE: type_name = "<class 'tuple'>"; break;
        case TYPE_FUNCTION: type_name = "<class 'function'>"; break;
        case TYPE_CLASS: type_name = "<class 'type'>"; break;
        case TYPE_INSTANCE: type_name = "<class 'instance'>"; break;
        case TYPE_NONE: type_name = "<class 'NoneType'>"; break;
        case TYPE_TYPE: type_name = "<class 'type'>"; break;
        default: type_name = "<class 'object'>"; break;
    }
    
    auto* result = PyObjectFactory::create_str(nullptr, type_name);
    return from_pyobj(result);
}

int64_t pyc_len(pyc_obj_t obj) {
    auto* py_obj = to_pyobj(obj);
    if (!py_obj) return 0;
    
    uint32_t type_mask = py_obj->type_object & 0xFF;
    
    switch (type_mask) {
        case TYPE_STR:
            if (py_obj->str_value) return static_cast<int64_t>(py_obj->str_value->size());
            return 0;
        case TYPE_LIST:
            if (py_obj->list_elements) return static_cast<int64_t>(py_obj->list_elements->size());
            return 0;
        case TYPE_DICT:
            if (py_obj->dict_entries) return static_cast<int64_t>(py_obj->dict_entries->size());
            return 0;
        case TYPE_TUPLE:
            if (py_obj->list_elements) return static_cast<int64_t>(py_obj->list_elements->size());
            return 0;
        default:
            return 0;
    }
}

// ===== Attribute Access =====

pyc_obj_t pyc_getattr(pyc_obj_t obj, const char* name) {
    auto* py_obj = to_pyobj(obj);
    if (!py_obj || !name) return nullptr;
    
    std::string attr_name(name);
    
    // Check instance attributes
    if (py_obj->instance_attrs) {
        auto it = py_obj->instance_attrs->find(attr_name);
        if (it != py_obj->instance_attrs->end()) {
            pyc_ref_inc(it->second);
            return from_pyobj(it->second);
        }
    }
    
    return nullptr;
}

void pyc_setattr(pyc_obj_t obj, const char* name, pyc_obj_t value) {
    auto* py_obj = to_pyobj(obj);
    if (!py_obj || !name) return;
    
    std::string attr_name(name);
    
    if (!py_obj->instance_attrs) {
        py_obj->instance_attrs = new std::unordered_map<std::string, PyObject*>();
    }
    
    (*py_obj->instance_attrs)[attr_name] = to_pyobj(value);
}

// ===== Type Operations =====

int64_t pyc_isinstance(pyc_obj_t obj, pyc_type_kind_t type_kind) {
    auto* py_obj = to_pyobj(obj);
    if (!py_obj) return 0;
    
    uint32_t obj_type = py_obj->type_object & 0xFF;
    return (obj_type == static_cast<uint32_t>(type_kind)) ? 1 : 0;
}

pyc_obj_t pyc_object_init(pyc_obj_t obj) {
    auto* py_obj = to_pyobj(obj);
    if (!py_obj) return obj;
    
    // Call __init__ if it exists on the object
    if (py_obj->func_callable) {
        std::vector<PyObject*> empty_args;
        auto* result = (*py_obj->func_callable)(py_obj, empty_args);
        if (result) {
            pyc_ref_inc(result);
            return from_pyobj(result);
        }
    }
    
    return obj;
}

// ===== Math Operations =====

double pyc_pow(int64_t base, int64_t exp) {
    if (exp < 0) {
        return std::pow(static_cast<double>(base), static_cast<double>(exp));
    }
    
    // Integer power for non-negative exponents
    double result = 1.0;
    double b = static_cast<double>(base);
    int64_t e = exp;
    
    while (e > 0) {
        if (e & 1) result *= b;
        b *= b;
        e >>= 1;
    }
    
    return result;
}

int64_t pyc_int_from_double(double val) {
    return static_cast<int64_t>(val);
}

// ===== I/O =====

void pyc_print(pyc_obj_t obj) {
    auto* py_obj = to_pyobj(obj);
    std::cout << py_object_to_string(py_obj) << std::endl;
}

// ===== Memory Management =====

void pyc_ref_inc(pyc_obj_t obj) {
    if (!obj) return;
    auto* py_obj = to_pyobj(obj);
    if (py_obj && !(py_obj->type_object & 0x0001)) {  // Not a singleton
        py_obj->refcount++;
    }
}

void pyc_ref_dec(pyc_obj_t obj) {
    if (!obj) return;
    auto* py_obj = to_pyobj(obj);
    if (py_obj && !(py_obj->type_object & 0x0001)) {  // Not a singleton
        py_obj->refcount--;
        if (py_obj->refcount <= 0) {
            delete py_obj;
        }
    }
}

// ===== Exception Handling =====

static PyObject* g_current_exception = nullptr;

void pyc_raise_exception(pyc_obj_t exc) {
    if (g_current_exception) {
        pyc_ref_dec(g_current_exception);
    }
    g_current_exception = to_pyobj(exc);
    if (g_current_exception) {
        pyc_ref_inc(g_current_exception);
    }
}

pyc_obj_t pyc_get_exception() {
    if (g_current_exception) {
        pyc_ref_inc(g_current_exception);
        return from_pyobj(g_current_exception);
    }
    return nullptr;
}

void pyc_clear_exception() {
    if (g_current_exception) {
        pyc_ref_dec(g_current_exception);
        g_current_exception = nullptr;
    }
}

// ===== Module Loading =====

static std::unordered_map<std::string, PyObject*> g_loaded_modules;

pyc_obj_t pyc_import_module(const char* module_name) {
    if (!module_name) return from_pyobj(PyObjectFactory::create_dict(nullptr));
    
    // Use the new import system
    auto* module_dict = pyc::runtime::import_module(module_name);
    return from_pyobj(module_dict);
}

} // namespace pyc::runtime
