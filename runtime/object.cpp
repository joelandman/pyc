// runtime/object.cpp - Python object model implementation
// Contains PyObject, PyTypeObject, and PyObjectFactory implementations

#include "runtime/object.h"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace pyc::runtime {

// ===== PyObjectFactory Implementations =====

std::unordered_map<PyTypeKind, PyObject*> PyObjectFactory::singletons_ = {};

void PyObjectFactory::initialize() {
    // Create singleton None
    auto none = new PyObject();
    none->refcount = 1;
    none->type_object = static_cast<uint32_t>(TYPE_NONE) | PY_FLAG_SINGLETON;
    singletons_[TYPE_NONE] = none;

    // Create singleton True
    auto true_obj = new PyObject();
    true_obj->refcount = 1;
    true_obj->type_object = static_cast<uint32_t>(TYPE_BOOL) | PY_FLAG_SINGLETON;
    true_obj->data = 1;
    singletons_[TYPE_BOOL] = true_obj;

    // Create singleton False (reuses TYPE_BOOL singleton with data=0)
    // False is handled by create_bool with value=false, not a singleton
    // But we need a TYPE_TYPE singleton for type objects
    auto type_obj = new PyObject();
    type_obj->refcount = 1;
    type_obj->type_object = static_cast<uint32_t>(TYPE_TYPE) | PY_FLAG_SINGLETON;
    singletons_[TYPE_TYPE] = type_obj;
}

PyObject* PyObjectFactory::create_none(PyObject* /*type_obj*/) {
    return get_singleton(TYPE_NONE);
}

PyObject* PyObjectFactory::create_bool(PyObject* /*type_obj*/, bool value) {
    if (value) return get_singleton(TYPE_BOOL);
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_BOOL);
    obj->data = 0;
    return obj;
}

PyObject* PyObjectFactory::create_int(PyObject* /*type_obj*/, int64_t value) {
    if (value >= -1 && value <= 1) {
        // Small integers are cached as singletons
        auto it = singletons_.find(TYPE_INT);
        if (it != singletons_.end()) {
            it->second->refcount++;
            return it->second;
        }
    }
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_INT);
    obj->data = static_cast<uint64_t>(value);
    return obj;
}

PyObject* PyObjectFactory::create_float(PyObject* /*type_obj*/, double value) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_FLOAT);
    obj->data = *reinterpret_cast<uint64_t*>(&value);
    return obj;
}

PyObject* PyObjectFactory::create_str(PyObject* /*type_obj*/, const std::string& value) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_STR) | PY_FLAG_STRING;
    obj->str_value = new std::string(value);
    return obj;
}

PyObject* PyObjectFactory::create_list(PyObject* /*type_obj*/) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_LIST) | PY_FLAG_LIST;
    obj->list_elements = new std::vector<PyObject*>();
    return obj;
}

PyObject* PyObjectFactory::create_dict(PyObject* /*type_obj*/) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_DICT) | PY_FLAG_DICT;
    obj->dict_entries = new std::unordered_map<std::string, PyObject*>();
    return obj;
}

PyObject* PyObjectFactory::create_tuple(PyObject* /*type_obj*/) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_TUPLE) | PY_FLAG_TUPLE;
    obj->list_elements = new std::vector<PyObject*>();
    return obj;
}

PyObject* PyObjectFactory::create_function(PyObject* /*type_obj*/, std::string /*name*/,
                                            std::function<PyObject*(PyObject*, std::vector<PyObject*>)>* func) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_FUNCTION) | PY_FLAG_FUNCTION;
    obj->func_callable = func;
    return obj;
}

PyObject* PyObjectFactory::create_instance(PyObject* /*type_obj*/, std::shared_ptr<PyTypeObject> class_type) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_INSTANCE) | PY_FLAG_INSTANCE;
    obj->instance_attrs = new std::unordered_map<std::string, PyObject*>();
    return obj;
}

PyObject* PyObjectFactory::create_class(PyObject* /*type_obj*/, std::string name,
                                         PyTypeObject* methods) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_CLASS) | PY_FLAG_CLASS;
    return obj;
}

PyObject* PyObjectFactory::get_singleton(PyTypeKind kind) {
    auto it = singletons_.find(kind);
    if (it != singletons_.end()) {
        it->second->refcount++;
        return it->second;
    }
    return nullptr;
}

void PyObjectFactory::finalize() {
    for (auto& [kind, obj] : singletons_) {
        delete obj;
    }
    singletons_.clear();
}

// ===== PyTypeObject Methods =====

void PyTypeObject::set_methods(const std::unordered_map<std::string, PyObject::VTable>& methods) {
    this->methods_map.clear();
}

bool PyTypeObject::has_method(const std::string& name) const {
    return methods_map.count(name) > 0;
}

PyObject* PyTypeObject::call_method(const std::string& name, PyObject* self, std::vector<PyObject*> args) {
    auto it = methods_map.find(name);
    if (it != methods_map.end()) {
        return it->second(self, args);
    }
    return nullptr;
}

} // namespace pyc::runtime
