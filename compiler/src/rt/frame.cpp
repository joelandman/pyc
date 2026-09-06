#define Py_BUILD_CORE 1
#include <Python.h>
#include <frameobject.h>
#include "internal/pycore_frame.h"
#include "internal/pycore_interpframe.h"

// Link a PyFrameObject into tstate->current_frame so CPython builtins that
// read _PyThreadState_GetFrame (locals, globals, eval, sys._getframe) see it.
//
// PyFrame_New already builds a complete interpreter frame (owner
// FRAME_OWNED_BY_FRAME_OBJECT, instr_ptr past firsttraceable). The only
// internal access is f->f_frame and tstate->current_frame; both come from
// the target headers, so a layout shift is a compile error (CHARTER I8).
//
// This is C1a: eager PyFrameObject. CHARTER measured 59.88 ns/call. C1b
// replaces it with a C-stack _PyInterpreterFrame.

extern "C" int pyc_rt_install_frame(PyFrameObject* f,
                                    struct _PyInterpreterFrame** saved) {
    if (!f || !f->f_frame) return -1;
    PyThreadState* ts = PyThreadState_Get();
    *saved = ts->current_frame;
    f->f_frame->previous = *saved;
    ts->current_frame = f->f_frame;
    return 0;
}

extern "C" void pyc_rt_uninstall_frame(struct _PyInterpreterFrame* saved) {
    PyThreadState_Get()->current_frame = saved;
}

static void frame_capsule_dtor(PyObject* cap) {
    PyFrameObject* fo = (PyFrameObject*)PyCapsule_GetPointer(cap, "pyc.frame");
    if (!fo) { PyErr_Clear(); return; }
    PyThreadState* ts = PyThreadState_Get();
    if (fo->f_frame && ts->current_frame == fo->f_frame)
        ts->current_frame = fo->f_frame->previous;
    Py_DECREF(fo);
}

// Push a frame whose f_locals is `locals` (the class namespace, or any
// mapping). The returned capsule's destructor pops the frame, so a landing
// pad that decrefs it unwinds correctly. Locals is borrowed; we INCREF for
// PyFrame_New, which steals.
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
    Py_INCREF(locals);
    PyFrameObject* fo = PyFrame_New(PyThreadState_Get(), co, g, locals);
    Py_DECREF(co);
    if (!fo) { Py_DECREF(locals); return nullptr; }
    struct _PyInterpreterFrame* saved = nullptr;
    if (pyc_rt_install_frame(fo, &saved) < 0) {
        Py_DECREF(fo);
        return nullptr;
    }
    PyObject* cap = PyCapsule_New(fo, "pyc.frame", frame_capsule_dtor);
    if (!cap) {
        pyc_rt_uninstall_frame(saved);
        Py_DECREF(fo);
        return nullptr;
    }
    return cap;
}
