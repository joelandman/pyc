// runtime/object.cpp - Python object model implementation
// Contains PyObject, PyTypeObject, and PyObjectFactory implementations

#include "runtime/object.h"
#include "runtime/object_registry.h"
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

    // Create singleton TYPE_INT for value 0 (small int caching)
    auto int0_obj = new PyObject();
    int0_obj->refcount = 1;
    int0_obj->type_object = static_cast<uint32_t>(TYPE_INT) | PY_FLAG_SINGLETON;
    int0_obj->data = 0;
    singletons_[TYPE_INT] = int0_obj;

    // Create singleton for value -1 (small int caching)
    auto int_neg1_obj = new PyObject();
    int_neg1_obj->refcount = 1;
    int_neg1_obj->type_object = static_cast<uint32_t>(TYPE_INT) | PY_FLAG_SINGLETON;
    int_neg1_obj->data = static_cast<uint64_t>(-1);
    singletons_[static_cast<PyTypeKind>(TYPE_INT + 1)] = int_neg1_obj;

    // Create singleton for value 1 (small int caching)
    auto int1_obj = new PyObject();
    int1_obj->refcount = 1;
    int1_obj->type_object = static_cast<uint32_t>(TYPE_INT) | PY_FLAG_SINGLETON;
    int1_obj->data = 1;
    singletons_[static_cast<PyTypeKind>(TYPE_INT + 2)] = int1_obj;

    // Create singleton TYPE_TYPE for type objects
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
        PyTypeKind singleton_kind;
        if (value == -1) singleton_kind = static_cast<PyTypeKind>(TYPE_INT + 1);
        else if (value == 0) singleton_kind = TYPE_INT;
        else singleton_kind = static_cast<PyTypeKind>(TYPE_INT + 2);
        
        auto it = singletons_.find(singleton_kind);
        if (it != singletons_.end()) {
            it->second->refcount++;
            return it->second;
        }
    }
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_INT);
    obj->data = static_cast<uint64_t>(value);
    register_object(obj);
    return obj;
}

PyObject* PyObjectFactory::create_float(PyObject* /*type_obj*/, double value) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_FLOAT);
    obj->data = *reinterpret_cast<uint64_t*>(&value);
    register_object(obj);
    return obj;
}

PyObject* PyObjectFactory::create_str(PyObject* /*type_obj*/, const std::string& value) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_STR) | PY_FLAG_STRING;
    obj->str_value = new std::string(value);
    register_object(obj);
    return obj;
}

PyObject* PyObjectFactory::create_list(PyObject* /*type_obj*/) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_LIST) | PY_FLAG_LIST;
    obj->list_elements = new std::vector<PyObject*>();
    register_object(obj);
    return obj;
}

PyObject* PyObjectFactory::create_dict(PyObject* /*type_obj*/) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_DICT) | PY_FLAG_DICT;
    obj->dict_entries = new std::unordered_map<std::string, PyObject*>();
    register_object(obj);
    return obj;
}

PyObject* PyObjectFactory::create_tuple(PyObject* /*type_obj*/) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_TUPLE) | PY_FLAG_TUPLE;
    obj->list_elements = new std::vector<PyObject*>();
    register_object(obj);
    return obj;
}

PyObject* PyObjectFactory::create_function(PyObject* /*type_obj*/, std::string /*name*/,
                                             std::function<PyObject*(PyObject*, std::vector<PyObject*>)>* func) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_FUNCTION) | PY_FLAG_FUNCTION;
    obj->func_callable = func;
    register_object(obj);
    return obj;
}

PyObject* PyObjectFactory::create_instance(PyObject* /*type_obj*/, std::shared_ptr<PyTypeObject> class_type) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_INSTANCE) | PY_FLAG_INSTANCE;
    obj->instance_attrs = new std::unordered_map<std::string, PyObject*>();
    register_object(obj);
    return obj;
}

PyObject* PyObjectFactory::create_class(PyObject* /*type_obj*/, std::string name,
                                          PyTypeObject* methods) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_CLASS) | PY_FLAG_CLASS;
    register_object(obj);
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

static PyObjectRegistry& get_registry() {
    static PyObjectRegistry reg;
    return reg;
}

void PyObjectFactory::finalize() {
    // Clean up all non-singleton registered objects
    auto& reg = get_registry();
    for (auto* obj : reg.objects()) {
        if (obj && !(obj->type_object & PY_FLAG_SINGLETON)) {
            delete obj;
        }
    }
    reg.cleanup();
    
    // Then clean up singletons
    for (auto& [kind, obj] : singletons_) {
        delete obj;
    }
    singletons_.clear();
}

void PyObjectFactory::register_object(PyObject* obj) {
    get_registry().register_object(obj);
}

void PyObjectFactory::unregister_object(PyObject* obj) {
    get_registry().unregister_object(obj);
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
