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
PyObject* pyc_rt_make_function(const char* name, PycImpl impl,
                               int nargs, int nlocals);

// Vectorcall over an argument array.
PyObject* pyc_rt_call(PyObject* callable, PyObject** args, Py_ssize_t nargs);

// Literals. The integer literal arrives as DECIMAL TEXT and is parsed by
// CPython, so a value of any magnitude is exact -- there is no path here
// through a machine word.
PyObject* pyc_rt_int_from_text(const char* digits);
PyObject* pyc_rt_str(const char* utf8, Py_ssize_t len);
PyObject* pyc_rt_bytes(const char* data, Py_ssize_t len);
PyObject* pyc_rt_none(void);

#ifdef __cplusplus
}
#endif
