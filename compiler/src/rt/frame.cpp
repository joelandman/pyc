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
