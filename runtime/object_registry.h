// runtime/object_registry.h - Registry for tracking PyObject allocations

#pragma once
#include "runtime/object.h"
#include <vector>
#include <unordered_set>
#include <cstddef>

namespace pyc::runtime {

class PyObjectRegistry {
public:
    PyObjectRegistry() : peak_count_(0), total_allocated_(0), total_freed_(0) {}
    
    // Register a newly allocated object
    void register_object(PyObject* obj);
    
    // Unregister a freed object
    void unregister_object(PyObject* obj);
    
    // Get all registered objects
    const std::vector<PyObject*>& objects() const { return objects_; }
    
    // Get count of live objects
    size_t live_count() const { return objects_.size(); }
    
    // Get peak object count
    size_t peak_count() const { return peak_count_; }
    
    // Get total allocations
    size_t total_allocated() const { return total_allocated_; }
    
    // Get total freed
    size_t total_freed() const { return total_freed_; }
    
    // Cleanup: delete all non-singleton objects
    void cleanup();
    
    // Reset stats
    void reset_stats();
    
    // Check if object is registered
    bool is_registered(PyObject* obj) const;
    
private:
    std::vector<PyObject*> objects_;
    std::unordered_set<PyObject*> object_set_;
    size_t peak_count_;
    size_t total_allocated_;
    size_t total_freed_;
};

} // namespace pyc::runtime
