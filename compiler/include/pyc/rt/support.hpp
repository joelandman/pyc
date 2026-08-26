#pragma once
// Runtime helpers the generated code calls (INTERFACES.md §4).
//
// These exist so codegen emits ONE call per IR instruction instead of
// open-coding namespace lookup or argument unpacking in LLVM IR. Anything
// with a decision in it lives here, in C++, where it can be read and tested;
// the emitted IR stays a mechanical translation of pyc::ir.
#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

// A lowered Python function: takes its fast-locals array, returns a new
// reference or NULL with an exception set.
typedef PyObject* (*PycImpl)(PyObject** locals);

// Namespace access. Globals fall back to builtins, then raise NameError --
// which is why `print` needs no special case anywhere in lowering.
PyObject* pyc_rt_load_global(const char* name);          // new ref, or NULL
int       pyc_rt_store_global(const char* name, PyObject* v);

// A local read before assignment is UnboundLocalError, not a fallback to a
// global of the same name (the distinction scope.cpp exists to preserve).
PyObject* pyc_rt_load_local(PyObject** locals, int slot, const char* name);
void      pyc_rt_store_local(PyObject** locals, int slot, PyObject* v);

// Wrap a lowered function as a Python callable.
// `argnames` is the parameter list, needed so the callee can bind keyword
// arguments by name. Without it a compiled function is METH_VARARGS and
// rejects every keyword call -- which turned an honest compile error into a
// runtime TypeError when keyword CALLS started lowering.
// `defaults` is a tuple covering the LAST k named parameters, or NULL.
// vararg_slot / kwarg_slot are local slot indices for *args / **kwargs, or -1.
//
// Defaults are evaluated once at DEF time and shared across calls -- the
// behaviour behind the mutable-default surprise -- so they are built by the
// caller, not here.
PyObject* pyc_rt_make_function(const char* name, PycImpl impl,
                               int nargs, int nkwonly, int nlocals,
                               const char* const* argnames,
                               PyObject* defaults, PyObject* kwdefaults,
                               int vararg_slot, int kwarg_slot,
                               PyObject** closure, int nfree);

// Vectorcall over an argument array.
PyObject* pyc_rt_call(PyObject* callable, PyObject** args, Py_ssize_t nargs);

// Literals. The integer literal arrives as DECIMAL TEXT and is parsed by
// CPython, so a value of any magnitude is exact -- there is no path here
// through a machine word.
PyObject* pyc_rt_int_from_text(const char* digits);
PyObject* pyc_rt_str(const char* utf8, Py_ssize_t len);
PyObject* pyc_rt_bytes(const char* data, Py_ssize_t len);
PyObject* pyc_rt_none(void);

// Build a class from an already-populated namespace.
//
// CPython's __build_class__ runs the body with the frame's f_locals bound to
// the namespace mapping, which a compiled callable cannot do. Instead the body
// is lowered inline with its stores directed into `ns`, and this performs the
// rest of what __build_class__ does: resolve the most-derived metaclass and
// call it. type.__new__ still runs __set_name__ and __init_subclass__, so
// those hooks are not lost.
PyObject* pyc_rt_build_class(const char* name, PyObject* bases, PyObject* ns);

// Raise. Accepts a class or an instance, as `raise` does, and always returns
// -1 so the caller's error edge is taken.
int pyc_rt_raise(PyObject* exc);

// Unpack `value` into exactly `n` items, returning them as a tuple. The
// arity check and its message live here rather than in emitted IR, because
// CPython's wording ("not enough values to unpack (expected 3, got 2)")
// is part of observable behaviour and belongs where it can be read.
PyObject* pyc_rt_unpack(PyObject* value, Py_ssize_t n);

// Make a class-body value behave like a method when it needs to.
//
// A compiled function is a PyCFunction, which is NOT a descriptor, so it never
// binds self. A plain Python function is, which is why CPython needs no such
// step. But a decorator may already have produced a descriptor -- property,
// staticmethod, classmethod -- and wrapping THAT would break it, turning
// `@property def v` into a bound method instead of a computed attribute.
// So the wrap is conditional on the value still being one of ours.
PyObject* pyc_rt_bind_method(PyObject* v);

// Context-manager protocol. __enter__ and __exit__ are looked up on the TYPE,
// not the instance, which is what the language specifies and what makes the
// protocol work for classes that define them.
PyObject* pyc_rt_cm_exit(PyObject* mgr);       // the bound __exit__
PyObject* pyc_rt_cm_enter(PyObject* mgr);      // result of __enter__
int pyc_rt_exit_normal(PyObject* exitf);       // exit(None, None, None)
// Calls exit(type, value, tb) with the exception currently set.
//   1  suppressed -- the error is cleared and execution continues
//   0  not suppressed -- the exception is restored for propagation
//  -1  __exit__ itself failed
int pyc_rt_exit_exc(PyObject* exitf);

// Append every item of `iterable` to `list`. Used for `*x` in a call or a
// literal, where the star's contents are spliced rather than nested.
int pyc_rt_extend(PyObject* list, PyObject* iterable);

// `assert cond, msg` -- raises AssertionError. msg may be NULL.
int pyc_rt_assert_fail(PyObject* msg);
// `del name` at module scope.
int pyc_rt_del_global(const char* name);

// Read a cell. An empty cell means the variable was read before assignment,
// which CPython reports as NameError for a free variable -- PyCell_Get would
// simply return NULL with no exception set, which would look like a crash.
PyObject* pyc_rt_cell_get(PyObject* cell);

// Class-body name lookup: namespace, then globals, then builtins (LOAD_NAME).
PyObject* pyc_rt_load_classname(PyObject* ns, const char* name);

// Unresolvable zero-argument super(); raises the RuntimeError CPython raises.
int pyc_rt_super_fail(int has_args);

// Pattern matching. Py_None means "did not match"; a tuple holds the extracted
// values; NULL with an exception set is a real error.
PyObject* pyc_rt_match_sequence(PyObject* subj, Py_ssize_t nbefore,
                                Py_ssize_t nafter, int has_star);
PyObject* pyc_rt_match_mapping(PyObject* subj, PyObject* keys, int want_rest);
PyObject* pyc_rt_match_class(PyObject* subj, PyObject* cls, int npos,
                             PyObject* kwnames);

// Append one traceback entry (file/function/line) to the exception being
// propagated. Innermost frame first. See pyc_rt_add_traceback in support.cpp.
void pyc_rt_add_traceback(PyObject** cache, const char* file,
                          const char* func, int line);

// A generator expression: marshalled code object + closure cells + the outer
// iterator, run by the linked interpreter. See rebuild/GENERATORS.md.
PyObject* pyc_rt_make_genexp(const char* blob, Py_ssize_t len, PyObject** cache,
                             PyObject* closure, PyObject* iterator);

// A generator function: marshalled code object + closure cells + defaults.
PyObject* pyc_rt_make_genfunc(const char* blob, Py_ssize_t len, PyObject** cache,
                              PyObject* closure, PyObject* defaults);

// Bare `raise`: re-raise whatever exception is currently being handled.
int pyc_rt_reraise(void);
// Entering/leaving an except block. CPython keeps a separate "currently
// handled" exception, which is what bare `raise` and sys.exc_info() read --
// clearing the raised error is not the same thing. push returns the previous
// handled exception so nesting restores correctly.
PyObject* pyc_rt_push_handled(PyObject* exc);
int pyc_rt_pop_handled(PyObject* prev);
// `from mod import *`
int pyc_rt_import_star(PyObject* mod);

// Extended unpacking: `a, *rest, b = value`. Returns a tuple of
// nbefore + 1 + nafter items whose middle element is a list holding whatever
// the fixed positions did not claim.
PyObject* pyc_rt_unpack_ex(PyObject* value, Py_ssize_t nbefore, Py_ssize_t nafter);

#ifdef __cplusplus
}
#endif
