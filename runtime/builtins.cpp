// runtime/builtins.cpp - Python builtin functions implementation
// Implements the Python built-in namespace (print, len, range, etc.)

#include "runtime/object.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cctype>
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
            new_obj->data = *reinterpret_cast<uint64_t*>(&obj->data);
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
    
    if (!args.empty()) {
        auto* obj = args[0];
        uint32_t type_mask = obj->type_object & 0xFF;
        
        // Return instance attributes for instances
        if (type_mask == TYPE_INSTANCE && obj->instance_attrs) {
            for (auto& [key, val] : *obj->instance_attrs) {
                result->list_elements->push_back(PyObjectFactory::create_str(nullptr, key));
            }
        }
        // Return dict keys for dicts
        else if (type_mask == TYPE_DICT && obj->dict_entries) {
            for (auto& [key, val] : *obj->dict_entries) {
                result->list_elements->push_back(PyObjectFactory::create_str(nullptr, key));
            }
        }
        // Return type methods/attributes for types
        else {
            std::vector<std::string> type_attrs;
            switch (type_mask) {
                case TYPE_INT:
                    type_attrs = {"__abs__", "__add__", "__and__", "__bool__", "__ceil__", 
                                  "__divmod__", "__floor__", "__floordiv__", "__format__", 
                                  "__ge__", "__getattribute__", "__getnewargs__", "__gt__", 
                                  "__hash__", "__index__", "__int__", "__invert__", "__le__", 
                                  "__lshift__", "__lt__", "__mod__", "__mul__", "__neg__", 
                                  "__or__", "__pos__", "__pow__", "__radd__", "__rand__", 
                                  "__rdivmod__", "__rfloordiv__", "__rlshift__", "__rmod__", 
                                  "__rmul__", "__ror__", "__round__", "__rpow__", "__rrshift__", 
                                  "__rshift__", "__rsub__", "__rtruediv__", "__rxor__", 
                                  "__setattr__", "__sizeof__", "__str__", "__sub__", 
                                  "__truediv__", "__trunc__", "__xor__"};
                    break;
                case TYPE_FLOAT:
                    type_attrs = {"__abs__", "__add__", "__and__", "__bool__", "__ceil__", 
                                  "__divmod__", "__floor__", "__floordiv__", "__format__", 
                                  "__ge__", "__getattribute__", "__gt__", "__hash__", 
                                  "__int__", "__le__", "__lt__", "__mod__", "__mul__", 
                                  "__neg__", "__or__", "__pos__", "__pow__", "__radd__", 
                                  "__rand__", "__rdivmod__", "__rfloordiv__", "__rmod__", 
                                  "__rmul__", "__ror__", "__round__", "__rpow__", "__rshift__", 
                                  "__rsub__", "__rtruediv__", "__rxor__", "__sizeof__", 
                                  "__str__", "__sub__", "__truediv__", "__trunc__", "__xor__"};
                    break;
                case TYPE_STR:
                    type_attrs = {"__add__", "__class__", "__contains__", "__eq__", "__format__", 
                                  "__ge__", "__getattribute__", "__getitem__", "__getnewargs__", 
                                  "__gt__", "__hash__", "__iter__", "__le__", "__len__", "__lt__", 
                                  "__mod__", "__mul__", "__ne__", "__new__", "__reduce__", 
                                  "__reduce_ex__", "__repr__", "__rmod__", "__rmul__", "__setattr__", 
                                  "__sizeof__", "__str__", "capitalize", "casefold", "center", 
                                  "count", "encode", "endswith", "expandtabs", "find", "format", 
                                  "format_map", "index", "isalnum", "isalpha", "isascii", 
                                  "isdecimal", "isdigit", "isidentifier", "islower", "isnumeric", 
                                  "isprintable", "isspace", "istitle", "isupper", "join", "ljust", 
                                  "lower", "lstrip", "maketrans", "partition", "replace", "rfind", 
                                  "rindex", "rjust", "rpartition", "rsplit", "rstrip", "split", 
                                  "splitlines", "startswith", "strip", "swapcase", "title", 
                                  "translate", "upper", "zfill"};
                    break;
                case TYPE_LIST:
                    type_attrs = {"__add__", "__class__", "__contains__", "__delitem__", 
                                  "__eq__", "__ge__", "__getattribute__", "__getitem__", "__gt__", 
                                  "__iadd__", "__imul__", "__init__", "__iter__", "__le__", 
                                  "__len__", "__lt__", "__mul__", "__ne__", "__new__", "__reduce__", 
                                  "__reduce_ex__", "__repr__", "__reversed__", "__rmul__", 
                                  "__setitem__", "__sizeof__", "__str__", "append", "clear", 
                                  "copy", "count", "extend", "index", "insert", "pop", "remove", 
                                  "reverse", "sort"};
                    break;
                case TYPE_DICT:
                    type_attrs = {"__class__", "__contains__", "__delitem__", "__eq__", 
                                  "__ge__", "__getattribute__", "__getitem__", "__gt__", 
                                  "__init__", "__iter__", "__le__", "__len__", "__lt__", 
                                  "__ne__", "__new__", "__reduce__", "__reduce_ex__", "__repr__", 
                                  "__setattr__", "__setitem__", "__sizeof__", "__str__", 
                                  "clear", "copy", "fromkeys", "get", "items", "keys", 
                                  "pop", "popitem", "setdefault", "update", "values"};
                    break;
                default:
                    type_attrs = {"__class__", "__delattr__", "__dir__", "__doc__", 
                                  "__eq__", "__format__", "__ge__", "__getattribute__", 
                                  "__gt__", "__hash__", "__init__", "__init_subclass__", 
                                  "__le__", "__lt__", "__ne__", "__new__", "__reduce__", 
                                  "__reduce_ex__", "__repr__", "__setattr__", "__sizeof__", 
                                  "__str__", "__subclasshook__"};
                    break;
            }
            for (auto& attr : type_attrs) {
                result->list_elements->push_back(PyObjectFactory::create_str(nullptr, attr));
            }
        }
    } else {
        // Empty dir returns empty list
    }
    
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
    // Returns empty dict - full implementation requires interpreter integration
    // to access the current interpreter's global_vars_ map
    (void)args;
    return PyObjectFactory::create_dict(nullptr);
}

PyObject* BuiltinFunctions::builtin_locals(PyObject* /*self*/, std::vector<PyObject*> args) {
    // Returns empty dict - full implementation requires interpreter integration
    // to access the current frame's local variables
    (void)args;
    return PyObjectFactory::create_dict(nullptr);
}

PyObject* BuiltinFunctions::builtin_exec(PyObject* /*self*/, std::vector<PyObject*> args) {
    // Execute code string dynamically
    // Requires access to parser and IR builder to compile and execute
    // For now, returns None (stub)
    (void)args;
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_eval(PyObject* /*self*/, std::vector<PyObject*> args) {
    // Evaluate expression string dynamically
    // Requires access to parser and IR builder to compile and execute
    // For now, returns 0 (stub)
    (void)args;
    return PyObjectFactory::create_int(nullptr, 0);
}

PyObject* BuiltinFunctions::builtin_import(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    return PyObjectFactory::create_dict(nullptr);
}

// ===== LIST METHODS =====

PyObject* BuiltinFunctions::builtin_append(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* lst = args[0];
    if (!lst->list_elements) return nullptr;
    lst->list_elements->push_back(args[1]);
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_extend(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* lst = args[0];
    auto* other = args[1];
    if (!lst->list_elements) return nullptr;
    if (other->list_elements) {
        for (auto* elem : *other->list_elements) {
            lst->list_elements->push_back(elem);
        }
    }
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_pop(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* lst = args[0];
    if (!lst->list_elements || lst->list_elements->empty()) return nullptr;
    
    int64_t idx = -1;
    if (args.size() >= 2) {
        idx = static_cast<int64_t>(args[1]->data);
    }
    
    if (idx < 0) idx = lst->list_elements->size() + idx;
    if (idx < 0 || idx >= static_cast<int64_t>(lst->list_elements->size())) return nullptr;
    
    auto* result = (*lst->list_elements)[static_cast<size_t>(idx)];
    lst->list_elements->erase(lst->list_elements->begin() + idx);
    return result;
}

PyObject* BuiltinFunctions::builtin_remove(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return PyObjectFactory::get_singleton(TYPE_NONE);
    auto* lst = args[0];
    auto* value = args[1];
    if (!lst->list_elements) return nullptr;
    
    for (auto it = lst->list_elements->begin(); it != lst->list_elements->end(); ++it) {
        if ((*it)->data == value->data) {
            lst->list_elements->erase(it);
            return PyObjectFactory::get_singleton(TYPE_NONE);
        }
    }
    return nullptr;
}

PyObject* BuiltinFunctions::builtin_clear(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* lst = args[0];
    if (lst->list_elements) lst->list_elements->clear();
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_count(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* lst = args[0];
    auto* value = args[1];
    int64_t cnt = 0;
    if (lst->list_elements) {
        for (auto* elem : *lst->list_elements) {
            if (elem->data == value->data) cnt++;
        }
    }
    return PyObjectFactory::create_int(nullptr, cnt);
}

PyObject* BuiltinFunctions::builtin_index(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* lst = args[0];
    auto* value = args[1];
    if (lst->list_elements) {
        for (size_t i = 0; i < lst->list_elements->size(); ++i) {
            if ((*lst->list_elements)[i]->data == value->data) {
                return PyObjectFactory::create_int(nullptr, static_cast<int64_t>(i));
            }
        }
    }
    return nullptr;
}

// ===== DICT METHODS =====

PyObject* BuiltinFunctions::builtin_get(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* dct = args[0];
    if (!dct->dict_entries) return nullptr;
    
    std::string key;
    if (args.size() >= 2) {
        if (args[1]->str_value) key = *args[1]->str_value;
        else key = py_object_to_string(args[1]);
    }
    
    auto it = dct->dict_entries->find(key);
    if (it != dct->dict_entries->end()) return it->second;
    
    if (args.size() >= 3) return args[2];
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

PyObject* BuiltinFunctions::builtin_keys(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* dct = args[0];
    auto* result = PyObjectFactory::create_list(nullptr);
    if (dct->dict_entries && result->list_elements) {
        for (auto& [k, v] : *dct->dict_entries) {
            result->list_elements->push_back(PyObjectFactory::create_str(nullptr, k));
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_values(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* dct = args[0];
    auto* result = PyObjectFactory::create_list(nullptr);
    if (dct->dict_entries && result->list_elements) {
        for (auto& [k, v] : *dct->dict_entries) {
            result->list_elements->push_back(v);
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_items(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* dct = args[0];
    auto* result = PyObjectFactory::create_list(nullptr);
    if (dct->dict_entries && result->list_elements) {
        for (auto& [k, v] : *dct->dict_entries) {
            auto* pair = PyObjectFactory::create_tuple(nullptr);
            if (pair->list_elements) {
                pair->list_elements->push_back(PyObjectFactory::create_str(nullptr, k));
                pair->list_elements->push_back(v);
            }
            result->list_elements->push_back(pair);
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_pop_dict(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* dct = args[0];
    if (!dct->dict_entries) return nullptr;
    
    std::string key;
    if (args[1]->str_value) key = *args[1]->str_value;
    else key = py_object_to_string(args[1]);
    
    auto it = dct->dict_entries->find(key);
    if (it == dct->dict_entries->end()) {
        if (args.size() >= 3) return args[2];
        return nullptr;
    }
    
    auto* result = it->second;
    dct->dict_entries->erase(it);
    return result;
}

PyObject* BuiltinFunctions::builtin_setdefault(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* dct = args[0];
    if (!dct->dict_entries) return nullptr;
    
    std::string key;
    if (args[1]->str_value) key = *args[1]->str_value;
    else key = py_object_to_string(args[1]);
    
    auto it = dct->dict_entries->find(key);
    if (it != dct->dict_entries->end()) return it->second;
    
    PyObject* default_val = (args.size() >= 3) ? args[2] : nullptr;
    (*dct->dict_entries)[key] = default_val;
    return default_val;
}

PyObject* BuiltinFunctions::builtin_dict_clear(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* dct = args[0];
    if (dct->dict_entries) dct->dict_entries->clear();
    return PyObjectFactory::get_singleton(TYPE_NONE);
}

// ===== STRING METHODS =====

PyObject* BuiltinFunctions::builtin_join(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* sep_obj = args[0];
    auto* iterable = args[1];
    
    std::string sep;
    if (sep_obj->str_value) sep = *sep_obj->str_value;
    
    std::string result;
    if (iterable->list_elements) {
        bool first = true;
        for (auto* elem : *iterable->list_elements) {
            if (!first) result += sep;
            result += py_object_to_string(elem);
            first = false;
        }
    }
    return PyObjectFactory::create_str(nullptr, result);
}

PyObject* BuiltinFunctions::builtin_split(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* str_obj = args[0];
    if (!str_obj->str_value) return nullptr;
    
    std::string sep;
    if (args.size() >= 2 && args[1]->str_value) sep = *args[1]->str_value;
    
    std::vector<std::string> parts;
    if (sep.empty()) {
        parts.push_back(*str_obj->str_value);
    } else {
        size_t start = 0;
        size_t end = str_obj->str_value->find(sep);
        while (end != std::string::npos) {
            parts.push_back(str_obj->str_value->substr(start, end - start));
            start = end + sep.size();
            end = str_obj->str_value->find(sep, start);
        }
        parts.push_back(str_obj->str_value->substr(start));
    }
    
    auto* result = PyObjectFactory::create_list(nullptr);
    if (result->list_elements) {
        for (auto& p : parts) {
            result->list_elements->push_back(PyObjectFactory::create_str(nullptr, p));
        }
    }
    return result;
}

PyObject* BuiltinFunctions::builtin_strip(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* str_obj = args[0];
    if (!str_obj->str_value) return nullptr;
    
    std::string s = *str_obj->str_value;
    size_t start = 0;
    size_t end = s.size();
    
    while (start < end && std::isspace(s[start])) start++;
    while (end > start && std::isspace(s[end - 1])) end--;
    
    return PyObjectFactory::create_str(nullptr, s.substr(start, end - start));
}

PyObject* BuiltinFunctions::builtin_replace(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 3) return nullptr;
    auto* str_obj = args[0];
    if (!str_obj->str_value) return nullptr;
    
    std::string old_str, new_str;
    if (args[1]->str_value) old_str = *args[1]->str_value;
    if (args[2]->str_value) new_str = *args[2]->str_value;
    
    std::string s = *str_obj->str_value;
    size_t pos = 0;
    while ((pos = s.find(old_str, pos)) != std::string::npos) {
        s.replace(pos, old_str.size(), new_str);
        pos += new_str.size();
    }
    return PyObjectFactory::create_str(nullptr, s);
}

PyObject* BuiltinFunctions::builtin_upper(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* str_obj = args[0];
    if (!str_obj->str_value) return nullptr;
    
    std::string s = *str_obj->str_value;
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return PyObjectFactory::create_str(nullptr, s);
}

PyObject* BuiltinFunctions::builtin_lower(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return nullptr;
    auto* str_obj = args[0];
    if (!str_obj->str_value) return nullptr;
    
    std::string s = *str_obj->str_value;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return PyObjectFactory::create_str(nullptr, s);
}

PyObject* BuiltinFunctions::builtin_find(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.size() < 2) return nullptr;
    auto* str_obj = args[0];
    if (!str_obj->str_value) return nullptr;
    
    std::string sub;
    if (args[1]->str_value) sub = *args[1]->str_value;
    
    size_t pos = str_obj->str_value->find(sub);
    if (pos == std::string::npos) return PyObjectFactory::create_int(nullptr, -1);
    return PyObjectFactory::create_int(nullptr, static_cast<int64_t>(pos));
}

PyObject* BuiltinFunctions::builtin_format(PyObject* /*self*/, std::vector<PyObject*> args) {
    if (args.empty()) return PyObjectFactory::create_str(nullptr, "");
    
    auto* obj = args[0];
    std::string format_str;
    
    // Get the format string from first arg if it's a string
    if (obj->type_object & 0xFF == TYPE_STR && obj->str_value) {
        format_str = *obj->str_value;
    } else {
        format_str = py_object_to_string(obj);
    }
    
    // Handle remaining args for format specifiers
    std::string result = format_str;
    
    // Replace {0}, {1}, etc. with arg values
    for (size_t i = 1; i < args.size(); ++i) {
        std::string placeholder = "{" + std::to_string(i - 1) + "}";
        std::string replacement = py_object_to_string(args[i]);
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.size(), replacement);
            pos += replacement.size();
        }
    }
    
    // Handle format specifiers like {:.2f}, {:d}, {:s}
    size_t spec_start = result.find(':');
    if (spec_start != std::string::npos) {
        size_t spec_end = result.find('}', spec_start);
        if (spec_end != std::string::npos) {
            std::string spec = result.substr(spec_start + 1, spec_end - spec_start - 1);
            std::string value_str = result.substr(0, spec_start);
            
            // Remove the placeholder if present
            size_t placeholder_end = value_str.find('}');
            if (placeholder_end != std::string::npos) {
                value_str = value_str.substr(0, placeholder_end);
            }
            
            // Parse format specifier
            double value = 0.0;
            bool is_float = false;
            
            // Extract numeric value
            try {
                size_t idx = 0;
                value = std::stod(value_str, &idx);
                is_float = (value_str[idx - 1] == '.' || value_str.find('.') != std::string::npos);
            } catch (...) {
                value = std::stod(value_str);
                is_float = true;
            }
            
            // Apply format specifier
            if (spec.find('f') != std::string::npos || spec.find('F') != std::string::npos) {
                int precision = 6;
                size_t dot_pos = spec.find('.');
                if (dot_pos != std::string::npos) {
                    std::string prec_str = spec.substr(dot_pos + 1);
                    size_t colon_pos = prec_str.find('f');
                    if (colon_pos != std::string::npos) {
                        prec_str = prec_str.substr(0, colon_pos);
                    }
                    if (!prec_str.empty()) {
                        precision = std::stoi(prec_str);
                    }
                }
                std::stringstream ss;
                ss << std::fixed << std::setprecision(precision) << value;
                result = ss.str();
            } else if (spec.find('d') != std::string::npos || spec.find('i') != std::string::npos) {
                result = std::to_string(static_cast<int64_t>(value));
            } else if (spec.find('s') != std::string::npos) {
                // String format - just use the original string
                result = value_str;
            } else if (spec.find('%') != std::string::npos) {
                std::stringstream ss;
                ss << (value * 100) << "%";
                result = ss.str();
            }
        }
    }
    
    return PyObjectFactory::create_str(nullptr, result);
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
            return PyObjectFactory::create_bool(nullptr, args[0]->data != 0);
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
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> append_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_append(self, args);
        };
    add_builtin("append", &append_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> extend_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_extend(self, args);
        };
    add_builtin("extend", &extend_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> pop_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_pop(self, args);
        };
    add_builtin("pop", &pop_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> remove_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_remove(self, args);
        };
    add_builtin("remove", &remove_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> clear_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_clear(self, args);
        };
    add_builtin("clear", &clear_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> count_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_count(self, args);
        };
    add_builtin("count", &count_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> index_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_index(self, args);
        };
    add_builtin("index", &index_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> get_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_get(self, args);
        };
    add_builtin("get", &get_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> keys_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_keys(self, args);
        };
    add_builtin("keys", &keys_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> values_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_values(self, args);
        };
    add_builtin("values", &values_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> items_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_items(self, args);
        };
    add_builtin("items", &items_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> join_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_join(self, args);
        };
    add_builtin("join", &join_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> split_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_split(self, args);
        };
    add_builtin("split", &split_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> strip_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_strip(self, args);
        };
    add_builtin("strip", &strip_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> replace_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_replace(self, args);
        };
    add_builtin("replace", &replace_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> upper_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_upper(self, args);
        };
    add_builtin("upper", &upper_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> lower_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_lower(self, args);
        };
    add_builtin("lower", &lower_func);
    
    static std::function<PyObject*(PyObject*, std::vector<PyObject*>)> find_func = 
        [](PyObject* self, std::vector<PyObject*> args) -> PyObject* {
            return builtin_find(self, args);
        };
    add_builtin("find", &find_func);
}

} // namespace pyc::runtime
