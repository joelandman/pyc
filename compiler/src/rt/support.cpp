#include "pyc/rt/support.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {

static PyObject* globals_dict() {
    PyObject* m = PyImport_AddModule("__main__");   // borrowed
    return m ? PyModule_GetDict(m) : nullptr;       // borrowed
}

PyObject* pyc_rt_load_global(const char* name) {
    PyObject* g = globals_dict();
    if (!g) return nullptr;
    PyObject* key = PyUnicode_FromString(name);
    if (!key) return nullptr;

    PyObject* v = nullptr;
    if (PyDict_GetItemRef(g, key, &v) < 0) { Py_DECREF(key); return nullptr; }
    if (v) { Py_DECREF(key); return v; }

    // Not a module global: try builtins. This is what makes `print` an
    // ordinary name lookup rather than a special case in lowering (I3).
    PyObject* b = PyEval_GetBuiltins();             // borrowed
    if (b && PyDict_GetItemRef(b, key, &v) < 0) { Py_DECREF(key); return nullptr; }
    if (v) { Py_DECREF(key); return v; }

    PyErr_Format(PyExc_NameError, "name '%s' is not defined", name);
    Py_DECREF(key);
    return nullptr;
}

int pyc_rt_store_global(const char* name, PyObject* v) {
    PyObject* g = globals_dict();
    if (!g) return -1;
    return PyDict_SetItemString(g, name, v);        // INCREFs v
}

PyObject* pyc_rt_load_local(PyObject** locals, int slot, const char* name) {
    PyObject* v = locals[slot];
    if (!v) {
        PyErr_Format(PyExc_UnboundLocalError,
                     "cannot access local variable '%s' where it is not "
                     "associated with a value", name);
        return nullptr;
    }
    Py_INCREF(v);
    return v;
}

void pyc_rt_store_local(PyObject** locals, int slot, PyObject* v) {
    PyObject* old = locals[slot];
    Py_XINCREF(v);
    locals[slot] = v;
    Py_XDECREF(old);          // after the store: a self-assignment must not free
}

// --- callables -------------------------------------------------------------

namespace {
// Each function owns its PyMethodDef so ml_name carries its real name.
// Setting __name__ afterwards does not work: it is read-only on a
// builtin_function_or_method, and the failed SetAttr left an exception set
// that surfaced later as an unrelated SystemError.
struct Bound { PycImpl impl; int nargs; int nlocals; PyMethodDef def; char* name; };

void bound_free(PyObject* cap) {
    Bound* b = static_cast<Bound*>(PyCapsule_GetPointer(cap, "pyc.bound"));
    if (b) { std::free(b->name); delete b; }
}

PyObject* trampoline(PyObject* self, PyObject* args) {
    Bound* b = static_cast<Bound*>(PyCapsule_GetPointer(self, "pyc.bound"));
    if (!b) return nullptr;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n != b->nargs) {
        PyErr_Format(PyExc_TypeError,
                     "function takes %d positional argument%s but %zd %s given",
                     b->nargs, b->nargs == 1 ? "" : "s", n,
                     n == 1 ? "was" : "were");
        return nullptr;
    }
    PyObject** locals = new (std::nothrow) PyObject*[b->nlocals ? b->nlocals : 1];
    if (!locals) return PyErr_NoMemory();
    for (int i = 0; i < b->nlocals; ++i) locals[i] = nullptr;
    for (int i = 0; i < b->nargs; ++i) {
        PyObject* a = PyTuple_GET_ITEM(args, i);    // borrowed
        Py_INCREF(a);
        locals[i] = a;
    }
    PyObject* r = b->impl(locals);
    for (int i = 0; i < b->nlocals; ++i) Py_XDECREF(locals[i]);
    delete[] locals;
    return r;
}

}  // namespace

PyObject* pyc_rt_make_function(const char* name, PycImpl impl,
                               int nargs, int nlocals) {
    char* owned = strdup(name);
    if (!owned) return PyErr_NoMemory();
    Bound* b = new (std::nothrow) Bound{impl, nargs, nlocals, {}, owned};
    if (!b) { std::free(owned); return PyErr_NoMemory(); }
    b->def = PyMethodDef{b->name, trampoline, METH_VARARGS, nullptr};
    PyObject* cap = PyCapsule_New(b, "pyc.bound", bound_free);
    if (!cap) { std::free(owned); delete b; return nullptr; }
    PyObject* fn = PyCFunction_New(&b->def, cap);
    Py_DECREF(cap);                                  // PyCFunction_New holds it
    return fn;
}

PyObject* pyc_rt_call(PyObject* callable, PyObject** args, Py_ssize_t nargs) {
    return PyObject_Vectorcall(callable, args, (size_t)nargs, nullptr);
}

PyObject* pyc_rt_int_from_text(const char* digits) {
    return PyLong_FromString(digits, nullptr, 10);
}
PyObject* pyc_rt_str(const char* utf8, Py_ssize_t len) {
    return PyUnicode_DecodeUTF8(utf8, len, "surrogatepass");
}
PyObject* pyc_rt_bytes(const char* data, Py_ssize_t len) {
    return PyBytes_FromStringAndSize(data, len);
}

}  // extern "C"

extern "C" PyObject* pyc_rt_none(void) { Py_RETURN_NONE; }
