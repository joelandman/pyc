#define Py_BUILD_CORE 1
#include <Python.h>
#include "pyc/rt/support.hpp"
#include <new>
#include "cpython/funcobject.h"
#include "internal/pycore_interpframe.h"
#include "internal/pycore_code.h"
#include "internal/pycore_ceval.h"
#include "internal/pycore_frame.h"

// C1b: an interpreter frame on CPython's datastack, not a PyFrameObject.
//
// PyFrame_New (C1a) was measured at 59.88 ns/call — 2.43× slower than
// CPython. Pushing _PyInterpreterFrame via _PyThreadState_PushFrame and
// leaving PyFrameObject lazy is the path CHARTER costed (~8–12 ns).
//
// The iframe lives on the thread's datastack (heap chunks), so a lazy
// PyFrameObject created by sys._getframe can take ownership via
// _PyFrame_ClearExceptCode rather than dangle at a C-stack address.
//
// Layout comes from the target headers. A field rename or GIL/free-threaded
// split is a compile error here (I8). GIL 3.14.7: FRAME_SPECIALS_SIZE == 10.

#ifndef Py_GIL_DISABLED
static_assert(FRAME_SPECIALS_SIZE == 10,
              "cp314 _PyInterpreterFrame specials changed; update C1b");
#endif

// CPython's eval loop calls this when eval_breaker has events. It runs
// signal handlers AND detaches the GIL only if another thread asked
// (_PY_GIL_DROP_REQUEST_BIT). Unconditional SaveThread/RestoreThread
// deadlocked test_logging; this is the request-only path.
extern "C" int pyc_rt_handle_pending(void) {
    return _Py_HandlePending(PyThreadState_Get());
}

extern "C" void* pyc_rt_interp_enter(PyCodeObject* code, PyObject* globals,
                                     PyObject* locals, PyObject* func) {
    if (!code || !globals) return nullptr;
    PyThreadState* ts = PyThreadState_Get();
    int size = code->co_framesize;
    if (size < FRAME_SPECIALS_SIZE) size = FRAME_SPECIALS_SIZE;
    _PyInterpreterFrame* f = _PyThreadState_PushFrame(ts, (size_t)size);
    if (!f) { PyErr_NoMemory(); return nullptr; }
    f->previous = ts->current_frame;
    f->f_executable = PyStackRef_FromPyObjectNew(reinterpret_cast<PyObject*>(code));
    f->f_globals = globals;
    f->f_builtins = PyEval_GetBuiltins();
    if (func && PyFunction_Check(func)) {
        auto* fo = reinterpret_cast<PyFunctionObject*>(func);
        if (fo->func_globals) f->f_globals = fo->func_globals;
        if (fo->func_builtins) f->f_builtins = fo->func_builtins;
    }
    f->f_locals = locals;
    if (locals) Py_INCREF(locals);
    f->frame_obj = nullptr;
    f->instr_ptr = _PyCode_CODE(code) + code->_co_firsttraceable + 1;
    {
        int nplus = code->co_nlocalsplus;
        if (nplus < 0) nplus = 0;
        for (int i = 0; i < nplus; ++i) f->localsplus[i] = PyStackRef_NULL;
        f->stackpointer = f->localsplus + nplus;
    }
#ifdef Py_GIL_DISABLED
    f->tlbc_index = 0;
#endif
    f->return_offset = 0;
    f->owner = FRAME_OWNED_BY_THREAD;
    f->visited = 0;
#ifdef Py_DEBUG
    f->lltrace = 0;
#endif
    // sys._getframemodulename reads f_funcobj via PyFunction_GetModule.
    // Compiled callables are real PyFunction objects; `func` is the def-time
    // function so a later `__name__` rebind on the module is not picked up.
    PyObject* fn = func;
    if (!fn || !PyFunction_Check(fn)) {
        fn = PyFunction_New(reinterpret_cast<PyObject*>(code), globals);
        if (!fn) {
            Py_XDECREF(locals);
            PyStackRef_CLOSE(f->f_executable);
            _PyThreadState_PopFrame(ts, f);
            return nullptr;
        }
        f->f_funcobj = PyStackRef_FromPyObjectNew(fn);
        Py_DECREF(fn);
    } else {
        f->f_funcobj = PyStackRef_FromPyObjectNew(fn);
    }
    ts->current_frame = f;
    return f;
}

#ifndef CO_FAST_LOCAL
#define CO_FAST_LOCAL 0x20
#define CO_FAST_CELL  0x40
#define CO_FAST_FREE  0x80
#endif

extern "C" void pyc_rt_interp_fill_locals(void* frame, PyObject** locals, int n) {
    if (!frame || !locals || n <= 0) return;
    auto* f = static_cast<_PyInterpreterFrame*>(frame);
    PyCodeObject* co = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    int nplus = co ? co->co_nlocalsplus : 0;
    int nfast = co ? co->co_nlocals : 0;
    int m = n < nplus ? n : nplus;
    char* kinds = nullptr;
    Py_ssize_t nk = 0;
    if (co && co->co_localspluskinds && PyBytes_Check(co->co_localspluskinds)) {
        kinds = PyBytes_AS_STRING(co->co_localspluskinds);
        nk = PyBytes_GET_SIZE(co->co_localspluskinds);
    }
    for (int i = 0; i < m; ++i) {
        if (locals[i])
            f->localsplus[i] = PyStackRef_FromPyObjectNew(locals[i]);
        else
            f->localsplus[i] = PyStackRef_NULL;
        if (kinds && i < nk) {
            if (locals[i] && PyCell_Check(locals[i]))
                kinds[i] = (i >= nfast) ? (char)CO_FAST_FREE : (char)CO_FAST_CELL;
            else
                kinds[i] = (char)CO_FAST_LOCAL;
        }
    }
}

extern "C" void pyc_rt_interp_leave(void* frame) {
    if (!frame) return;
    auto* f = static_cast<_PyInterpreterFrame*>(frame);
    PyThreadState* ts = PyThreadState_Get();
    if (ts->current_frame == f) ts->current_frame = f->previous;
    _PyFrame_ClearExceptCode(f);
    PyStackRef_CLOSE(f->f_executable);
    _PyThreadState_PopFrame(ts, f);
}

static void iframe_capsule_dtor(PyObject* cap) {
    void* f = PyCapsule_GetPointer(cap, "pyc.iframe");
    if (!f) { PyErr_Clear(); return; }
    pyc_rt_interp_leave(f);
}

extern "C" PyObject* pyc_rt_push_frame(PyObject* name, PyObject* locals) {
    PyObject* g = PyEval_GetGlobals();
    if (!g) {
        PyObject* m = PyImport_AddModule("__main__");
        g = m ? PyModule_GetDict(m) : nullptr;
    }
    if (!g || !locals) return nullptr;
    const char* nm = "<class>";
    if (name && PyUnicode_Check(name)) {
        const char* s = PyUnicode_AsUTF8(name);
        if (s && s[0]) nm = s;
    }
    PyCodeObject* co = PyCode_NewEmpty("<pyc>", nm, 1);
    if (!co) return nullptr;
    void* f = pyc_rt_interp_enter(co, g, locals, nullptr);
    Py_DECREF(co);
    if (!f) return nullptr;
    PyObject* cap = PyCapsule_New(f, "pyc.iframe", iframe_capsule_dtor);
    if (!cap) { pyc_rt_interp_leave(f); return nullptr; }
    return cap;
}

extern "C" PyObject* pyc_rt_run_from_frame(PyObject*, PyObject*) {
    PyFrameObject* fo = PyEval_GetFrame();
    _PyInterpreterFrame* f = fo ? fo->f_frame : PyThreadState_Get()->current_frame;
    if (!f) {
        PyErr_SetString(PyExc_RuntimeError, "pyc eval without a frame");
        return nullptr;
    }
    PyObject* code = PyStackRef_AsPyObjectBorrow(f->f_executable);
    auto* co = reinterpret_cast<PyCodeObject*>(code);
    int nfast = co->co_nlocals;
    int nfree = (int)PyCode_GetNumFree(co);
    int n = nfast + nfree;
    if (n < 0) n = 0;
    _PyStackRef* arr = _PyFrame_GetLocalsArray(f);
    PyObject** locals = new (std::nothrow) PyObject*[n ? n : 1];
    if (!locals) return PyErr_NoMemory();
    for (int i = 0; i < n; ++i) locals[i] = nullptr;
    for (int i = 0; i < nfast && i < co->co_nlocalsplus; ++i) {
        if (PyStackRef_IsNull(arr[i])) continue;
        PyObject* o = PyStackRef_AsPyObjectBorrow(arr[i]);
        locals[i] = o;
        Py_XINCREF(o);
    }
    PyObject* clo = nullptr;
    if (!PyStackRef_IsNull(f->f_funcobj)) {
        PyObject* func = PyStackRef_AsPyObjectBorrow(f->f_funcobj);
        if (func && PyFunction_Check(func)) clo = PyFunction_GET_CLOSURE(func);
    }
    if (clo && PyTuple_Check(clo)) {
        Py_ssize_t cn = PyTuple_GET_SIZE(clo);
        for (int i = 0; i < nfree && i < cn; ++i) {
            PyObject* cell = PyTuple_GET_ITEM(clo, i);
            Py_INCREF(cell);
            locals[nfast + i] = cell;
        }
    } else {
        int base = nfast;
        for (int i = 0; i < nfree && base + i < co->co_nlocalsplus; ++i) {
            if (PyStackRef_IsNull(arr[base + i])) continue;
            PyObject* o = PyStackRef_AsPyObjectBorrow(arr[base + i]);
            locals[nfast + i] = o;
            Py_XINCREF(o);
        }
    }
    PyObject* r = pyc_rt_invoke_code(code, locals);
    for (int i = 0; i < n; ++i) Py_XDECREF(locals[i]);
    delete[] locals;
    return r;
}
