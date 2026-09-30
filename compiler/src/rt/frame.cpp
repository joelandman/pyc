#define Py_BUILD_CORE 1
#include <Python.h>
#include "pyc/rt/support.hpp"
#include <new>
#include "cpython/funcobject.h"
#include "internal/pycore_interpframe.h"
#include "internal/pycore_code.h"
#include "internal/pycore_ceval.h"
#include "internal/pycore_frame.h"
#include "internal/pycore_instruments.h"
#include "internal/pycore_call.h"

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

// GIL-free phi-while (unboxing step 11): the body is provably heap-free, so
// it MAY run without the GIL. But 3.14's SaveThread/RestoreThread is a full
// stop-the-world-aware GIL handoff (~54 ns/pair, measured), and CPython's
// own eval loop releases only when another thread set the drop-request bit.
// This is that request, checked per iteration with one relaxed atomic load:
// nobody waiting keeps the GIL held at ~zero cost, and a waiting thread gets
// the per-iteration preemption the unconditional drop used to give. Returns
// 1 if the GIL was dropped (the existing pyc_rt_gil_acquire sites must run),
// 0 if it stayed held.
extern "C" int pyc_rt_gil_maybe_release(void) {
    if (!_Py_eval_breaker_bit_is_set(PyThreadState_Get(),
                                     _PY_GIL_DROP_REQUEST_BIT))
        return 0;
    pyc_rt_gil_release();
    return 1;
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
    // A code object compiled by CPython already carries cell and free bits,
    // including argument flags on the same byte. Replacing the byte drops
    // those flags. The stub's cells are still plain CO_FAST_LOCAL until the
    // prologue stores a cell, which is the only case that needs a write.
    unsigned char cur = (unsigned char)kinds[slot];
    if (cur & (CO_FAST_CELL | CO_FAST_FREE)) return;
    int nfast = co->co_nlocals;
    if (slot >= nfast)
        kinds[slot] = (char)CO_FAST_FREE;
    else
        kinds[slot] = (char)(cur | CO_FAST_LOCAL | CO_FAST_CELL);
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

// CPython's super_init_without_args: the class cell is whatever free
// variable of the executing frame is named __class__, not a slot chosen
// when the function was compiled.
extern "C" PyObject* pyc_rt_super_from_frame(void) {
    PyThreadState* ts = PyThreadState_Get();
    _PyInterpreterFrame* f = ts ? ts->current_frame : nullptr;
    PyCodeObject* co = nullptr;
    if (f)
        co = reinterpret_cast<PyCodeObject*>(
            PyStackRef_AsPyObjectBorrow(f->f_executable));
    if (!co || !PyCode_Check(co) || co->co_argcount == 0) {
        PyErr_SetString(PyExc_RuntimeError, "super(): no arguments");
        return nullptr;
    }
    if (co->co_nlocalsplus < 1 || PyStackRef_IsNull(f->localsplus[0])) {
        PyErr_SetString(PyExc_RuntimeError, "super(): arg[0] deleted");
        return nullptr;
    }
    PyObject* firstarg = PyStackRef_AsPyObjectBorrow(f->localsplus[0]);
    if (co->co_localspluskinds && PyBytes_Check(co->co_localspluskinds)
        && PyBytes_GET_SIZE(co->co_localspluskinds) > 0
        && ((unsigned char)PyBytes_AS_STRING(co->co_localspluskinds)[0] & CO_FAST_CELL)) {
        if (!firstarg || !PyCell_Check(firstarg)) {
            PyErr_SetString(PyExc_RuntimeError, "super(): arg[0] deleted");
            return nullptr;
        }
        firstarg = PyCell_Get(firstarg);
        if (!firstarg) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_RuntimeError, "super(): arg[0] deleted");
            return nullptr;
        }
    } else {
        Py_INCREF(firstarg);
    }

    static PyObject* klass_name = nullptr;
    if (!klass_name) {
        klass_name = PyUnicode_InternFromString("__class__");
        if (!klass_name) { Py_DECREF(firstarg); return nullptr; }
    }
    PyObject* type = nullptr;
    PyObject* names = co->co_localsplusnames;
    int first_free = PyUnstable_Code_GetFirstFree(co);
    for (int i = first_free; i < co->co_nlocalsplus; ++i) {
        PyObject* name = PyTuple_GET_ITEM(names, i);
        int eq = PyObject_RichCompareBool(name, klass_name, Py_EQ);
        if (eq < 0) { Py_DECREF(firstarg); return nullptr; }
        if (!eq) continue;
        if (PyStackRef_IsNull(f->localsplus[i])) {
            PyErr_SetString(PyExc_RuntimeError, "super(): bad __class__ cell");
            Py_DECREF(firstarg);
            return nullptr;
        }
        PyObject* cell = PyStackRef_AsPyObjectBorrow(f->localsplus[i]);
        if (!cell || !PyCell_Check(cell)) {
            PyErr_SetString(PyExc_RuntimeError, "super(): bad __class__ cell");
            Py_DECREF(firstarg);
            return nullptr;
        }
        type = PyCell_Get(cell);
        if (!type) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_RuntimeError, "super(): empty __class__ cell");
            Py_DECREF(firstarg);
            return nullptr;
        }
        if (!PyType_Check(type)) {
            PyErr_Format(PyExc_RuntimeError,
                         "super(): __class__ is not a type (%s)",
                         Py_TYPE(type)->tp_name);
            Py_DECREF(type);
            Py_DECREF(firstarg);
            return nullptr;
        }
        break;
    }
    if (!type) {
        PyErr_SetString(PyExc_RuntimeError, "super(): __class__ cell not found");
        Py_DECREF(firstarg);
        return nullptr;
    }
    PyObject* builtins = PyEval_GetBuiltins();
    PyObject* fn = builtins ? PyDict_GetItemString(builtins, "super") : nullptr;
    if (!fn) {
        PyErr_SetString(PyExc_SystemError, "super(): builtin missing");
        Py_DECREF(type);
        Py_DECREF(firstarg);
        return nullptr;
    }
    PyObject* args[2] = {type, firstarg};
    PyObject* out = PyObject_Vectorcall(fn, args, 2, nullptr);
    Py_DECREF(type);
    Py_DECREF(firstarg);
    return out;
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
    int r = ts->c_tracefunc((PyObject*)ts, fo, PyTrace_LINE, Py_None);
    ts->tracing--;
    if (r < 0) return;
}

// Byte offset of a code unit whose location is this source line. The eval
// stub parked the instruction pointer at a fixed slot in its padding; a
// CPython code object has to be searched, or f_lineno and traceback carets
// name whatever opcode happens to sit at that slot.
static int unit_for_source(PyCodeObject* co, int line, int col, int end_col,
                           int legacy) {
    PyObject* raw = PyCode_GetCode(co);
    if (!raw) {
        PyErr_Clear();
        return legacy;
    }
    Py_ssize_t nbytes = PyBytes_GET_SIZE(raw);
    Py_DECREF(raw);
    int exact = -1, start_only = -1, first = -1;
    for (int addr = 0; addr + 1 < nbytes; addr += (int)sizeof(_Py_CODEUNIT)) {
        int sl = 0, el = 0, sc = -1, ec = -1;
        if (!PyCode_Addr2Location(co, addr, &sl, &sc, &el, &ec)) continue;
        if (sl != line) continue;
        int unit = addr / (int)sizeof(_Py_CODEUNIT);
        if (first < 0) first = unit;
        if (col >= 0 && sc == col && start_only < 0) start_only = unit;
        if (col >= 0 && end_col >= 0 && sc == col && ec == end_col && exact < 0)
            exact = unit;
    }
    if (exact >= 0) return exact;
    if (start_only >= 0) return start_only;
    if (first >= 0) return first;
    return legacy;
}

extern "C" void pyc_rt_set_lasti(int slot, int line, int col, int end_col) {
    if (slot < 0 || line < 1) return;
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
    int legacy = 8 + slot;
    if (legacy < co->_co_firsttraceable) legacy = co->_co_firsttraceable;
    if (legacy >= nunits) legacy = (int)nunits - 1;
    int off = legacy;
    int sl = 0, sc = -1, el = 0, ec = -1;
    int legacy_addr = legacy * (int)sizeof(_Py_CODEUNIT);
    if (PyCode_Addr2Location(co, legacy_addr, &sl, &sc, &el, &ec) && sl == line
        && (col < 0 || sc == col)) {
        off = legacy;
    } else {
        off = unit_for_source(co, line, col, end_col, legacy);
        if (off < 0) off = legacy;
        if (off >= nunits) off = (int)nunits - 1;
    }
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

// Highest set tool id in a monitoring bitset. `bits` is never zero.
static int monitor_msb(uint8_t bits) {
    int tool = 7;
    while ((bits & (uint8_t)(1u << tool)) == 0) --tool;
    return tool;
}

static const char* monitor_event_name(int event) {
    switch (event) {
    case PY_MONITORING_EVENT_PY_UNWIND: return "PY_UNWIND";
    case PY_MONITORING_EVENT_CALL: return "CALL";
    case PY_MONITORING_EVENT_C_RETURN: return "C_RETURN";
    case PY_MONITORING_EVENT_C_RAISE: return "C_RAISE";
    default: return "monitoring";
    }
}

// C_RETURN and C_RAISE are ancillary: the tool bit lives on CALL, and the
// callable is stored under the ancillary event id.
static int monitor_tool_event(int event) {
    if (event == PY_MONITORING_EVENT_C_RETURN ||
        event == PY_MONITORING_EVENT_C_RAISE)
        return PY_MONITORING_EVENT_CALL;
    return event;
}

// Compiled calls never enter ceval, and stub code objects have no per-code
// monitors, so _Py_call_instrumentation would see an empty tool set.
// sys.setprofile and cProfile both register on the interpreter-global bits.
// tstate->tracing is held across each callback; without that the callback
// (itself a compiled function) re-enters this helper forever.
// `extras` is borrowed. nextra 0 is PY_START; 1 is a return value or
// exception; 2 is (callable, arg0) for CALL / C_RETURN / C_RAISE.
static int fire_monitor(_PyInterpreterFrame* f, int event,
                        PyObject* const* extras, int nextra) {
    if (!f) return 0;
    PyThreadState* ts = PyThreadState_Get();
    if (!ts || ts->tracing) return 0;
    PyCodeObject* code = reinterpret_cast<PyCodeObject*>(
        PyStackRef_AsPyObjectBorrow(f->f_executable));
    if (!code || (code->co_flags & CO_NO_MONITORING_EVENTS)) return 0;
    int tool_event = monitor_tool_event(event);
    if (tool_event < 0 || tool_event >= _PY_MONITORING_UNGROUPED_EVENTS) return 0;
    if (event < 0 || event >= _PY_MONITORING_EVENTS) return 0;
    PyInterpreterState* interp = ts->interp;
    uint8_t tools = interp->monitors.tools[tool_event];
    if (!tools) return 0;

    _Py_CODEUNIT* base = _PyCode_CODE(code);
    long units = 0;
    if (f->instr_ptr && base && f->instr_ptr >= base)
        units = (long)(f->instr_ptr - base);
    PyObject* off = PyLong_FromLong(units * (long)sizeof(_Py_CODEUNIT));
    if (!off) return -1;

    int nargs = 2 + nextra;
    PyObject* slot[5] = {nullptr, reinterpret_cast<PyObject*>(code), off, nullptr, nullptr};
    for (int i = 0; i < nextra && i < 2; ++i) slot[3 + i] = extras[i];
    size_t nargsf = (size_t)nargs | PY_VECTORCALL_ARGUMENTS_OFFSET;
    PyObject** callargs = &slot[1];
    int err = 0;
    while (tools) {
        int tool = monitor_msb(tools);
        tools = (uint8_t)(tools & ~(uint8_t)(1u << tool));
        PyObject* instrument = interp->monitoring_callables[tool][event];
        if (!instrument) continue;
        int old_what = ts->what_event;
        ts->what_event = event;
        ts->tracing++;
        PyObject* res = _PyObject_VectorcallTstate(
            ts, instrument, callargs, nargsf, nullptr);
        ts->tracing--;
        ts->what_event = old_what;
        if (!res) { err = -1; break; }
        int disabled = res == &_PyInstrumentation_DISABLE;
        Py_DECREF(res);
        if (!disabled) continue;
        // Local events: removing a tool rewrites bytecode we do not run.
        // PEP 669 allows a spurious event after DISABLE.
        if (PY_MONITORING_IS_INSTRUMENTED_EVENT(event)) continue;
        PyErr_Format(PyExc_ValueError,
                     "Cannot disable %s events. Callback removed.",
                     monitor_event_name(event));
        Py_CLEAR(interp->monitoring_callables[tool][event]);
        err = -1;
        break;
    }
    Py_DECREF(off);
    return err;
}

static _PyInterpreterFrame* caller_frame() {
    PyThreadState* ts = PyThreadState_Get();
    return ts ? ts->current_frame : nullptr;
}

extern "C" int pyc_rt_profile_enter(void* frame) {
    return fire_monitor(static_cast<_PyInterpreterFrame*>(frame),
                        PY_MONITORING_EVENT_PY_START, nullptr, 0);
}

extern "C" int pyc_rt_profile_return(void* frame, PyObject* retval) {
    auto* f = static_cast<_PyInterpreterFrame*>(frame);
    if (retval)
        return fire_monitor(f, PY_MONITORING_EVENT_PY_RETURN, &retval, 1);
    // A NULL return with no exception is an internal failure, not an unwind.
    if (!PyErr_Occurred()) return 0;
    PyObject* exc = PyErr_GetRaisedException();
    if (!exc) return 0;
    int err = fire_monitor(f, PY_MONITORING_EVENT_PY_UNWIND, &exc, 1);
    if (err == 0) PyErr_SetRaisedException(exc);
    else Py_DECREF(exc);
    return err;
}

// CALL / C_RETURN / C_RAISE for a Python-level call. arg0 NULL means the
// instrumentation missing-arg sentinel (no positional argument).
static PyObject* profile_arg0(PyObject* arg0) {
    return arg0 ? arg0 : &_PyInstrumentation_MISSING;
}

extern "C" int pyc_rt_profile_ccall(PyObject* callable, PyObject* arg0) {
    PyThreadState* ts = PyThreadState_Get();
    if (!ts || ts->tracing || !ts->current_frame) return 0;
    if (!ts->interp->monitors.tools[PY_MONITORING_EVENT_CALL]) return 0;
    PyObject* extras[2] = {callable, profile_arg0(arg0)};
    if (fire_monitor(ts->current_frame, PY_MONITORING_EVENT_CALL, extras, 2) < 0)
        return -1;
    return 1;
}

extern "C" int pyc_rt_profile_creturn(PyObject* callable, PyObject* arg0) {
    PyObject* extras[2] = {callable, profile_arg0(arg0)};
    return fire_monitor(caller_frame(), PY_MONITORING_EVENT_C_RETURN, extras, 2);
}

extern "C" int pyc_rt_profile_craise(PyObject* callable, PyObject* arg0) {
    PyThreadState* ts = PyThreadState_Get();
    if (!ts || ts->tracing) return 0;
    if (!ts->interp->monitors.tools[PY_MONITORING_EVENT_CALL]) return 0;
    PyObject* exc = PyErr_GetRaisedException();
    if (!exc) return 0;
    PyObject* extras[2] = {callable, profile_arg0(arg0)};
    int err = fire_monitor(ts->current_frame, PY_MONITORING_EVENT_C_RAISE,
                           extras, 2);
    if (err == 0) PyErr_SetRaisedException(exc);
    else Py_DECREF(exc);
    return err;
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
