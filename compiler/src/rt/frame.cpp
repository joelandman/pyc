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
    pyc_rt_gil_ensure();
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
    // CPython's eval loop counts Python frames via py_recursion_remaining
    // (_Py_EnterRecursivePy). Py_EnterRecursiveCall only checks C stack, so
    // sys.setrecursionlimit was a no-op for compiled calls.
    if (ts->py_recursion_remaining-- <= 0) {
        ts->py_recursion_remaining++;
        PyErr_SetString(PyExc_RecursionError,
                        "maximum recursion depth exceeded");
        ts->current_frame = f->previous;
        if (locals) Py_DECREF(locals);
        PyStackRef_CLOSE(f->f_funcobj);
        PyStackRef_CLOSE(f->f_executable);
        _PyThreadState_PopFrame(ts, f);
        return nullptr;
    }
    return f;
}

#ifndef CO_FAST_LOCAL
#define CO_FAST_LOCAL 0x20
#define CO_FAST_CELL  0x40
#define CO_FAST_FREE  0x80
#endif

static _PyInterpreterFrame* frame_from_arg_or_current(void* vp) {
    if (vp) return static_cast<_PyInterpreterFrame*>(vp);
    PyThreadState* ts = PyThreadState_Get();
    return ts ? ts->current_frame : nullptr;
}

static void mark_cell_kind(_PyInterpreterFrame* f, int slot) {
    PyCodeObject* co = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    if (!co || slot < 0 || slot >= co->co_nlocalsplus) return;
    if (!co->co_localspluskinds || !PyBytes_Check(co->co_localspluskinds)) return;
    char* kinds = PyBytes_AS_STRING(co->co_localspluskinds);
    if (slot >= PyBytes_GET_SIZE(co->co_localspluskinds)) return;
    int nfast = co->co_nlocals;
    kinds[slot] = (slot >= nfast)
        ? (char)CO_FAST_FREE
        : (char)(CO_FAST_LOCAL | CO_FAST_CELL);
}

extern "C" PyObject* pyc_rt_frame_local_borrow(void* vp, int slot) {
    _PyInterpreterFrame* f = frame_from_arg_or_current(vp);
    if (!f || slot < 0) return nullptr;
    PyCodeObject* co = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    int nplus = co ? co->co_nlocalsplus : 0;
    if (nplus < 0 || slot >= nplus) return nullptr;
    return PyStackRef_AsPyObjectBorrow(f->localsplus[slot]);
}

extern "C" void pyc_rt_frame_local_set(void* vp, int slot, PyObject* v) {
    _PyInterpreterFrame* f = frame_from_arg_or_current(vp);
    if (!f || slot < 0) { Py_XINCREF(v); return; }
    PyCodeObject* co = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    int nplus = co ? co->co_nlocalsplus : 0;
    if (nplus < 0 || slot >= nplus) { Py_XINCREF(v); return; }
    _PyStackRef old = f->localsplus[slot];
    f->localsplus[slot] = v ? PyStackRef_FromPyObjectNew(v) : PyStackRef_NULL;
    PyStackRef_XCLOSE(old);
    if (v && PyCell_Check(v)) mark_cell_kind(f, slot);
}

extern "C" int pyc_rt_frame_local_is_null(int slot) {
    _PyInterpreterFrame* f = frame_from_arg_or_current(nullptr);
    if (!f || slot < 0) return -1;
    PyCodeObject* co = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    int nplus = co ? co->co_nlocalsplus : 0;
    if (nplus < 0 || slot >= nplus) return -1;
    return PyStackRef_IsNull(f->localsplus[slot]) ? 1 : 0;
}

static void maybe_line_trace(_PyInterpreterFrame* f, PyThreadState* ts, int line) {
    if (!ts || ts->tracing || !ts->c_tracefunc) return;
    while (f && _PyFrame_IsIncomplete(f)) f = f->previous;
    if (!f) return;
    PyFrameObject* fo = _PyFrame_GetFrameObject(f);
    if (!fo) { PyErr_Clear(); return; }
    if (!fo->f_trace_lines) return;
    if (line > 0) fo->f_lineno = line;
    ts->tracing++;
    int r = ts->c_tracefunc(ts->c_traceobj, fo, PyTrace_LINE, Py_None);
    ts->tracing--;
    if (r < 0) return;
}

extern "C" void pyc_rt_set_lasti(int slot, int line) {
    if (slot < 0) return;
    pyc_rt_gil_ensure();
    PyThreadState* ts = PyThreadState_Get();
    _PyInterpreterFrame* f = ts->current_frame;
    if (!f) return;
    PyCodeObject* co = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    if (!co) return;
    int off = 8 + slot;
    if (off < co->_co_firsttraceable) off = co->_co_firsttraceable;
#ifdef Py_DEBUG
    PyObject* raw = PyCode_GetCode(co);
    if (raw) {
        Py_ssize_t nunits = PyBytes_GET_SIZE(raw) / (Py_ssize_t)sizeof(_Py_CODEUNIT);
        if (off >= nunits) off = nunits > 0 ? (int)nunits - 1 : 0;
        Py_DECREF(raw);
    } else {
        PyErr_Clear();
    }
#endif
    f->instr_ptr = _PyCode_CODE(co) + off;
    if (f->frame_obj && line > 0) f->frame_obj->f_lineno = line;
    if (ts->c_tracefunc) maybe_line_trace(f, ts, line);
}

extern "C" void pyc_rt_set_lineno(int line) {
    pyc_rt_set_location(line, -1, -1);
}

extern "C" void pyc_rt_set_location(int line, int col, int end_col) {
    (void)col;
    (void)end_col;
    if (line < 1) return;
    pyc_rt_gil_ensure();
    PyThreadState* ts = PyThreadState_Get();
    _PyInterpreterFrame* f = ts->current_frame;
    if (!f) return;
    PyCodeObject* co = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    if (!co) return;
    PyObject* raw = PyCode_GetCode(co);
    if (!raw) { PyErr_Clear(); return; }
    Py_ssize_t nunits = PyBytes_GET_SIZE(raw) / (Py_ssize_t)sizeof(_Py_CODEUNIT);
    Py_DECREF(raw);
    if (nunits <= 0) return;
    int off = line - co->co_firstlineno;
    if (off < 0) off = 0;
    if (off < co->_co_firsttraceable) off = co->_co_firsttraceable;
    if (off >= nunits) off = (int)nunits - 1;
    f->instr_ptr = _PyCode_CODE(co) + off;
    if (f->frame_obj) f->frame_obj->f_lineno = line;
    if (ts->c_tracefunc) maybe_line_trace(f, ts, line);
}

extern "C" void pyc_rt_traceback_here(void) {
    pyc_rt_gil_ensure();
    PyObject *t = nullptr, *v = nullptr, *tb = nullptr;
    PyErr_Fetch(&t, &v, &tb);
    if (!t) return;
    PyThreadState* ts = PyThreadState_Get();
    _PyInterpreterFrame* f = ts->current_frame;
    while (f && _PyFrame_IsIncomplete(f)) f = f->previous;
    if (!f) { PyErr_Restore(t, v, tb); return; }
    PyFrameObject* fo = _PyFrame_GetFrameObject(f);
    if (!fo) { PyErr_Clear(); PyErr_Restore(t, v, tb); return; }
    if (tb && PyTraceBack_Check(tb)) {
        PyTracebackObject* tbo = reinterpret_cast<PyTracebackObject*>(tb);
        if (tbo->tb_frame == fo) {
            PyErr_Restore(t, v, tb);
            return;
        }
    }
    PyErr_Restore(t, v, tb);
    PyTraceBack_Here(fo);
}

static void snapshot_newlocals_into_fast(_PyInterpreterFrame* f) {
    PyObject* dict = f->f_locals;
    if (!dict || !PyDict_Check(dict)) return;
    PyCodeObject* co = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    if (!co || !(co->co_flags & CO_NEWLOCALS)) return;
    PyObject* names = co->co_localsplusnames;
    if (!names || !PyTuple_Check(names)) {
        Py_CLEAR(f->f_locals);
        return;
    }
    int nplus = co->co_nlocalsplus;
    Py_ssize_t nt = PyTuple_GET_SIZE(names);
    if (nplus > nt) nplus = (int)nt;
    char* kinds = nullptr;
    Py_ssize_t nk = 0;
    if (co->co_localspluskinds && PyBytes_Check(co->co_localspluskinds)) {
        kinds = PyBytes_AS_STRING(co->co_localspluskinds);
        nk = PyBytes_GET_SIZE(co->co_localspluskinds);
    }
    PyObject *et = nullptr, *ev = nullptr, *tb = nullptr;
    PyErr_Fetch(&et, &ev, &tb);
    for (int i = 0; i < nplus; ++i) {
        if (kinds && i < nk && (kinds[i] & (CO_FAST_CELL | CO_FAST_FREE)))
            continue;
        PyObject* name = PyTuple_GET_ITEM(names, i);
        PyObject* val = PyDict_GetItemWithError(dict, name);
        if (!val && PyErr_Occurred()) PyErr_Clear();
        if (!PyStackRef_IsNull(f->localsplus[i]))
            PyStackRef_CLOSE(f->localsplus[i]);
        f->localsplus[i] = val ? PyStackRef_FromPyObjectNew(val)
                               : PyStackRef_NULL;
    }
    Py_CLEAR(f->f_locals);
    PyErr_Restore(et, ev, tb);
}

extern "C" void pyc_rt_interp_leave(void* frame) {
    if (!frame) return;
    auto* f = static_cast<_PyInterpreterFrame*>(frame);
    PyThreadState* ts = PyThreadState_Get();
    ts->py_recursion_remaining++;
    if (ts->current_frame == f) ts->current_frame = f->previous;
    if (f->frame_obj)
        snapshot_newlocals_into_fast(f);
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
    PyCodeObject* co = PyCode_NewEmpty(pyc_rt_source_file(), nm, 1);
    if (!co) return nullptr;
    // GetLocals on an optimized code object rebuilds f_locals from
    // empty fast locals and drops the class namespace mapping, so
    // locals()["x"]=43 would not be visible to LOAD_NAME.
    co->co_flags &= ~CO_OPTIMIZED;
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
    if (!code) {
        PyErr_SetString(PyExc_RuntimeError, "pyc eval without a code object");
        return nullptr;
    }
    auto* co = reinterpret_cast<PyCodeObject*>(code);
    int nfree = (int)PyCode_GetNumFree(co);
    if (nfree > 0 && !PyStackRef_IsNull(f->f_funcobj)) {
        PyObject* func = PyStackRef_AsPyObjectBorrow(f->f_funcobj);
        if (func && PyFunction_Check(func)) {
            PyObject* clo = PyFunction_GET_CLOSURE(func);
            if (clo && PyTuple_Check(clo)) {
                Py_ssize_t cn = PyTuple_GET_SIZE(clo);
                int base = PyUnstable_Code_GetFirstFree(co);
                for (int i = 0; i < nfree && i < cn; ++i) {
                    PyObject* cell = PyTuple_GET_ITEM(clo, i);
                    if (PyCell_Check(cell))
                        pyc_rt_frame_local_set(f, base + i, cell);
                }
            }
        }
    }
    return pyc_rt_invoke_code(code, f);
}
