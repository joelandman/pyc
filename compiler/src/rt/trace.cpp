// --trace core: emits a Perfetto (Chrome-JSON) trace of the program's
// execution when PYC_TRACE=<path> is set at run time. Linked only into
// --trace builds; dormant cost is one getenv per hook. See trace.hpp and
// rebuild/TRACING.md for the event model and the measured evidence.
//
// Event source: the runtime's existing CPython monitoring fires (one
// wrapper per generated call; generator/coroutine bodies are real
// bytecode and run in the eval loop, so their events fire natively).
// This file registers C callbacks under the reserved PROFILER_ID and
// appends JSON lines. While active the binary owns the PROFILER_ID: a
// program that tries to install its own profiler under that id fails,
// which is the documented trade for a profiler built in.
//
// The callbacks run with the GIL held (the monitoring delivery contract)
// and must never raise: they are METH_FASTCALL builtins that always
// return None.

#include "pyc/rt/trace.hpp"
#include "pyc/rt/support.hpp"

#include <Python.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

namespace {

FILE* g_fp = nullptr;
bool g_active = false;
bool g_ended = false;
bool g_lines = false;
unsigned long g_main_tid = 0;
unsigned long g_pid = 0;
int g_thread_seq = 0;
std::unordered_map<unsigned long, std::string> g_thread_names;
PyObject* g_tool = nullptr;  // in-process module owning the callback functions

// One entry per open function frame. gen mode = segment mode: generator
// and coroutine frames suspend (PY_YIELD) and resume (PY_RESUME), so their
// slices are emitted as complete (X) events per executed segment instead
// of B/E pairs -- two B's before an E would not be valid Chrome nesting.
struct FuncSlice {
    std::string name;
    std::string file;
    int line = 0;
    bool gen = false;
    bool open = false;      // a segment is currently running
    uint64_t seg_start = 0; // ns, segment start
};
std::vector<FuncSlice> g_stack;

// A call into non-Python code (C builtin, method-wrapper, class): opened
// on CALL, closed as an X slice on C_RETURN / C_RAISE.
struct CSlice { std::string name; uint64_t start; };
std::vector<CSlice> g_cstack;

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void jstr(std::string& out, const char* s) {
    out += '"';
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else out += (char)c;
        }
    }
    out += '"';
}

// ph: "B" open, "E" close, "X" complete, "i" instant, "M" metadata.
// cat/args are optional; for "X" `dur_ns` is the slice duration.
void emit(const char* name, const char* ph, unsigned long tid, uint64_t ts_ns,
          uint64_t dur_ns, const char* cat, const char* extra_args) {
    if (!g_fp) return;
    char tsbuf[32], durbuf[32];
    std::snprintf(tsbuf, sizeof tsbuf, "%.3f", (double)ts_ns / 1000.0);
    std::string s;
    s += "{\"name\":";
    jstr(s, name);
    s += ",\"ph\":\"";
    s += ph;
    s += "\"";
    s += ",\"pid\":";
    s += std::to_string(g_pid);
    s += ",\"tid\":";
    s += std::to_string(tid);
    if (ph[0] != 'M') {
        if (ph[0] == 'i') s += ",\"s\":\"t\"";
        s += ",\"ts\":";
        s += tsbuf;
        if (ph[0] == 'X') {
            std::snprintf(durbuf, sizeof durbuf, "%.3f", (double)dur_ns / 1000.0);
            s += ",\"dur\":";
            s += durbuf;
        }
    }
    if (cat) { s += ",\"cat\":\""; s += cat; s += "\""; }
    if (extra_args) { s += ",\"args\":"; s += extra_args; }
    s += "}\n";
    std::fwrite(s.data(), 1, s.size(), g_fp);
}

void emit_thread_name(unsigned long tid) {
    if (g_thread_names.count(tid)) return;
    std::string nm = (tid == g_main_tid) ? "main" : "thread-" + std::to_string(++g_thread_seq);
    g_thread_names[tid] = nm;
    std::string args = "{\"name\":";
    jstr(args, nm.c_str());
    args += "}";
    emit(nm.c_str(), "M", tid, 0, 0, nullptr, args.c_str());
}

unsigned long cur_tid() {
    PyThreadState* ts = PyThreadState_Get();
    return ts ? ts->thread_id : 0;
}

std::string name_of(PyObject* c) {
    std::string r = "C:";
    PyObject* nm = c ? PyObject_GetAttrString(c, "__name__") : nullptr;
    if (nm) {
        if (PyUnicode_Check(nm)) {
            const char* s = PyUnicode_AsUTF8(nm);
            if (s) r += s;
        }
        Py_DECREF(nm);
        if (r.size() > 2) return r;
    } else if (c) {
        PyErr_Clear();  // never leave an exception set: these callbacks
    }                  // run under the GIL and must return None cleanly
    if (c && Py_TYPE(c)) r += Py_TYPE(c)->tp_name;
    else r += "?";
    return r;
}

// --- monitoring callbacks (METH_FASTCALL; never raise) -------------------

static PyObject* on_py_start(PyObject*, PyObject* const* a, Py_ssize_t n) {
    if (g_active && g_fp && n >= 2) {
        PyCodeObject* co = (PyCodeObject*)a[0];
        FuncSlice sl;
        const char* q = co->co_qualname ? PyUnicode_AsUTF8(co->co_qualname) : nullptr;
        sl.name = q ? q : "?";
        const char* f = co->co_filename ? PyUnicode_AsUTF8(co->co_filename) : nullptr;
        sl.file = f ? f : "";
        sl.line = co->co_firstlineno;
        sl.gen = (co->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ITERABLE_COROUTINE)) != 0;
        sl.open = true;
        sl.seg_start = now_ns();
        unsigned long tid = cur_tid();
        emit_thread_name(tid);
        if (!sl.gen)
            emit(sl.name.c_str(), "B", tid, sl.seg_start, 0, "pyc.func", nullptr);
        g_stack.push_back(std::move(sl));
    }
    Py_RETURN_NONE;
}

static PyObject* on_py_return(PyObject*, PyObject* const* a, Py_ssize_t n) {
    (void)a;
    if (g_active && g_fp && n >= 3 && !g_stack.empty()) {
        FuncSlice& sl = g_stack.back();
        uint64_t ts = now_ns();
        if (sl.gen) {
            if (sl.open)
                emit(sl.name.c_str(), "X", cur_tid(), sl.seg_start, ts - sl.seg_start,
                     "pyc.func", nullptr);
            sl.open = false;
        } else {
            emit(sl.name.c_str(), "E", cur_tid(), ts, 0, nullptr, nullptr);
        }
        g_stack.pop_back();
    }
    Py_RETURN_NONE;
}

static PyObject* on_py_unwind(PyObject*, PyObject* const* a, Py_ssize_t n) {
    (void)a;
    if (g_active && g_fp && n >= 3 && !g_stack.empty()) {
        FuncSlice& sl = g_stack.back();
        uint64_t ts = now_ns();
        const char* args = "{\"exception\":true}";
        if (sl.gen) {
            if (sl.open)
                emit(sl.name.c_str(), "X", cur_tid(), sl.seg_start, ts - sl.seg_start,
                     "pyc.func", args);
            sl.open = false;
        } else {
            emit(sl.name.c_str(), "E", cur_tid(), ts, 0, nullptr, args);
        }
        g_stack.pop_back();
    }
    Py_RETURN_NONE;
}

static PyObject* on_call(PyObject*, PyObject* const* a, Py_ssize_t n) {
    if (g_active && g_fp && n >= 4) {
        PyObject* callable = a[2];
        // A PyFunction (stdlib Python or a pyc function: pyc functions are
        // real PyFunctionObjects whose vectorcall is swapped to the native
        // trampoline) opens its own slice on PY_START. Everything else is
        // time spent outside generated code: a C:<name> slice.
        if (!PyFunction_Check(callable))
            g_cstack.push_back({name_of(callable), now_ns()});
    }
    Py_RETURN_NONE;
}

static void close_c(PyObject*, uint64_t ts, bool raised) {
    if (g_cstack.empty()) return;  // defensive: unbalanced C event, drop
    CSlice& c = g_cstack.back();
    emit(c.name.c_str(), "X", cur_tid(), c.start, ts - c.start,
         "pyc.ccall", raised ? "{\"exception\":true}" : nullptr);
    g_cstack.pop_back();
}

static PyObject* on_creturn(PyObject*, PyObject* const* a, Py_ssize_t n) {
    (void)a;
    if (g_active && g_fp && n >= 3) close_c(nullptr, now_ns(), false);
    Py_RETURN_NONE;
}

static PyObject* on_craise(PyObject*, PyObject* const* a, Py_ssize_t n) {
    (void)a;
    if (g_active && g_fp && n >= 3) close_c(nullptr, now_ns(), true);
    Py_RETURN_NONE;
}

static PyObject* on_py_yield(PyObject*, PyObject* const* a, Py_ssize_t n) {
    (void)a;
    // The frame that yields is the one executing, i.e. the top entry.
    if (g_active && g_fp && n >= 2 && !g_stack.empty()) {
        FuncSlice& sl = g_stack.back();
        if (sl.gen && sl.open) {
            uint64_t ts = now_ns();
            emit(sl.name.c_str(), "X", cur_tid(), sl.seg_start, ts - sl.seg_start,
                 "pyc.func", nullptr);
            sl.open = false;
        }
    }
    Py_RETURN_NONE;
}

static PyObject* on_py_resume(PyObject*, PyObject* const* a, Py_ssize_t n) {
    (void)a;
    if (g_active && g_fp && n >= 2 && !g_stack.empty()) {
        FuncSlice& sl = g_stack.back();
        if (sl.gen) {
            sl.open = true;
            sl.seg_start = now_ns();
        }
    }
    Py_RETURN_NONE;
}

static PyObject* on_line(PyObject*, PyObject* const* a, Py_ssize_t n) {
    if (g_active && g_fp && g_lines && n >= 2) {
        PyCodeObject* co = (PyCodeObject*)a[0];
        long line = PyLong_AsLong(a[1]);
        if (line == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            line = 0;
        }
        const char* f = co->co_filename ? PyUnicode_AsUTF8(co->co_filename) : nullptr;
        std::string nm;
        nm += f ? f : "?";
        nm += ":";
        nm += std::to_string(line);
        std::string args = "{\"file\":";
        jstr(args, f ? f : "");
        args += ",\"line\":";
        args += std::to_string(line);
        args += "}";
        emit(nm.c_str(), "i", cur_tid(), now_ns(), 0, "pyc.line", args.c_str());
    }
    Py_RETURN_NONE;
}

// --- bootstrap -----------------------------------------------------------

static PyMethodDef kMethods[] = {
    {"on_py_start", (PyCFunction)on_py_start, METH_FASTCALL, nullptr},
    {"on_py_return", (PyCFunction)on_py_return, METH_FASTCALL, nullptr},
    {"on_py_unwind", (PyCFunction)on_py_unwind, METH_FASTCALL, nullptr},
    {"on_call", (PyCFunction)on_call, METH_FASTCALL, nullptr},
    {"on_creturn", (PyCFunction)on_creturn, METH_FASTCALL, nullptr},
    {"on_craise", (PyCFunction)on_craise, METH_FASTCALL, nullptr},
    {"on_py_yield", (PyCFunction)on_py_yield, METH_FASTCALL, nullptr},
    {"on_py_resume", (PyCFunction)on_py_resume, METH_FASTCALL, nullptr},
    {"on_line", (PyCFunction)on_line, METH_FASTCALL, nullptr},
    {nullptr, nullptr, 0, nullptr}};

static struct PyModuleDef kModule = {PyModuleDef_HEAD_INIT,
                                     "__pyc_trace",
                                     "pyc --trace core",
                                     -1,
                                     kMethods};

void fail_deactivate(const char* why) {
    std::string detail;
    PyObject* typ = nullptr, *val = nullptr, *tb = nullptr;
    PyErr_Fetch(&typ, &val, &tb);
    if (val) {
        PyObject* s = PyObject_Str(val);
        if (s) {
            const char* c = PyUnicode_AsUTF8(s);
            if (c) detail = c;
            Py_DECREF(s);
        }
    }
    Py_XDECREF(typ);
    Py_XDECREF(val);
    Py_XDECREF(tb);
    std::fprintf(stderr, "pyc: trace: %s%s%s (tracing disabled, running on)\n", why,
                 detail.empty() ? "" : ": ", detail.c_str());
    g_active = false;
    if (g_fp) {
        std::fclose(g_fp);
        g_fp = nullptr;
    }
}

void bootstrap_monitoring() {
    // sys.monitoring is not importable by name (3.12+): it is reached as an
    // attribute of sys. PyImport_ImportModule("sys.monitoring") fails.
    PyObject* sysmod = PyImport_ImportModule("sys");
    if (!sysmod) {
        fail_deactivate("could not import sys");
        return;
    }
    PyObject* mon = PyObject_GetAttrString(sysmod, "monitoring");
    Py_DECREF(sysmod);
    if (!mon) {
        fail_deactivate("sys.monitoring is not available in the target interpreter");
        return;
    }
    PyObject* prof = PyObject_GetAttrString(mon, "PROFILER_ID");
    if (!prof) {
        Py_DECREF(mon);
        fail_deactivate("sys.monitoring.PROFILER_ID is not available");
        return;
    }
    // 3.14: the tool is a NAME (str), not an object.
    PyObject* tool_name = PyUnicode_InternFromString("pyc-trace");
    if (!tool_name) {
        Py_DECREF(prof);
        Py_DECREF(mon);
        fail_deactivate("could not create the tool name");
        return;
    }
    if (PyObject_CallMethod(mon, "use_tool_id", "OO", prof, tool_name) == nullptr) {
        Py_DECREF(tool_name);
        Py_DECREF(prof);
        Py_DECREF(mon);
        fail_deactivate("use_tool_id(PROFILER_ID) failed (is another tool using it?)");
        return;
    }
    Py_DECREF(tool_name);
    // Event constants come from the module, not the C header: on 3.14 the
    // runtime numbering (PY_START=1, PY_RETURN=4, CALL=16, LINE=32) diverges
    // from the C PyMonitorEvent enum (PY_START=0), and 0 is NO_EVENTS --
    // rejected by register_callback. Reading them at bootstrap is robust
    // across target versions.
    const char* kEvs[] = {"PY_START", "PY_RETURN", "PY_UNWIND", "CALL",
                          "C_RETURN", "C_RAISE", "PY_RESUME", "PY_YIELD",
                          "LINE"};
    PyObject* evns = PyObject_GetAttrString(mon, "events");
    if (!evns) {
        Py_DECREF(prof);
        Py_DECREF(mon);
        fail_deactivate("sys.monitoring.events is not available");
        return;
    }
    bool ok = true;
    PyObject* evobjs[9] = {nullptr};
    for (int i = 0; i < 9; ++i) {
        evobjs[i] = PyObject_GetAttrString(evns, kEvs[i]);
        if (!evobjs[i]) {
            PyErr_Clear();
            ok = false;
            break;
        }
    }
    PyObject* mask = nullptr;
    if (ok) {
        int n = g_lines ? 9 : 8;
        for (int i = 0; i < n; ++i) {
            PyObject* m2 = mask ? PyNumber_Or(mask, evobjs[i]) : evobjs[i];
            if (mask) Py_DECREF(mask);
            mask = m2;
            if (!mask) { ok = false; break; }
        }
    }
    if (ok && PyObject_CallMethod(mon, "set_events", "OO", prof, mask) == nullptr)
        ok = false;
    // One callback per event (3.14: register_callback takes exactly one
    // event, not a mask).
    const char* kFns[] = {"on_py_start", "on_py_return", "on_py_unwind",
                          "on_call", "on_creturn", "on_craise", "on_py_resume",
                          "on_py_yield", "on_line"};
    if (ok) {
        int n = g_lines ? 9 : 8;
        for (int i = 0; i < n; ++i) {
            PyObject* fn = PyObject_GetAttrString(g_tool, kFns[i]);
            if (!fn) { ok = false; break; }
            if (PyObject_CallMethod(mon, "register_callback", "OOO", prof,
                                    evobjs[i], fn) == nullptr) {
                Py_DECREF(fn);
                ok = false;
                break;
            }
            Py_DECREF(fn);
        }
    }
    for (int i = 0; i < 9; ++i) Py_XDECREF(evobjs[i]);
    Py_DECREF(evns);
    Py_XDECREF(mask);
    Py_DECREF(prof);
    Py_DECREF(mon);
    if (!ok) fail_deactivate("monitoring bootstrap failed (events/callbacks)");
}

}  // namespace

extern "C" {

void pyc_rt_trace_init(void) {
    const char* path = std::getenv("PYC_TRACE");
    if (!path || !*path || g_active || g_ended) return;
    g_fp = std::fopen(path, "w");
    if (!g_fp) {
        std::fprintf(stderr, "pyc: trace: cannot open '%s' for writing\n", path);
        return;
    }
    std::setvbuf(g_fp, nullptr, _IOFBF, 1 << 16);
    g_pid = (unsigned long)getpid();
    PyThreadState* ts = PyThreadState_Get();
    g_main_tid = ts ? ts->thread_id : 0;
    const char* lines = std::getenv("PYC_TRACE_LINES");
    g_lines = lines && (lines[0] == '1' || lines[0] == 'y' || lines[0] == 'Y');

    // Metadata first: names the UI shows in the header and the lane labels.
    const char* src = pyc_rt_source_file();
    std::string pn = "pyc program";
    if (src) {
        pn += " ";
        pn += src;
    }
    std::string args = "{\"name\":";
    jstr(args, pn.c_str());
    args += "}";
    emit(pn.c_str(), "M", 0, 0, 0, nullptr, args.c_str());
    emit_thread_name(g_main_tid);

    g_tool = PyModule_Create(&kModule);
    if (!g_tool) {
        fail_deactivate("could not create the in-process trace module");
        return;
    }
    g_active = true;
    std::atexit(pyc_rt_trace_end);
    bootstrap_monitoring();
}

void pyc_rt_trace_end(void) {
    if (!g_fp || g_ended) return;
    g_ended = true;
    g_active = false;
    // Close whatever is still open so the file is always balanced: the
    // module frame may return after this point, and a generator can die
    // suspended.
    while (!g_stack.empty()) {
        FuncSlice sl = std::move(g_stack.back());
        g_stack.pop_back();
        uint64_t ts = now_ns();
        if (sl.gen && sl.open)
            emit(sl.name.c_str(), "X", cur_tid(), sl.seg_start, ts - sl.seg_start,
                 "pyc.func", nullptr);
        else if (!sl.gen)
            emit(sl.name.c_str(), "E", cur_tid(), ts, 0, nullptr, nullptr);
    }
    while (!g_cstack.empty()) {
        CSlice c = std::move(g_cstack.back());
        g_cstack.pop_back();
        emit(c.name.c_str(), "X", cur_tid(), c.start, now_ns() - c.start,
             "pyc.ccall", nullptr);
    }
    std::fflush(g_fp);
    std::fclose(g_fp);
    g_fp = nullptr;
    if (g_tool && Py_IsInitialized()) {
        Py_DECREF(g_tool);
        g_tool = nullptr;
    }
}

}  // extern "C"
