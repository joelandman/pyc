// runtime/gc.cpp - Mark-and-sweep garbage collector
// Implements proper reference counting and mark-sweep collection

#include "runtime/object.h"
#include <iostream>
#include <unordered_set>
#include <vector>
#include <algorithm>

namespace pyc::runtime {

// ===== GarbageCollector Implementation =====

GarbageCollector::GarbageCollector(bool enabled)
    : enabled_(enabled), collected_(0) {}

GarbageCollector::~GarbageCollector() {
    // Clean up all remaining roots (including singletons if finalize was not called)
    for (auto* obj : roots_) {
        if (obj && !(obj->type_object & PY_FLAG_SINGLETON)) {
            delete obj;
        }
    }
    roots_.clear();
}

void GarbageCollector::collect() {
    if (!enabled_) return;
    
    // Mark phase: mark all objects reachable from roots
    mark_all_roots();
    
    // Sweep phase: delete unmarked non-singleton objects
    swept_count_ = 0;
    std::vector<PyObject*> new_roots;
    new_roots.reserve(roots_.size());
    
    for (auto* obj : roots_) {
        if (!obj) continue;
        
        // Never collect singletons
        if (obj->type_object & PY_FLAG_SINGLETON) {
            new_roots.push_back(obj);
            continue;
        }
        
        if (marked_.count(obj) > 0) {
            // Object is reachable, keep it and unmark
            new_roots.push_back(obj);
        } else {
            // Object is unreachable, collect it
            delete obj;
            swept_count_++;
            collected_++;
        }
    }
    
    // Replace roots with only live objects
    roots_ = std::move(new_roots);
    marked_.clear();
}

void GarbageCollector::mark_all_roots() {
    // Clear mark set
    marked_.clear();
    
    // Mark all roots
    for (auto* obj : roots_) {
        if (obj) {
            mark_object(obj);
        }
    }
}

void GarbageCollector::mark_object(PyObject* obj) {
    if (!obj || marked_.count(obj) > 0) return;
    
    marked_.insert(obj);
    
    // Recursively mark children (objects linked via next pointer)
    if (obj->next) {
        mark_object(static_cast<PyObject*>(obj->next));
    }
}

void GarbageCollector::add_ref(PyObject* obj) {
    if (!obj) return;
    obj->refcount++;
}

void GarbageCollector::del_ref(PyObject* obj) {
    if (!obj) return;
    obj->refcount--;
    
    // If refcount reaches zero, delete immediately (refcounting takes precedence)
    if (obj->refcount == 0) {
        delete obj;
        swept_count_++;
        collected_++;
    }
}

PyObject* GarbageCollector::new_object(PyTypeKind kind, size_t /*size*/) {
    auto* obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(kind);
    obj->data = 0;
    obj->next = nullptr;
    
    // Do NOT auto-register as root. Roots are explicitly managed.
    return obj;
}

void GarbageCollector::gc_root(PyObject* obj) {
    if (obj) {
        // Don't add duplicates
        if (std::find(roots_.begin(), roots_.end(), obj) == roots_.end()) {
            roots_.push_back(obj);
        }
    }
}

void GarbageCollector::unmark_all() {
    marked_.clear();
}

bool GarbageCollector::is_marked(PyObject* obj) const {
    return marked_.count(obj) > 0;
}

size_t GarbageCollector::object_count() const {
    return roots_.size();
}

size_t GarbageCollector::collected_count() const {
    return collected_;
}

} // namespace pyc::runtime
