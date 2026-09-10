#define Py_BUILD_CORE 1
#include <Python.h>
#include "internal/pycore_interpframe.h"
#include "internal/pycore_code.h"
#include "internal/pycore_ceval.h"

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
    f->f_locals = locals;
    if (locals) Py_INCREF(locals);
    f->frame_obj = nullptr;
    f->instr_ptr = _PyCode_CODE(code) + code->_co_firsttraceable + 1;
    f->stackpointer = f->localsplus;
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
    // A real PyFunctionObject is required (PycFunc fails PyFunction_Check).
    // `func` is the def-time snapshot: constructing here from live globals
    // would pick up `__name__ = "test.test_metaclass"` and miss sys.modules.
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
