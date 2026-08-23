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
struct Bound { PycImpl impl; int nargs; int nlocals; PyMethodDef def;
               char* name; const char* const* argnames; };

void bound_free(PyObject* cap) {
    Bound* b = static_cast<Bound*>(PyCapsule_GetPointer(cap, "pyc.bound"));
    if (b) { std::free(b->name); delete b; }
}

PyObject* trampoline(PyObject* self, PyObject* args, PyObject* kwargs) {
    Bound* b = static_cast<Bound*>(PyCapsule_GetPointer(self, "pyc.bound"));
    if (!b) return nullptr;
    Py_ssize_t npos = PyTuple_GET_SIZE(args);
    if (npos > b->nargs) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes %d positional argument%s but %zd %s given",
                     b->name, b->nargs, b->nargs == 1 ? "" : "s", npos,
                     npos == 1 ? "was" : "were");
        return nullptr;
    }
    PyObject** locals = new (std::nothrow) PyObject*[b->nlocals ? b->nlocals : 1];
    if (!locals) return PyErr_NoMemory();
    for (int i = 0; i < b->nlocals; ++i) locals[i] = nullptr;
    for (Py_ssize_t i = 0; i < npos; ++i) {
        PyObject* a = PyTuple_GET_ITEM(args, i);    // borrowed
        Py_INCREF(a);
        locals[i] = a;
    }
    // Bind keywords by parameter name, rejecting duplicates and unknowns the
    // way CPython does rather than silently ignoring them.
    if (kwargs) {
        PyObject *k, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &k, &val)) {
            const char* ks = PyUnicode_AsUTF8(k);
            if (!ks) goto fail;
            int slot = -1;
            for (int i = 0; i < b->nargs; ++i)
                if (std::strcmp(ks, b->argnames[i]) == 0) { slot = i; break; }
            if (slot < 0) {
                PyErr_Format(PyExc_TypeError,
                             "%s() got an unexpected keyword argument '%s'",
                             b->name, ks);
                goto fail;
            }
            if (locals[slot]) {
                PyErr_Format(PyExc_TypeError,
                             "%s() got multiple values for argument '%s'",
                             b->name, ks);
                goto fail;
            }
            Py_INCREF(val);
            locals[slot] = val;
        }
    }
    for (int i = 0; i < b->nargs; ++i) {
        if (!locals[i]) {
            PyErr_Format(PyExc_TypeError,
                         "%s() missing required argument '%s'",
                         b->name, b->argnames[i]);
            goto fail;
        }
    }
    {
        PyObject* r = b->impl(locals);
        for (int i = 0; i < b->nlocals; ++i) Py_XDECREF(locals[i]);
        delete[] locals;
        return r;
    }
fail:
    for (int i = 0; i < b->nlocals; ++i) Py_XDECREF(locals[i]);
    delete[] locals;
    return nullptr;
}

}  // namespace

PyObject* pyc_rt_make_function(const char* name, PycImpl impl,
                               int nargs, int nlocals,
                               const char* const* argnames) {
    char* owned = strdup(name);
    if (!owned) return PyErr_NoMemory();
    Bound* b = new (std::nothrow) Bound{impl, nargs, nlocals, {}, owned, argnames};
    if (!b) { std::free(owned); return PyErr_NoMemory(); }
    b->def = PyMethodDef{b->name,
                     reinterpret_cast<PyCFunction>(
                         reinterpret_cast<void(*)()>(trampoline)),
                     METH_VARARGS | METH_KEYWORDS, nullptr};
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

extern "C" PyObject* pyc_rt_build_class(const char* name, PyObject* bases,
                                        PyObject* ns) {
    // Most-derived metaclass, as type.__call__ requires: using `type`
    // unconditionally breaks any class whose base has a custom metaclass
    // (ABCMeta, enum.EnumMeta), and does so with a confusing error far from
    // the cause.
    PyObject* meta = reinterpret_cast<PyObject*>(&PyType_Type);
    Py_ssize_t n = PyTuple_GET_SIZE(bases);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* b = PyTuple_GET_ITEM(bases, i);        // borrowed
        PyObject* bt = reinterpret_cast<PyObject*>(Py_TYPE(b));
        int sub = PyObject_IsSubclass(bt, meta);
        if (sub < 0) return nullptr;
        if (sub) meta = bt;
    }
    PyObject* nm = PyUnicode_FromString(name);
    if (!nm) return nullptr;
    PyObject* cls = PyObject_CallFunctionObjArgs(meta, nm, bases, ns, nullptr);
    Py_DECREF(nm);
    return cls;
}

extern "C" int pyc_rt_raise(PyObject* exc) {
    // `raise E` and `raise E(...)` are both legal: a class is instantiated by
    // PyErr_SetObject, an instance is raised as-is. Anything else is the
    // TypeError CPython gives, rather than a confusing failure later.
    if (PyExceptionClass_Check(exc)) {
        PyErr_SetObject(exc, nullptr);
    } else if (PyExceptionInstance_Check(exc)) {
        PyErr_SetObject(PyExceptionInstance_Class(exc), exc);
    } else {
        PyErr_SetString(PyExc_TypeError,
                        "exceptions must derive from BaseException");
    }
    return -1;
}

extern "C" PyObject* pyc_rt_unpack(PyObject* value, Py_ssize_t n) {
    PyObject* t = PySequence_Tuple(value);      // works for any iterable
    if (!t) {
        if (PyErr_ExceptionMatches(PyExc_TypeError)) {
            PyErr_Clear();
            PyErr_Format(PyExc_TypeError, "cannot unpack non-iterable %s object",
                         Py_TYPE(value)->tp_name);
        }
        return nullptr;
    }
    Py_ssize_t got = PyTuple_GET_SIZE(t);
    if (got < n) {
        PyErr_Format(PyExc_ValueError,
                     "not enough values to unpack (expected %zd, got %zd)", n, got);
        Py_DECREF(t);
        return nullptr;
    }
    if (got > n) {
        // CPython 3.14 includes the actual count here; older versions did not.
        // The wording is observable, and the differential harness flagged the
        // omission as a P0 -- nothing else would have caught it.
        PyErr_Format(PyExc_ValueError,
                     "too many values to unpack (expected %zd, got %zd)", n, got);
        Py_DECREF(t);
        return nullptr;
    }
    return t;
}
