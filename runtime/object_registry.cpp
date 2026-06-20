// runtime/object_registry.cpp - Registry for tracking PyObject allocations

#include "runtime/object_registry.h"
#include <algorithm>
#include <iostream>

namespace pyc::runtime {

void PyObjectRegistry::register_object(PyObject* obj) {
    if (!obj) return;
    
    // Don't register singletons
    if (obj->type_object & PY_FLAG_SINGLETON) return;
    
    // Don't register if already registered
    if (object_set_.count(obj) > 0) return;
    
    objects_.push_back(obj);
    object_set_.insert(obj);
    total_allocated_++;
    
    if (objects_.size() > peak_count_) {
        peak_count_ = objects_.size();
    }
}

void PyObjectRegistry::unregister_object(PyObject* obj) {
    if (!obj) return;
    
    auto it = object_set_.find(obj);
    if (it == object_set_.end()) return;
    
    object_set_.erase(it);
    
    // Remove from vector (O(n) but acceptable for registry)
    auto vec_it = std::find(objects_.begin(), objects_.end(), obj);
    if (vec_it != objects_.end()) {
        objects_.erase(vec_it);
    }
    
    total_freed_++;
}

void PyObjectRegistry::cleanup() {
    // Delete all non-singleton objects
    for (auto* obj : objects_) {
        if (obj && !(obj->type_object & PY_FLAG_SINGLETON)) {
            delete obj;
        }
    }
    
    objects_.clear();
    object_set_.clear();
}

void PyObjectRegistry::reset_stats() {
    peak_count_ = 0;
    total_allocated_ = 0;
    total_freed_ = 0;
}

bool PyObjectRegistry::is_registered(PyObject* obj) const {
    return object_set_.count(obj) > 0;
}

} // namespace pyc::runtime
