#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <type_traits>

// Forward declarations
namespace pyc::runtime {

// ===== OBJECT MODEL (PyObject-like) =====

// Python object type flags
enum PyObjectTypeFlag : uint32_t {
    PY_FLAG_SINGLETON   = 0x0001,  // Singleton (None, True, False)
    PY_FLAG_IMMUTABLE   = 0x0002,  // Immutable object
    PY_FLAG_NUMBER      = 0x0004,  // Numeric type
    PY_FLAG_SEQUENCE    = 0x0008,  // Sequence type
    PY_FLAG_MAPPING     = 0x0010,  // Mapping type
    PY_FLAG_STRING      = 0x0020,  // String type
    PY_FLAG_LIST        = 0x0040,  // List type
    PY_FLAG_DICT        = 0x0080,  // Dict type
    PY_FLAG_TUPLE       = 0x0100,  // Tuple type
    PY_FLAG_FUNCTION    = 0x0200,  // Function type
    PY_FLAG_CLASS       = 0x0400,  // Class type
    PY_FLAG_INSTANCE    = 0x0800,  // Instance type
    PY_FLAG_NONE        = 0x1000,  // None type
    PY_FLAG_BOOL        = 0x2000,  // Bool type
};

// PyObject structure - mirrors CPython's PyObject
struct PyObject {
    uint32_t refcount;
    uint32_t type_object;  // pointer to PyTypeObject
    
    // Data union: for small values, stored directly
    uint64_t data;
    
    // For strings: stores the actual string content
    std::string* str_value;
    
    // For functions: stores the callable
    std::function<PyObject*(PyObject*, std::vector<PyObject*>)>* func_callable;
    
    // For lists: stores elements
    std::vector<PyObject*>* list_elements;
    
    // For dicts: stores key-value pairs
    std::unordered_map<std::string, PyObject*>* dict_entries;
    
    // For instances: stores instance attributes
    std::unordered_map<std::string, PyObject*>* instance_attrs;
    
    // Next pointer: for linked list objects (legacy)
    void* next;
    
    PyObject() 
        : refcount(0), type_object(0), data(0),
          str_value(nullptr), func_callable(nullptr),
          list_elements(nullptr), dict_entries(nullptr),
          instance_attrs(nullptr), next(nullptr) {}
    
    ~PyObject() {
        delete str_value;
        delete list_elements;
        delete dict_entries;
        delete instance_attrs;
    }
    
    // Virtual dispatch table
    struct VTable {
        std::function<void(PyObject*)> destructor;
        std::function<PyObject*(PyObject*, PyObject*)> binary_op;
        std::function<uint64_t(PyObject*)> hash;
        std::function<std::string(PyObject*)> repr;
        std::function<bool(PyObject*, PyObject*)> compare;
        std::function<PyObject*(PyObject*, PyObject*)> get_iter;
        std::function<PyObject*(PyObject*)> next;
        std::function<PyObject*(PyObject*, PyObject*)> get_item;
        std::function<void(PyObject*, PyObject*)> set_item;
    } vtable;
};

// Type objects
enum PyTypeKind : uint32_t {
    TYPE_NONE,
    TYPE_BOOL,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STR,
    TYPE_LIST,
    TYPE_DICT,
    TYPE_TUPLE,
    TYPE_FUNCTION,
    TYPE_CLASS,
    TYPE_INSTANCE,
    TYPE_TYPE,
    TYPE_MODULE,
    TYPE_OBJECT,
};

struct PyTypeObject {
    uint32_t refcount;
    PyTypeKind kind;
    std::string name;
    size_t size;
    PyObject::VTable methods;
    
    // Class hierarchy
    std::unordered_map<std::string, std::shared_ptr<PyTypeObject>> bases;
    std::unordered_map<std::string, std::function<PyObject*(PyObject*, std::vector<PyObject*>)>> methods_map;
    
    void set_methods(const std::unordered_map<std::string, PyObject::VTable>& methods);
    bool has_method(const std::string& name) const;
    PyObject* call_method(const std::string& name, PyObject* self, std::vector<PyObject*> args);
};

// ===== GC SYSTEM =====

class GarbageCollector {
public:
    explicit GarbageCollector(bool enabled = true);
    ~GarbageCollector();
    
    // GC operations
    void collect();
    void add_ref(PyObject* obj);
    void del_ref(PyObject* obj);
    PyObject* new_object(PyTypeKind kind, size_t size);
    void gc_root(PyObject* obj);
    void unmark_all();
    bool is_marked(PyObject* obj) const;
    
    // Stats
    size_t object_count() const;
    size_t collected_count() const;
    
private:
    void mark_all_roots();
    void mark_object(PyObject* obj);
    
    std::vector<PyObject*> roots_;
    std::unordered_set<PyObject*> marked_;
    size_t collected_;
    size_t swept_count_;
    bool enabled_;
};

// ===== BUILTIN FUNCTIONS =====

class BuiltinFunctions {
public:
    static PyObject* builtin_print(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_len(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_range(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_type(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_int(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_float(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_str(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_list(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_dict(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_tuple(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_abs(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_max(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_min(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_sum(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_isinstance(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_hash(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_pow(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_divmod(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_round(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_reversed(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_enumerate(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_zip(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_map(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_filter(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_sorted(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_count(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_index(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_append(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_extend(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_pop(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_remove(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_clear(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_keys(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_values(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_items(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_get(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_setdefault(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_join(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_split(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_strip(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_format(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_contains(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_repr(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_callable(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_isinstance_check(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_issubclass(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_property(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_static_method(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_class_method(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_super(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_object_init(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_dir(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_hasattr(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_setattr(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_delattr(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_globals(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_locals(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_exec(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_eval(PyObject* self, std::vector<PyObject*> args);
    static PyObject* builtin_import(PyObject* self, std::vector<PyObject*> args);
    // ... etc
    
    static void register_builtins(std::unordered_map<std::string, PyObject*>& builtins);
};

// ===== OBJECT FACTORY =====

class PyObjectFactory {
public:
    static PyObject* create_none(PyObject* type_obj = nullptr);
    static PyObject* create_bool(PyObject* type_obj = nullptr, bool value = false);
    static PyObject* create_int(PyObject* type_obj = nullptr, int64_t value = 0);
    static PyObject* create_float(PyObject* type_obj = nullptr, double value = 0.0);
    static PyObject* create_str(PyObject* type_obj = nullptr, const std::string& value = "");
    static PyObject* create_list(PyObject* type_obj = nullptr);
    static PyObject* create_dict(PyObject* type_obj = nullptr);
    static PyObject* create_tuple(PyObject* type_obj = nullptr);
    static PyObject* create_function(PyObject* type_obj = nullptr, std::string name = "",
                                      std::function<PyObject*(PyObject*, std::vector<PyObject*>)>* func = nullptr);
    static PyObject* create_instance(PyObject* type_obj = nullptr, std::shared_ptr<PyTypeObject> class_type = nullptr);
    static PyObject* create_class(PyObject* type_obj = nullptr, std::string name = "",
                                   PyTypeObject* methods = nullptr);
    
    static PyObject* create_singleton(PyTypeKind kind);
    static PyObject* get_singleton(PyTypeKind kind);
    
    static void initialize();
    static void finalize();
    
    static std::unordered_map<PyTypeKind, PyObject*> singletons_;
};

// ===== FRAME AND EXECUTION CONTEXT =====

struct PyFrame {
    std::string function_name;
    PyObject* code_object;
    std::unordered_map<std::string, PyObject*> locals;
    std::unordered_map<std::string, PyObject*> globals;
    PyObject* builtins;
    PyObject* return_value;
    int last_exception_line;
    
    PyFrame(std::string name, PyObject* code)
        : function_name(std::move(name)), code_object(code),
          return_value(nullptr), last_exception_line(-1) {
    }
};

class RuntimeError {
public:
    std::string type;
    std::string message;
    PyObject* exception_obj;
    int line_number;
    bool raised;
    
    RuntimeError(std::string t, std::string m, int line = -1)
        : type(std::move(t)), message(std::move(m)), exception_obj(nullptr),
          line_number(line), raised(false) {}
    
    void raise() { raised = true; }
    bool is_raised() const { return raised; }
};

class PyInterpreter {
public:
    PyInterpreter();
    ~PyInterpreter();
    
    void execute_statement(const std::string& statement);
    void execute_function(const std::string& name, std::vector<PyObject*> args);
    PyObject* eval_expression(const std::string& expression);
    
    // Module management
    void load_module(const std::string& name, const std::string& code);
    PyObject* get_module(const std::string& name);
    
    // Globals
    PyObject* get_global(const std::string& name);
    void set_global(const std::string& name, PyObject* value);
    std::unordered_map<std::string, PyObject*>& globals();
    
    // Error handling
    void push_exception(const RuntimeError& error);
    PyObject* pop_exception();
    PyObject* current_exception() const;
    
    // GC operations
    GarbageCollector& gc() { return gc_; }
    
private:
    std::unordered_map<std::string, PyObject*> globals_;
    std::unordered_map<std::string, PyObject*> builtins_;
    std::unordered_map<std::string, PyObject*> modules_;
    GarbageCollector gc_;
    std::vector<PyObject*> frame_stack_;
    PyFrame* current_frame_;
};

// ===== COUNTER FOR OBJECT IDs =====

class PyObjectIdCounter {
public:
    static uint64_t next_id() {
        static uint64_t id = 0;
        return ++id;
    }
};

} // namespace pyc::runtime
