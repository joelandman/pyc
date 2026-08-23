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
                               int nargs, int nlocals,
                               const char* const* argnames,
                               PyObject* defaults,
                               int vararg_slot, int kwarg_slot);

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

#ifdef __cplusplus
}
#endif
