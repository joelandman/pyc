// runtime/builtins.cpp - Python builtin functions implementation
// Implements the Python built-in namespace (print, len, range, etc.)

#include "runtime/object.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
#include <string>
#include <iomanip>

namespace pyc::runtime {

// ===== HELPER FUNCTIONS =====

static PyObject* to_int(PyObject* obj) {
    if (!obj) return PyObjectFactory::create_int(nullptr, 0);
    
    auto* new_obj = PyObjectFactory::create_int(nullptr, 0);
    switch (obj->type_object & 0xFF) {  // Mask out flags
        case TYPE_INT:
            new_obj->data = obj->data;
            break;
        case TYPE_FLOAT:
            new_obj->data = static_cast<uint64_t>(*reinterpret_cast<double*>(obj->data));
            break;
        case TYPE_STR:
            if (obj->str_value) {
                try { new_obj->data = static_cast<uint64_t>(std::stoll(*obj->str_value)); }
                catch (...) { return nullptr; }
            }
            break;
        case TYPE_BOOL:
            new_obj->data = obj->data;
            break;
        default:
            new_obj->data = 0;
    }
    return new_obj;
}

static PyObject* to_float(PyObject* obj) {
    if (!obj) return PyObjectFactory::create_float(nullptr, 0.0);
    
    double val = 0.0;
    switch (obj->type_object & 0xFF) {  // Mask out flags
        case TYPE_INT:
            val = static_cast<double>(obj->data);
            break;
        case TYPE_FLOAT:
            val = *reinterpret_cast<double*>(obj->data);
            break;
        case TYPE_STR:
            if (obj->str_value) {
                try { val = std::stod(*obj->str_value); }
                catch (...) { val = 0.0; }
            }
            break;
        case TYPE_BOOL:
            val = static_cast<double>(obj->data);
            break;
        default:
            val = 0.0;
    }
    return PyObjectFactory::create_float(nullptr, val);
}

static std::string py_object_to_string(PyObject* obj) {
    if (!obj) return "None";
    
    switch (obj->type_object & 0xFF) {  // Mask out flags
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
        default:
            return "<object>";
    }
}

// ===== BUILTIN FUNCTIONS IMPLEMENTATION =====

PyObject* BuiltinFunctions::builtin_print(PyObject* /*self*/, std::vector<PyObject*> args) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << py_object_to_string(args[i]);
    }
    std::cout << std::endl;
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_len(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* obj = args[0];
    
    uint64_t length = 0;
    switch (obj->type_object & 0xFF) {
        case TYPE_STR:
            if (obj->str_value) length = obj->str_value->size();
            break;
        case TYPE_LIST:
            if (obj->list_elements) length = obj->list_elements->size();
            break;
        case TYPE_DICT:
            if (obj->dict_entries) length = obj->dict_entries->size();
            break;
        case TYPE_TUPLE:
            if (obj->list_elements) length = obj->list_elements->size();
            break;
        default:
            length = static_cast<uint64_t>(obj->data);
            break;
    }
    
    return PyObjectFactory::create_int(nullptr, static_cast<int64_t>(length));
}

PyObject* BuiltinFunctions::builtin_range(PyObject* /*self*/, std::vector<PyObject*> args) {
    int64_t start = 0, stop = 0, step = 1;
    
    if (args.size() >= 1) {
        stop = static_cast<int64_t>(args[0]->data);
    }
    if (args.size() >= 2) {
        start = static_cast<int64_t>(args[0]->data);
        stop = static_cast<int64_t>(args[1]->data);
    }
    if (args.size() >= 3) {
        step = static_cast<int64_t>(args[2]->data);
    }
    
    // Create a list to represent the range
    auto* result = PyObjectFactory::create_list(nullptr);
    if (result->list_elements) {
        if (step > 0) {
            for (int64_t i = start; i < stop; i += step) {
                result->list_elements->push_back(PyObjectFactory::create_int(nullptr, i));
            }
        } else {
            for (int64_t i = start; i > stop; i += step) {
                result->list_elements->push_back(PyObjectFactory::create_int(nullptr, i));
            }
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_type(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) {
        return PyObjectFactory::get_singleton(TYPE_NONE);
    }
    
    auto* obj = args[0];
    if (!obj) return PyObjectFactory::get_singleton(TYPE_NONE);
    
    // Return a string with the type name
    std::string type_name;
    switch (obj->type_object & 0xFF) {
        case TYPE_INT: type_name = "<class 'int'>"; break;
        case TYPE_FLOAT: type_name = "<class 'float'>"; break;
        case TYPE_STR: type_name = "<class 'str'>"; break;
        case TYPE_BOOL: type_name = "<class 'bool'>"; break;
        case TYPE_LIST: type_name = "<class 'list'>"; break;
        case TYPE_DICT: type_name = "<class 'dict'>"; break;
        case TYPE_FUNCTION: type_name = "<class 'function'>"; break;
        case TYPE_NONE: type_name = "<class 'NoneType'>"; break;
        default: type_name = "<class 'object'>"; break;
    }
    return PyObjectFactory::create_str(nullptr, type_name);
}

PyObject* BuiltinFunctions::builtin_int(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return PyObjectFactory::create_int(nullptr, 0);
    return to_int(args[0]);
}

PyObject* BuiltinFunctions::builtin_float(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return PyObjectFactory::create_float(nullptr, 0.0);
    return to_float(args[0]);
}

PyObject* BuiltinFunctions::builtin_str(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return PyObjectFactory::create_str(nullptr, "");
    auto* obj = args[0];
    auto str = py_object_to_string(obj);
    return PyObjectFactory::create_str(nullptr, str);
}

PyObject* BuiltinFunctions::builtin_list(PyObject* /*self*/, std::vector<PyObject*> args) {
    auto* result = PyObjectFactory::create_list(nullptr);
    
    if (!args.empty()) {
        auto* it_obj = args[0];
        // If it's a list, copy elements
        if (it_obj->list_elements) {
            for (auto* elem : *it_obj->list_elements) {
                result->list_elements->push_back(elem);
            }
        }
    }
    
    return result;
}

PyObject* BuiltinFunctions::builtin_dict(PyObject* /*self*/, std::vector<PyObject*> args) {
    return PyObjectFactory::create_dict(nullptr);
}

PyObject* BuiltinFunctions::builtin_tuple(PyObject* /*self*/, std::vector<PyObject*> args) {
    auto* result = PyObjectFactory::create_tuple(nullptr);
    
    if (!args.empty()) {
        auto* it_obj = args[0];
        if (it_obj->list_elements) {
            for (auto* elem : *it_obj->list_elements) {
                result->list_elements->push_back(elem);
            }
        }
    }
    
    return result;
}

PyObject* BuiltinFunctions::builtin_max(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    
    if (args.size() == 1) {
        // Single iterable argument
        auto* iterable = args[0];
        if (!iterable->list_elements || iterable->list_elements->empty()) return nullptr;
        
        auto* max_obj = (*iterable->list_elements)[0];
        for (size_t i = 1; i < iterable->list_elements->size(); ++i) {
            if ((*iterable->list_elements)[i]->data > max_obj->data) {
                max_obj = (*iterable->list_elements)[i];
            }
        }
        return max_obj;
    }
    
    auto* max_obj = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i]->data > max_obj->data) {
            max_obj = args[i];
        }
    }
    return max_obj;
}

PyObject* BuiltinFunctions::builtin_min(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    
    if (args.size() == 1) {
        auto* iterable = args[0];
        if (!iterable->list_elements || iterable->list_elements->empty()) return nullptr;
        
        auto* min_obj = (*iterable->list_elements)[0];
        for (size_t i = 1; i < iterable->list_elements->size(); ++i) {
            if ((*iterable->list_elements)[i]->data < min_obj->data) {
                min_obj = (*iterable->list_elements)[i];
            }
        }
        return min_obj;
    }
    
    auto* min_obj = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i]->data < min_obj->data) {
            min_obj = args[i];
        }
    }
    return min_obj;
}

PyObject* BuiltinFunctions::builtin_sum(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return PyObjectFactory::create_int(nullptr, 0);
    
    int64_t total = 0;
    for (auto* obj : args) {
        total += static_cast<int64_t>(obj->data);
    }
    return PyObjectFactory::create_int(nullptr, total);
}

PyObject* BuiltinFunctions::builtin_abs(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* obj = args[0];
    auto val = static_cast<int64_t>(obj->data);
    if (val < 0) val = -val;
    return PyObjectFactory::create_int(nullptr, val);
}

PyObject* BuiltinFunctions::builtin_isinstance(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) {
        return PyObjectFactory::create_bool(nullptr, false);
    }
    
    auto* obj = args[0];
    auto* type_obj = args[1];
    
    return PyObjectFactory::create_bool(nullptr, 
        (obj->type_object & 0xFF) == (type_obj->type_object & 0xFF));
}

PyObject* BuiltinFunctions::builtin_pow(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* base = args[0];
    auto* exp = args[1];
    
    double base_val = static_cast<double>(base->data);
    double exp_val = static_cast<double>(exp->data);
    
    // Check if both are integers
    bool base_is_int = (base->type_object & 0xFF) == TYPE_INT;
    bool exp_is_int = (exp->type_object & 0xFF) == TYPE_INT;
    
    if (base_is_int && exp_is_int) {
        // Integer power
        int64_t result = 1;
        int64_t base_int = static_cast<int64_t>(base->data);
        int64_t exp_int = static_cast<int64_t>(exp->data);
        if (exp_int < 0) return PyObjectFactory::create_float(nullptr, std::pow(base_val, exp_val));
        for (int64_t i = 0; i < exp_int; ++i) {
            result *= base_int;
        }
        return PyObjectFactory::create_int(nullptr, result);
    }
    
    return PyObjectFactory::create_float(nullptr, std::pow(base_val, exp_val));
}

PyObject* BuiltinFunctions::builtin_divmod(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* a = args[0];
    auto* b = args[1];
    
    auto* div_result = PyObjectFactory::create_int(nullptr, static_cast<int64_t>(a->data / b->data));
    auto* mod_result = PyObjectFactory::create_int(nullptr, static_cast<int64_t>(a->data % b->data));
    
    auto* tuple = PyObjectFactory::create_tuple(nullptr);
    if (tuple->list_elements) {
        tuple->list_elements->push_back(div_result);
        tuple->list_elements->push_back(mod_result);
    }
    return tuple;
}

PyObject* BuiltinFunctions::builtin_reversed(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* iterable = args[0];
    auto* result = PyObjectFactory::create_list(nullptr);
    
    if (iterable->list_elements) {
        for (auto it = iterable->list_elements->rbegin(); it != iterable->list_elements->rend(); ++it) {
            result->list_elements->push_back(*it);
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_enumerate(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* iterable = args[0];
    auto* result = PyObjectFactory::create_list(nullptr);
    
    if (iterable->list_elements) {
        for (size_t i = 0; i < iterable->list_elements->size(); ++i) {
            auto* pair = PyObjectFactory::create_tuple(nullptr);
            if (pair->list_elements) {
                pair->list_elements->push_back(PyObjectFactory::create_int(nullptr, static_cast<int64_t>(i)));
                pair->list_elements->push_back((*iterable->list_elements)[i]);
            }
            result->list_elements->push_back(pair);
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_zip(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    
    // Find minimum length
    size_t min_len = SIZE_MAX;
    for (auto* arg : args) {
        if (arg->list_elements && arg->list_elements->size() < min_len) {
            min_len = arg->list_elements->size();
        }
    }
    
    if (min_len == SIZE_MAX) return PyObjectFactory::create_list(nullptr);
    
    auto* result = PyObjectFactory::create_list(nullptr);
    for (size_t i = 0; i < min_len; ++i) {
        auto* pair = PyObjectFactory::create_tuple(nullptr);
        if (pair->list_elements) {
            for (auto* arg : args) {
                if (arg->list_elements && i < arg->list_elements->size()) {
                    pair->list_elements->push_back((*arg->list_elements)[i]);
                }
            }
        }
        result->list_elements->push_back(pair);
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_map(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    
    auto* func = args[0];
    auto* iterable = args[1];
    auto* result = PyObjectFactory::create_list(nullptr);
    
    if (iterable->list_elements) {
        for (auto* elem : *iterable->list_elements) {
            std::vector<PyObject*> call_args = {elem};
            // Note: func_callable is stored as a pointer, call through it
            if (func->func_callable) {
                auto* ret = (*func->func_callable)(nullptr, call_args);
                result->list_elements->push_back(ret);
            }
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_filter(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    
    auto* func = args[0];
    auto* iterable = args[1];
    auto* result = PyObjectFactory::create_list(nullptr);
    
    if (iterable->list_elements) {
        for (auto* elem : *iterable->list_elements) {
            std::vector<PyObject*> call_args = {elem};
            if (func->func_callable) {
                auto* ret = (*func->func_callable)(nullptr, call_args);
                if (ret && ret->data) {
                    result->list_elements->push_back(elem);
                }
            }
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_sorted(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return PyObjectFactory::create_list(nullptr);
    auto* iterable = args[0];
    auto* result = PyObjectFactory::create_list(nullptr);
    
    if (iterable->list_elements) {
        result->list_elements = new std::vector<PyObject*>(*iterable->list_elements);
        std::sort(result->list_elements->begin(), result->list_elements->end(),
            [](PyObject* a, PyObject* b) { return a->data < b->data; });
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_round(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* obj = args[0];
    double val = static_cast<double>(obj->data);
    val = std::round(val);
    return PyObjectFactory::create_int(nullptr, static_cast<int64_t>(val));
}

PyObject* BuiltinFunctions::builtin_contains(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) {
        return PyObjectFactory::create_bool(nullptr, false);
    }
    auto* obj = args[0];
    auto* item = args[1];
    
    // Check in list
    if (obj->list_elements) {
        for (auto* elem : *obj->list_elements) {
            if (elem->data == item->data) {
                return PyObjectFactory::create_bool(nullptr, true);
            }
        }
    }
    
    // Check in dict keys
    if (obj->dict_entries) {
        for (auto& [k, v] : *obj->dict_entries) {
            if (k == py_object_to_string(item)) {
                return PyObjectFactory::create_bool(nullptr, true);
            }
        }
    }
    
    return PyObjectFactory::create_bool(nullptr, false);
}

PyObject* BuiltinFunctions::builtin_repr(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    return builtin_str(nullptr, args);
}

PyObject* BuiltinFunctions::builtin_callable(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return PyObjectFactory::create_bool(nullptr, false);
    auto* obj = args[0];
    return PyObjectFactory::create_bool(nullptr, 
        (obj->type_object & 0xFF) == TYPE_FUNCTION);
}

PyObject* BuiltinFunctions::builtin_dir(PyObject* /*self*/, std::vector<PyObject*> args) {
    auto* result = PyObjectFactory::create_list(nullptr);
    return result;
}

PyObject* BuiltinFunctions::builtin_hasattr(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* obj = args[0];
    auto* name_obj = args[1];
    std::string name;
    if (name_obj->str_value) name = *name_obj->str_value;
    
    if (obj->instance_attrs && obj->instance_attrs->count(name)) {
        return PyObjectFactory::create_bool(nullptr, true);
    }
    return PyObjectFactory::create_bool(nullptr, false);
}

PyObject* BuiltinFunctions::builtin_setattr(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 3) return PyObjectFactory::get_singleton(TYPE_NONE);
    auto* obj = args[0];
    auto* name_obj = args[1];
    auto* value = args[2];
    
    std::string name;
    if (name_obj->str_value) name = *name_obj->str_value;
    
    if (!obj->instance_attrs) {
        obj->instance_attrs = new std::unordered_map<std::string, PyObject*>();
    }
    (*obj->instance_attrs)[name] = value;
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_delattr(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return PyObjectFactory::get_singleton(TYPE_NONE);
    auto* obj = args[0];
    auto* name_obj = args[1];
    
    std::string name;
    if (name_obj->str_value) name = *name_obj->str_value;
    
    if (obj->instance_attrs) {
        obj->instance_attrs->erase(name);
    }
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_globals(PyObject* /*self*/, std::vector<PyObject*> args) {
    return PyObjectFactory::create_dict(nullptr);
}

PyObject* BuiltinFunctions::builtin_locals(PyObject* /*self*/, std::vector<PyObject*> args) {
    return PyObjectFactory::create_dict(nullptr);
}

PyObject* BuiltinFunctions::builtin_exec(PyObject* /*self*/, std::vector<PyObject*> args) {
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_eval(PyObject* /*self*/, std::vector<PyObject*> args) {
    return PyObjectFactory::create_int(nullptr, 0);
}

PyObject* BuiltinFunctions::builtin_import(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    return PyObjectFactory::create_dict(nullptr);
}

void BuiltinFunctions::register_builtins(std::unordered_map<std::string, PyObject*>& builtins) {
    auto add_builtin = [&](const std::string& name, 
                           std::function<PyObject*(PyObject*, std::vector<PyObject*>)>* func) {
        auto* obj = PyObjectFactory::create_function(nullptr, name, func);
        builtins[name] = obj;
    };
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> print_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_print(self, args);
        };
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> len_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_len(self, args);
        };
    
    add_builtin("print", &print_func);
    add_builtin("len", &len_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> range_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_range(self, args);
        };
    add_builtin("range", &range_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> type_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_type(self, args);
        };
    add_builtin("type", &type_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> int_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_int(self, args);
        };
    add_builtin("int", &int_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> float_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_float(self, args);
        };
    add_builtin("float", &float_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> str_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_str(self, args);
        };
    add_builtin("str", &str_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> list_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_list(self, args);
        };
    add_builtin("list", &list_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> dict_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_dict(self, args);
        };
    add_builtin("dict", &dict_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> tuple_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_tuple(self, args);
        };
    add_builtin("tuple", &tuple_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> max_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_max(self, args);
        };
    add_builtin("max", &max_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> min_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_min(self, args);
        };
    add_builtin("min", &min_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> abs_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_abs(self, args);
        };
    add_builtin("abs", &abs_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> bool_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            if (args.empty()) return PyObjectFactory::create_bool(nullptr, false);
            return PyObjectFactory::create_bool(nullptr, !args[0]->data);
        };
    add_builtin("bool", &bool_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> pow_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_pow(self, args);
        };
    add_builtin("pow", &pow_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> sum_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_sum(self, args);
        };
    add_builtin("sum", &sum_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> isinstance_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_isinstance(self, args);
        };
    add_builtin("isinstance", &isinstance_func);
}

} // namespace pyc::runtime
