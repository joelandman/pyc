// runtime/libpyc_runtime.h - Runtime functions for LLVM codegen
// These functions are called from generated LLVM IR code

#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Opaque PyObject type (matches LLVM i8*)
typedef void* pyc_obj_t;

// Type kind constants (must match PyTypeKind enum in object.h)
enum pyc_type_kind_t {
    PYC_TYPE_NONE = 0,
    PYC_TYPE_BOOL,
    PYC_TYPE_INT,
    PYC_TYPE_FLOAT,
    PYC_TYPE_STR,
    PYC_TYPE_LIST,
    PYC_TYPE_DICT,
    PYC_TYPE_TUPLE,
    PYC_TYPE_FUNCTION,
    PYC_TYPE_CLASS,
    PYC_TYPE_INSTANCE,
    PYC_TYPE_TYPE,
    PYC_TYPE_MODULE,
    PYC_TYPE_OBJECT,
};

// ===== Runtime Function Declarations =====

namespace pyc::runtime {

// Object creation
pyc_obj_t pyc_codegen_new_object(pyc_type_kind_t type_kind);
pyc_obj_t pyc_new_list();
 pyc_obj_t pyc_new_dict();
 pyc_obj_t pyc_new_tuple();
 pyc_obj_t pyc_new_type(pyc_type_kind_t type_kind);

// List operations
pyc_obj_t pyc_list_get(pyc_obj_t list_obj, int64_t index);
void pyc_list_set(pyc_obj_t list_obj, int64_t index, pyc_obj_t value);
pyc_obj_t pyc_range_list(int64_t start, int64_t stop, int64_t step);

// Dict operations
pyc_obj_t pyc_dict_set(pyc_obj_t dict_obj, pyc_obj_t key_obj, pyc_obj_t val_obj);
pyc_obj_t pyc_dict_get(pyc_obj_t dict_obj, pyc_obj_t key_obj);

// Membership and identity
int64_t pyc_contains(pyc_obj_t obj, pyc_obj_t item);
int64_t pyc_is(pyc_obj_t obj, pyc_obj_t other);

// String operations
const char* pyc_str_value(pyc_obj_t str_obj);
pyc_obj_t pyc_type_name(pyc_obj_t obj);
int64_t pyc_len(pyc_obj_t obj);

// Attribute access
pyc_obj_t pyc_getattr(pyc_obj_t obj, const char* name);
void pyc_setattr(pyc_obj_t obj, const char* name, pyc_obj_t value);

// Type operations
int64_t pyc_isinstance(pyc_obj_t obj, pyc_type_kind_t type_kind);
pyc_obj_t pyc_object_init(pyc_obj_t obj);

// Math operations
double pyc_pow(int64_t base, int64_t exp);
int64_t pyc_int_from_double(double val);

// I/O
void pyc_print(pyc_obj_t obj);

// Memory management
void pyc_ref_inc(pyc_obj_t obj);
void pyc_ref_dec(pyc_obj_t obj);

// Exception handling
void pyc_raise_exception(pyc_obj_t exc);
pyc_obj_t pyc_get_exception();
void pyc_clear_exception();

// Module loading
pyc_obj_t pyc_import_module(const char* module_name);

} // namespace pyc::runtime
