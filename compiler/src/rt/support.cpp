#include "pyc/rt/support.hpp"

#include <marshal.h>   // PyMarshal_ReadObjectFromString

#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <new>

extern "C" {

static PyObject* globals_dict() {
    PyObject* m = PyImport_AddModule("__main__");   // borrowed
    return m ? PyModule_GetDict(m) : nullptr;       // borrowed
}

// Name lookup inside a CLASS BODY. CPython compiles these to LOAD_NAME:
// the class namespace first, then globals, then builtins. Going straight to
// the global path makes a class-level name invisible to anything else in the
// body -- `x = 7` then `def get(self, k, default=x)` raised NameError, because
// the default is evaluated in the class body, where x is a namespace entry and
// not a global.
PyObject* pyc_rt_load_classname(PyObject* ns, const char* name) {
    PyObject* key = PyUnicode_FromString(name);
    if (!key) return nullptr;
    PyObject* v = nullptr;
    if (PyDict_GetItemRef(ns, key, &v) < 0) { Py_DECREF(key); return nullptr; }
    Py_DECREF(key);
    if (v) return v;
    return pyc_rt_load_global(name);
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
struct Bound { PycImpl impl; int nargs; int nlocals;
               char* name; const char* const* argnames;
               PyObject* defaults; int vararg; int kwarg;
               PyObject* closure; int nfree; };

PyObject* trampoline(Bound* b, PyObject* args, PyObject* kwargs) {
    if (!b) return nullptr;
    Py_ssize_t npos = PyTuple_GET_SIZE(args);
    if (npos > b->nargs && b->vararg < 0) {
        // CPython names the whole accepted RANGE when defaults make the lower
        // bound differ: "takes from 1 to 2 positional arguments".
        Py_ssize_t ndef0 = b->defaults ? PyTuple_GET_SIZE(b->defaults) : 0;
        if (ndef0 > 0)
            PyErr_Format(PyExc_TypeError,
                         "%s() takes from %zd to %d positional argument%s "
                         "but %zd %s given",
                         b->name, (Py_ssize_t)b->nargs - ndef0, b->nargs,
                         b->nargs == 1 ? "" : "s", npos,
                         npos == 1 ? "was" : "were");
        else
            PyErr_Format(PyExc_TypeError,
                         "%s() takes %d positional argument%s but %zd %s given",
                         b->name, b->nargs, b->nargs == 1 ? "" : "s", npos,
                         npos == 1 ? "was" : "were");
        return nullptr;
    }
    std::vector<const char*> missing;
    PyObject** locals = new (std::nothrow) PyObject*[b->nlocals ? b->nlocals : 1];
    if (!locals) return PyErr_NoMemory();
    for (int i = 0; i < b->nlocals; ++i) locals[i] = nullptr;
    Py_ssize_t nnamed = npos < b->nargs ? npos : b->nargs;
    for (Py_ssize_t i = 0; i < nnamed; ++i) {
        PyObject* a = PyTuple_GET_ITEM(args, i);    // borrowed
        Py_INCREF(a);
        locals[i] = a;
    }
    if (b->vararg >= 0) {
        PyObject* extra = PyTuple_GetSlice(args, nnamed, npos);
        if (!extra) goto fail;
        locals[b->vararg] = extra;                  // owned
    }
    if (b->kwarg >= 0) {
        locals[b->kwarg] = PyDict_New();
        if (!locals[b->kwarg]) goto fail;
    }
    // Free variables occupy the LAST nfree slots, holding the cells
    // themselves so writes through them are visible to the enclosing scope.
    for (int i = 0; i < b->nfree; ++i) {
        PyObject* cell = PyTuple_GET_ITEM(b->closure, i);
        Py_INCREF(cell);
        locals[b->nlocals - b->nfree + i] = cell;
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
                // Unmatched keywords go to **kwargs when the function has one;
                // otherwise they are the error CPython gives.
                if (b->kwarg >= 0) {
                    if (PyDict_SetItem(locals[b->kwarg], k, val) < 0) goto fail;
                    continue;
                }
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
    {
        // Defaults cover the LAST k parameters, so parameter i takes
        // defaults[i - (nargs - k)].
        Py_ssize_t ndef = b->defaults ? PyTuple_GET_SIZE(b->defaults) : 0;
        Py_ssize_t first_def = b->nargs - ndef;
        for (int i = 0; i < b->nargs; ++i) {
            if (locals[i]) continue;
            if (ndef && i >= first_def) {
                PyObject* d = PyTuple_GET_ITEM(b->defaults, i - first_def);
                Py_INCREF(d);
                locals[i] = d;
                continue;
            }
            missing.push_back(b->argnames[i]);
        }
        if (!missing.empty()) {
            // CPython reports ALL missing parameters in one message, counted
            // and joined: "missing 2 required positional arguments: 'a' and
            // 'b'"; three or more use an Oxford comma.
            std::string names;
            for (std::size_t k2 = 0; k2 < missing.size(); ++k2) {
                if (k2) names += (missing.size() == 2) ? " and "
                               : (k2 + 1 == missing.size() ? ", and " : ", ");
                names += "'"; names += missing[k2]; names += "'";
            }
            PyErr_Format(PyExc_TypeError,
                         "%s() missing %zd required positional argument%s: %s",
                         b->name, (Py_ssize_t)missing.size(),
                         missing.size() == 1 ? "" : "s", names.c_str());
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

namespace {
// A real function object.
//
// pyc functions used to be PyCFunction. A PyCFunction is not a descriptor, so
// it does not bind self on attribute access: methods needed an explicit
// PyInstanceMethod wrapper at class-definition time, and a function assigned
// to a class LATER (`cls.__repr__ = f`, the decorator idiom) was never wrapped
// and lost self entirely. It also reprs as "<built-in method f>" where CPython
// says "<function f at 0x...>".
//
// Both are the same root cause, so both are fixed in the same place: give the
// object a type of its own that implements tp_descr_get and tp_repr the way a
// Python function does.
// name and dict are writable: functools.wraps assigns __name__, __qualname__,
// __doc__, __module__ and __wrapped__ onto the wrapper, so a read-only
// function object makes every @functools.wraps decorator fail.
struct PycFunc { PyObject_HEAD Bound* b; PyObject* name; PyObject* doc; PyObject* dict; };

PyObject* func_call(PyObject* self, PyObject* args, PyObject* kwargs) {
    return trampoline(reinterpret_cast<PycFunc*>(self)->b, args, kwargs);
}

PyObject* func_descr_get(PyObject* self, PyObject* obj, PyObject* /*type*/) {
    // Accessed on the class itself, not an instance: stay unbound.
    if (!obj || obj == Py_None) { Py_INCREF(self); return self; }
    return PyMethod_New(self, obj);
}

PyObject* func_repr(PyObject* self) {
    PycFunc* f = reinterpret_cast<PycFunc*>(self);
    return PyUnicode_FromFormat("<function %U at %p>", f->name, self);
}

PyObject* func_get_name(PyObject* self, void*) {
    PyObject* n = reinterpret_cast<PycFunc*>(self)->name;
    Py_INCREF(n);
    return n;
}

PyObject* func_get_doc(PyObject* self, void*) {
    PyObject* d = reinterpret_cast<PycFunc*>(self)->doc;
    if (!d) d = Py_None;                // absent docstring reads as None
    Py_INCREF(d);
    return d;
}

int func_set_doc(PyObject* self, PyObject* v, void*) {
    PycFunc* f = reinterpret_cast<PycFunc*>(self);
    Py_XINCREF(v);
    Py_XSETREF(f->doc, v);
    return 0;
}

int func_set_name(PyObject* self, PyObject* v, void*) {
    if (!v || !PyUnicode_Check(v)) {
        PyErr_SetString(PyExc_TypeError,
                        "__name__ must be set to a string object");
        return -1;
    }
    PycFunc* f = reinterpret_cast<PycFunc*>(self);
    Py_INCREF(v);
    Py_XSETREF(f->name, v);
    return 0;
}

void func_dealloc(PyObject* self) {
    PycFunc* f = reinterpret_cast<PycFunc*>(self);
    if (f->b) { Py_XDECREF(f->b->defaults); Py_XDECREF(f->b->closure);
                std::free(f->b->name); delete f->b; }
    Py_XDECREF(f->name);
    Py_XDECREF(f->doc);
    Py_XDECREF(f->dict);
    Py_TYPE(self)->tp_free(self);
}

PyGetSetDef func_getset[] = {
    {"__name__", func_get_name, func_set_name, nullptr, nullptr},
    {"__qualname__", func_get_name, func_set_name, nullptr, nullptr},
    // tp_dictoffset alone gives the object storage but no way to REACH it;
    // functools.wraps reads wrapper.__dict__ directly.
    {"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, nullptr, nullptr},
    {"__doc__", func_get_doc, func_set_doc, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyTypeObject PycFuncType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "function",                     // tp_name -- what type(f).__name__ reports
    sizeof(PycFunc),
};

bool init_func_type() {
    static bool done = false, okv = false;
    if (done) return okv;
    done = true;
    PycFuncType.tp_flags = Py_TPFLAGS_DEFAULT;
    PycFuncType.tp_call = func_call;
    PycFuncType.tp_repr = func_repr;
    PycFuncType.tp_descr_get = func_descr_get;
    PycFuncType.tp_dealloc = func_dealloc;
    PycFuncType.tp_getset = func_getset;
    PycFuncType.tp_getattro = PyObject_GenericGetAttr;
    PycFuncType.tp_setattro = PyObject_GenericSetAttr;
    PycFuncType.tp_dictoffset = offsetof(PycFunc, dict);
    PycFuncType.tp_new = nullptr;
    okv = PyType_Ready(&PycFuncType) == 0;
    return okv;
}
}  // namespace

PyObject* pyc_rt_make_function(const char* name, PycImpl impl,
                               int nargs, int nlocals,
                               const char* const* argnames,
                               PyObject* defaults,
                               int vararg_slot, int kwarg_slot,
                               PyObject** closure, int nfree) {
    char* owned = strdup(name);
    if (!owned) return PyErr_NoMemory();
    Py_XINCREF(defaults);
    // The closure is captured as a tuple of CELLS, not values: the whole point
    // is that the inner function sees later assignments to the outer variable.
    PyObject* clo = nullptr;
    if (nfree > 0) {
        clo = PyTuple_New(nfree);
        if (!clo) { Py_XDECREF(defaults); std::free(owned); return nullptr; }
        for (int i = 0; i < nfree; ++i) {
            Py_INCREF(closure[i]);
            PyTuple_SET_ITEM(clo, i, closure[i]);
        }
    }
    Bound* b = new (std::nothrow) Bound{impl, nargs, nlocals, owned,
                                        argnames, defaults,
                                        vararg_slot, kwarg_slot, clo, nfree};
    if (!b) { Py_XDECREF(defaults); std::free(owned); return PyErr_NoMemory(); }
    if (!init_func_type()) { Py_XDECREF(defaults); std::free(owned); delete b; return nullptr; }
    PycFunc* fn = PyObject_New(PycFunc, &PycFuncType);
    if (!fn) { Py_XDECREF(defaults); std::free(owned); delete b; return nullptr; }
    fn->b = b;
    fn->dict = nullptr;                 // created lazily by generic setattr
    fn->doc = nullptr;                  // reads as None until a docstring is set
    fn->name = PyUnicode_FromString(name);
    if (!fn->name) { Py_DECREF(fn); return nullptr; }
    return reinterpret_cast<PyObject*>(fn);
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

extern "C" PyObject* pyc_rt_bind_method(PyObject* v) {
    // A pyc function is now a descriptor in its own right, so there is nothing
    // to wrap: binding happens on attribute access, for methods defined in the
    // class body and for functions assigned to the class afterwards alike.
    Py_XINCREF(v);
    return v;
}

namespace {
PyObject* type_lookup(PyObject* mgr, const char* name) {
    // Special-method lookup skips the instance dict, as the language requires.
    PyObject* t = reinterpret_cast<PyObject*>(Py_TYPE(mgr));
    PyObject* f = PyObject_GetAttrString(t, name);
    if (!f) {
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError,
                     "'%s' object does not support the context manager protocol",
                     Py_TYPE(mgr)->tp_name);
        return nullptr;
    }
    return f;
}
}  // namespace

extern "C" PyObject* pyc_rt_cm_exit(PyObject* mgr) {
    PyObject* f = type_lookup(mgr, "__exit__");
    if (!f) return nullptr;
    PyObject* bound = PyMethod_Check(f) ? f : PyObject_GetAttrString(mgr, "__exit__");
    if (bound != f) { Py_DECREF(f); if (!bound) return nullptr; }
    return bound;
}

extern "C" PyObject* pyc_rt_cm_enter(PyObject* mgr) {
    PyObject* f = type_lookup(mgr, "__enter__");
    if (!f) return nullptr;
    PyObject* r = PyObject_CallOneArg(f, mgr);
    Py_DECREF(f);
    return r;
}

extern "C" int pyc_rt_exit_normal(PyObject* exitf) {
    PyObject* r = PyObject_CallFunctionObjArgs(exitf, Py_None, Py_None, Py_None, nullptr);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}

extern "C" int pyc_rt_exit_exc(PyObject* exitf) {
    PyObject* exc = PyErr_GetRaisedException();          // clears the indicator
    if (!exc) return 0;
    PyObject* type = reinterpret_cast<PyObject*>(Py_TYPE(exc));
    PyObject* tb = PyException_GetTraceback(exc);
    PyObject* r = PyObject_CallFunctionObjArgs(exitf, type, exc,
                                               tb ? tb : Py_None, nullptr);
    Py_XDECREF(tb);
    if (!r) { Py_DECREF(exc); return -1; }
    int suppress = PyObject_IsTrue(r);
    Py_DECREF(r);
    if (suppress < 0) { Py_DECREF(exc); return -1; }
    if (suppress) { Py_DECREF(exc); return 1; }
    PyErr_SetRaisedException(exc);                        // steals exc
    return 0;
}

extern "C" int pyc_rt_extend(PyObject* list, PyObject* iterable) {
    PyObject* it = PyObject_GetIter(iterable);
    if (!it) return -1;
    for (;;) {
        PyObject* item = PyIter_Next(it);
        if (!item) break;
        int r = PyList_Append(list, item);
        Py_DECREF(item);
        if (r < 0) { Py_DECREF(it); return -1; }
    }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}

extern "C" PyObject* pyc_rt_unpack_ex(PyObject* value, Py_ssize_t nbefore,
                                      Py_ssize_t nafter) {
    PyObject* all = PySequence_Tuple(value);
    if (!all) return nullptr;
    Py_ssize_t n = PyTuple_GET_SIZE(all);
    if (n < nbefore + nafter) {
        PyErr_Format(PyExc_ValueError,
                     "not enough values to unpack (expected at least %zd, got %zd)",
                     nbefore + nafter, n);
        Py_DECREF(all);
        return nullptr;
    }
    PyObject* out = PyTuple_New(nbefore + 1 + nafter);
    if (!out) { Py_DECREF(all); return nullptr; }
    for (Py_ssize_t i = 0; i < nbefore; ++i) {
        PyObject* v = PyTuple_GET_ITEM(all, i);
        Py_INCREF(v);
        PyTuple_SET_ITEM(out, i, v);
    }
    PyObject* mid = PyList_New(0);
    if (!mid) { Py_DECREF(all); Py_DECREF(out); return nullptr; }
    for (Py_ssize_t i = nbefore; i < n - nafter; ++i) {
        if (PyList_Append(mid, PyTuple_GET_ITEM(all, i)) < 0) {
            Py_DECREF(mid); Py_DECREF(all); Py_DECREF(out); return nullptr;
        }
    }
    PyTuple_SET_ITEM(out, nbefore, mid);            // steals mid
    for (Py_ssize_t i = 0; i < nafter; ++i) {
        PyObject* v = PyTuple_GET_ITEM(all, n - nafter + i);
        Py_INCREF(v);
        PyTuple_SET_ITEM(out, nbefore + 1 + i, v);
    }
    Py_DECREF(all);
    return out;
}

// Zero-argument super() that could not be resolved at compile time. CPython
// raises at RUN time here, with a message that depends on why: a frame with no
// arguments at all has nothing to bind, while one with arguments but no class
// cell was simply not compiled inside a class body.
// A generator expression (rebuild/GENERATORS.md). The code object was
// compiled by CPython at BUILD time and marshalled into the binary; pyc
// supplies the closure cells and the outer iterator, and the linked
// interpreter runs the body. The result is a real generator -- same type,
// same laziness, same send/throw/close -- because it IS one.
extern "C" PyObject* pyc_rt_make_genexp(const char* blob, Py_ssize_t len,
                                        PyObject** cache, PyObject* closure,
                                        PyObject* iterator) {
    if (!*cache) {
        // Unmarshalled once per call site, not once per evaluation: a genexp
        // inside a loop would otherwise re-read the code object every time.
        *cache = PyMarshal_ReadObjectFromString(const_cast<char*>(blob), len);
        if (!*cache) return nullptr;
    }
    PyObject* g = globals_dict();
    if (!g) return nullptr;
    PyObject* fn = PyFunction_New(*cache, g);
    if (!fn) return nullptr;
    if (closure && closure != Py_None
        && PyFunction_SetClosure(fn, closure) < 0) { Py_DECREF(fn); return nullptr; }
    PyObject* gen = PyObject_CallOneArg(fn, iterator);
    Py_DECREF(fn);
    return gen;
}

extern "C" int pyc_rt_super_fail(int has_args) {
    PyErr_SetString(PyExc_RuntimeError,
                    has_args ? "super(): __class__ cell not found"
                             : "super(): no arguments");
    return -1;
}

extern "C" int pyc_rt_assert_fail(PyObject* msg) {
    if (msg) PyErr_SetObject(PyExc_AssertionError, msg);
    else     PyErr_SetNone(PyExc_AssertionError);
    return -1;
}

extern "C" int pyc_rt_del_global(const char* name) {
    PyObject* g = globals_dict();
    if (!g) return -1;
    if (PyDict_DelItemString(g, name) < 0) {
        // CPython reports a missing global as NameError, not KeyError.
        PyErr_Clear();
        PyErr_Format(PyExc_NameError, "name '%s' is not defined", name);
        return -1;
    }
    return 0;
}

extern "C" PyObject* pyc_rt_cell_get(PyObject* cell) {
    PyObject* v = PyCell_Get(cell);
    if (!v && !PyErr_Occurred())
        PyErr_SetString(PyExc_NameError,
                        "free variable referenced before assignment in enclosing scope");
    return v;
}

extern "C" int pyc_rt_reraise(void) {
    PyObject* exc = PyErr_GetHandledException();
    if (!exc) {
        PyErr_SetString(PyExc_RuntimeError, "No active exception to reraise");
        return -1;
    }
    PyErr_SetRaisedException(exc);        // steals exc
    return -1;
}

extern "C" int pyc_rt_import_star(PyObject* mod) {
    // Honour __all__ when present; otherwise copy public names, which is what
    // `import *` means. Copying everything would drag in imported modules and
    // private helpers the author did not intend to export.
    PyObject* g = globals_dict();
    if (!g) return -1;
    PyObject* all = PyObject_GetAttrString(mod, "__all__");
    if (all) {
        PyObject* seq = PySequence_Fast(all, "__all__ must be a sequence");
        Py_DECREF(all);
        if (!seq) return -1;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* nm = PySequence_Fast_GET_ITEM(seq, i);   // borrowed
            PyObject* v = PyObject_GetAttr(mod, nm);
            if (!v) { Py_DECREF(seq); return -1; }
            int r = PyDict_SetItem(g, nm, v);
            Py_DECREF(v);
            if (r < 0) { Py_DECREF(seq); return -1; }
        }
        Py_DECREF(seq);
        return 0;
    }
    PyErr_Clear();
    PyObject* d = PyObject_GetAttrString(mod, "__dict__");
    if (!d) return -1;
    PyObject *k, *v;
    Py_ssize_t pos = 0;
    while (PyDict_Next(d, &pos, &k, &v)) {
        const char* ks = PyUnicode_AsUTF8(k);
        if (!ks) { Py_DECREF(d); return -1; }
        if (ks[0] == '_') continue;
        if (PyDict_SetItem(g, k, v) < 0) { Py_DECREF(d); return -1; }
    }
    Py_DECREF(d);
    return 0;
}

extern "C" PyObject* pyc_rt_push_handled(PyObject* exc) {
    PyObject* prev = PyErr_GetHandledException();     // new ref or NULL
    Py_XINCREF(exc);
    PyErr_SetHandledException(exc);                   // steals
    // NULL would be indistinguishable from failure at the call site, so an
    // absent previous exception is reported as None.
    if (!prev) Py_RETURN_NONE;
    return prev;
}

extern "C" int pyc_rt_pop_handled(PyObject* prev) {
    PyObject* p = (prev == Py_None) ? nullptr : prev;
    Py_XINCREF(p);
    PyErr_SetHandledException(p);                     // steals
    return 0;
}
