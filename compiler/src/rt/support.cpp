#include "pyc/rt/support.hpp"

#include <marshal.h>      // PyMarshal_ReadObjectFromString
#include <frameobject.h>   // PyFrame_New, for synthesised tracebacks

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <new>

extern "C" {

// The module dict of __main__, resolved ONCE.
//
// This used to call PyImport_AddModule("__main__") on every global access --
// building a PyUnicode from the literal, searching sys.modules, and fetching
// the module dict, before the actual lookup even started -- on every read AND
// every write. It was the dominant cost of a global by a wide margin:
//
//     3M iterations      before     after      CPython
//     local  int +=      0.087s     0.091s     0.098s
//     global int +=      0.560s     0.134s     0.132s
//     attribute loop     0.346s     0.134s     0.129s
//
// __main__'s dict does not change for the lifetime of a compiled program, so
// it is resolved at startup and held. All eleven globals_dict() callers see
// the identical borrowed dict they saw before; only the cost changes.
//
// The lazy path stays as a fallback for anything that runs before
// pyc_rt_globals_init, because a borrowed reference to a module that has not
// been created yet is not something to guess at.
static PyObject* g_globals_cache = nullptr;
static std::vector<PyObject*> g_extra_consts;

extern "C" int pyc_rt_stash_marshal(const char* p, Py_ssize_t n) {
    PyObject* o = PyMarshal_ReadObjectFromString(const_cast<char*>(p), n);
    if (!o) return -1;
    g_extra_consts.push_back(o);
    return 0;
}

extern "C" void pyc_rt_globals_init(void) {
    PyObject* m = PyImport_AddModule("__main__");   // borrowed
    g_globals_cache = m ? PyModule_GetDict(m) : nullptr;   // borrowed
}

static PyObject* globals_dict() {
    PyObject* g = PyEval_GetGlobals();
    if (g) return g;
    if (g_globals_cache) return g_globals_cache;
    PyObject* m = PyImport_AddModule("__main__");   // borrowed
    return m ? PyModule_GetDict(m) : nullptr;       // borrowed
}

static PyObject* mapping_get(PyObject* map, PyObject* key) {
    if (!map || !key) return nullptr;
    if (PyDict_CheckExact(map)) {
        PyObject* v = nullptr;
        if (PyDict_GetItemRef(map, key, &v) < 0) return nullptr;
        return v;
    }
    PyObject* v = PyObject_GetItem(map, key);
    if (v) return v;
    if (PyErr_ExceptionMatches(PyExc_KeyError)) PyErr_Clear();
    return nullptr;
}

// Name lookup inside a CLASS BODY. CPython compiles these to LOAD_NAME:
// the class namespace first, then globals, then builtins. Going straight to
// the global path makes a class-level name invisible to anything else in the
// body -- `x = 7` then `def get(self, k, default=x)` raised NameError, because
// the default is evaluated in the class body, where x is a namespace entry and
// not a global.
PyObject* pyc_rt_load_classname(PyObject* ns, const char* name, PyObject* cell) {
    PyObject* key = PyUnicode_FromString(name);
    if (!key) return nullptr;
    PyObject* v = nullptr;
    if (PyDict_GetItemRef(ns, key, &v) < 0) { Py_DECREF(key); return nullptr; }
    Py_DECREF(key);
    if (v) return v;
    if (cell && PyCell_Check(cell)) {
        v = PyCell_GET(cell);
        if (v) return Py_NewRef(v);
        PyErr_Format(PyExc_NameError,
                     "cannot access free variable '%s' where it is not "
                     "associated with a value in enclosing scope",
                     name ? name : "?");
        return nullptr;
    }
    return pyc_rt_load_global(name);
}

// Global access with the name already built and INTERNED.
//
// pyc_rt_load_global below builds a fresh PyUnicode from the C string on every
// single access -- an allocation, a UTF-8 decode and a hash, per global read --
// and pyc_rt_store_global does the same through PyDict_SetItemString. The name
// is interned instead, so the dict lookup compares pointers and reuses a
// cached hash, which is what CPython's LOAD_GLOBAL relies on.
//
// Worth recording what this is and is not worth. Interning was the FIRST
// hypothesis for the 4.5x gap on globals and it was REFUTED: on its own it
// moved 3M global increments 0.560s -> 0.512s, nowhere near the 0.087s a local
// costs. The gap was the __main__ lookup above. Measured separately once that
// was cached, interning is a real but secondary win -- 0.148/0.176s without,
// 0.122/0.116s with -- which the larger cost had been hiding.
extern "C" PyObject* pyc_rt_load_global_obj(PyObject* name) {
    PyObject* g = globals_dict();
    if (!g) return nullptr;
    PyObject* v = mapping_get(g, name);
    if (v) return v;
    if (PyErr_Occurred()) return nullptr;
    // Not a module global: try builtins. This is what makes `print` an
    // ordinary name lookup rather than a special case in lowering (I3).
    PyObject* b = PyEval_GetBuiltins();             // borrowed
    v = mapping_get(b, name);
    if (v) return v;
    if (PyErr_Occurred()) return nullptr;
    PyErr_Format(PyExc_NameError, "name '%U' is not defined", name);
    return nullptr;
}

extern "C" int pyc_rt_store_global_obj(PyObject* name, PyObject* v) {
    PyObject* g = globals_dict();
    if (!g) return -1;
    return PyDict_SetItem(g, name, v);              // INCREFs v
}

// Build one interned name for the table. Interned so dict lookups compare
// pointers; kept separate from the literal table so literal identity semantics
// are untouched.
extern "C" PyObject* pyc_rt_intern(const char* name) {
    return PyUnicode_InternFromString(name);
}

PyObject* pyc_rt_load_global(const char* name) {
    PyObject* g = globals_dict();
    if (!g) return nullptr;
    PyObject* key = PyUnicode_FromString(name);
    if (!key) return nullptr;

    PyObject* v = mapping_get(g, key);
    if (v) { Py_DECREF(key); return v; }
    if (PyErr_Occurred()) { Py_DECREF(key); return nullptr; }

    // Not a module global: try builtins. This is what makes `print` an
    // ordinary name lookup rather than a special case in lowering (I3).
    PyObject* b = PyEval_GetBuiltins();             // borrowed
    v = mapping_get(b, key);
    if (v) { Py_DECREF(key); return v; }
    if (PyErr_Occurred()) { Py_DECREF(key); return nullptr; }

    PyErr_Format(PyExc_NameError, "name '%s' is not defined", name);
    Py_DECREF(key);
    return nullptr;
}

int pyc_rt_store_global(const char* name, PyObject* v) {
    PyObject* g = globals_dict();
    if (!g) return -1;
    return PyDict_SetItemString(g, name, v);        // INCREFs v
}

thread_local void* tls_module_frame = nullptr;
thread_local PyCodeObject* tls_module_code = nullptr;
static const char* g_source_file = "<pyc>";
static constexpr int kLineSlots = 8192;
static constexpr int kStubUnits = 8;
static constexpr uint8_t kNop = 27;

static void append_varint(std::vector<char>& o, unsigned val) {
    while (val >= 64) {
        o.push_back(static_cast<char>(64 | (val & 63)));
        val >>= 6;
    }
    o.push_back(static_cast<char>(val));
}
static void append_svarint(std::vector<char>& o, int val) {
    unsigned uval = val < 0 ? (((0u - (unsigned)val) << 1) | 1u)
                            : ((unsigned)val << 1);
    append_varint(o, uval);
}

static bool build_linemap(PyObject** bytecode, PyObject** linetable,
                          const int* locs = nullptr, int nlocs = 0,
                          int firstlineno = 1, int helper_idx = 2) {
    char kEvalCode[] = {
        '\x80', '\x00',
        'R', '\x02',
        '\x21', '\x00',
        '\x34', '\x00',
        '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
        '#', '\x00'
    };
    kEvalCode[3] = static_cast<char>(helper_idx);
    if (!locs || nlocs <= 0) {
        std::vector<char> codebuf(kLineSlots * 2, 0);
        std::memcpy(codebuf.data(), kEvalCode, 16);
        for (int i = 16; i + 1 < kLineSlots * 2; i += 2)
            codebuf[static_cast<std::size_t>(i)] = static_cast<char>(kNop);
        std::vector<char> lines;
        lines.reserve(static_cast<std::size_t>(kLineSlots) * 3);
        lines.push_back(static_cast<char>(128 | (10 << 3)));
        lines.push_back(0);
        lines.push_back(0);
        for (int i = 1; i < kLineSlots; ++i) {
            lines.push_back(static_cast<char>(128 | (11 << 3)));
            lines.push_back(0);
            lines.push_back(0);
        }
        *bytecode = PyBytes_FromStringAndSize(codebuf.data(), (Py_ssize_t)codebuf.size());
        *linetable = PyBytes_FromStringAndSize(lines.data(), (Py_ssize_t)lines.size());
        return *bytecode && *linetable;
    }
    int nunits = kStubUnits + nlocs;
    std::vector<char> codebuf(nunits * 2, 0);
    std::memcpy(codebuf.data(), kEvalCode, 16);
    for (int i = 16; i + 1 < nunits * 2; i += 2)
        codebuf[static_cast<std::size_t>(i)] = static_cast<char>(kNop);
    std::vector<char> lines;
    lines.push_back(static_cast<char>(128 | (15 << 3) | 7));
    int prev = firstlineno > 0 ? firstlineno : 1;
    for (int i = 0; i < nlocs; ++i) {
        int line = locs[i * 3];
        int col = locs[i * 3 + 1];
        int end_col = locs[i * 3 + 2];
        if (line < 1) line = prev;
        if (col < 0) col = 0;
        if (end_col < col) end_col = col;
        lines.push_back(static_cast<char>(128 | (14 << 3)));
        append_svarint(lines, line - prev);
        append_varint(lines, 0);
        append_varint(lines, (unsigned)col + 1);
        append_varint(lines, (unsigned)end_col + 1);
        prev = line;
    }
    *bytecode = PyBytes_FromStringAndSize(codebuf.data(), (Py_ssize_t)codebuf.size());
    *linetable = PyBytes_FromStringAndSize(lines.data(), (Py_ssize_t)lines.size());
    return *bytecode && *linetable;
}

const char* pyc_rt_source_file(void) { return g_source_file; }

void pyc_rt_set_source_file(const char* file) {
    if (file && file[0]) g_source_file = file;
    if (tls_module_code) {
        PyObject* u = PyUnicode_FromString(g_source_file);
        if (u) {
            Py_XSETREF(tls_module_code->co_filename, u);
        } else {
            PyErr_Clear();
        }
    }
}

int pyc_rt_push_module_frame(void) {
    PyObject* g = globals_dict();
    if (!g) return -1;
    PyObject *bytecode = nullptr, *linetable = nullptr;
    if (!build_linemap(&bytecode, &linetable)) {
        Py_XDECREF(bytecode); Py_XDECREF(linetable); return -1;
    }
    PyObject* empty_bytes = PyBytes_FromStringAndSize("", 0);
    PyObject* empty_tuple = PyTuple_New(0);
    PyObject* filename = PyUnicode_FromString(g_source_file);
    PyObject* name = PyUnicode_FromString("<module>");
    PyObject* consts = PyTuple_Pack(1, Py_None);
    if (!empty_bytes || !empty_tuple || !filename || !name || !consts) {
        Py_XDECREF(bytecode); Py_XDECREF(linetable); Py_XDECREF(empty_bytes);
        Py_XDECREF(empty_tuple); Py_XDECREF(filename); Py_XDECREF(name);
        Py_XDECREF(consts);
        return -1;
    }
    PyCodeObject* co = PyUnstable_Code_NewWithPosOnlyArgs(
        0, 0, 0, 0, 1, 0,
        bytecode, consts, empty_tuple, empty_tuple,
        empty_tuple, empty_tuple, filename, name, name, 1,
        linetable, empty_bytes);
    Py_DECREF(bytecode); Py_DECREF(linetable); Py_DECREF(empty_bytes);
    Py_DECREF(empty_tuple); Py_DECREF(filename); Py_DECREF(name); Py_DECREF(consts);
    if (!co) return -1;
    void* f = pyc_rt_interp_enter(co, g, g, nullptr);
    if (!f) { Py_DECREF(co); return -1; }
    tls_module_frame = f;
    tls_module_code = co;
    return 0;
}

void pyc_rt_pop_module_frame(void) {
    pyc_rt_interp_leave(tls_module_frame);
    Py_XDECREF(tls_module_code);
    tls_module_frame = nullptr;
    tls_module_code = nullptr;
}

// Refcounting, as something the optimiser can SEE.
//
// codegen used to emit calls to Py_IncRef/Py_DecRef, which are the out-of-line
// entry points in libpython -- CPython's own code never uses them, it uses the
// Py_INCREF/Py_DECREF macros. An opaque call per reference is not just its own
// cost: it is a barrier the optimiser cannot reason across, so nothing else in
// the loop can be moved, folded, or kept in a register either. Measured on the
// same objects and the same C-API calls, out-of-line vs inlined:
//
//     36.7 ns/iter  ->  11.3 ns/iter        (3.3x, no unboxing involved)
//
// These wrappers expand the target header's macros, so immortal objects and
// the free-threaded build's atomics are handled by CPython's own definition
// for the version being targeted rather than by an ABI guess here. They are
// inlined into the generated module by LTO (see pycc); without LTO they are
// correct, just no faster than what they replaced.
//
// X variants deliberately: codegen relies on the null tolerance Py_DecRef had.
static thread_local PyThreadState* pyc_gil_saved = nullptr;
static thread_local int pyc_gil_free_depth = 0;

static void pyc_gil_ensure(void) {
    if (pyc_gil_saved) {
        PyEval_RestoreThread(pyc_gil_saved);
        pyc_gil_saved = nullptr;
    }
}

void pyc_rt_incref(PyObject* o) { pyc_gil_ensure(); Py_XINCREF(o); }
void pyc_rt_decref(PyObject* o) { pyc_gil_ensure(); Py_XDECREF(o); }

extern "C" int pyc_rt_gil_release(void) {
    if (!pyc_gil_saved) pyc_gil_saved = PyEval_SaveThread();
    pyc_gil_free_depth++;
    return 0;
}

extern "C" int pyc_rt_gil_acquire(void) {
    if (pyc_gil_free_depth > 0) pyc_gil_free_depth--;
    if (pyc_gil_free_depth == 0) pyc_gil_ensure();
    return 0;
}

thread_local PyObject* tls_frame_locals = nullptr;
thread_local const char* const* tls_frame_names = nullptr;
thread_local int tls_frame_nnames = 0;

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

// `del x` on a local. CPython's DELETE_FAST raises UnboundLocalError when the
// slot is already empty rather than silently succeeding, and a later read of
// the slot must raise too -- which pyc_rt_load_local already does, since it
// treats NULL as unbound. So deleting is clearing the slot, not storing None:
// storing None would make the name still bound, to the wrong value.
int pyc_rt_del_local(PyObject** locals, int slot, const char* name) {
    PyObject* old = locals[slot];
    if (!old) {
        PyErr_Format(PyExc_UnboundLocalError,
                     "cannot access local variable '%s' where it is not "
                     "associated with a value", name);
        return -1;
    }
    locals[slot] = nullptr;       // clear BEFORE the decref: __del__ may run
    Py_DECREF(old);               // and must not observe a dangling slot
    if (tls_frame_locals && slot >= 0 && slot < tls_frame_nnames
        && tls_frame_names && tls_frame_names[slot]
        && PyDict_DelItemString(tls_frame_locals, tls_frame_names[slot]) < 0)
        PyErr_Clear();
    return 0;
}

void pyc_rt_store_local(PyObject** locals, int slot, PyObject* v) {
    PyObject* old = locals[slot];
    Py_XINCREF(v);
    locals[slot] = v;
    Py_XDECREF(old);          // after the store: a self-assignment must not free
    if (tls_frame_locals && slot >= 0 && slot < tls_frame_nnames
        && tls_frame_names && tls_frame_names[slot]) {
        PyObject* shown = v;
        if (v && PyCell_Check(v)) shown = PyCell_GET(v);
        if (shown) PyDict_SetItemString(tls_frame_locals, tls_frame_names[slot], shown);
        else if (PyDict_DelItemString(tls_frame_locals, tls_frame_names[slot]) < 0)
            PyErr_Clear();
    }
}

// --- callables -------------------------------------------------------------

namespace {
// Each function owns its PyMethodDef so ml_name carries its real name.
// Setting __name__ afterwards does not work: it is read-only on a
// builtin_function_or_method, and the failed SetAttr left an exception set
// that surfaced later as an unrelated SystemError.
// nargs counts POSITIONALLY-BINDABLE parameters. Keyword-only parameters
// occupy the next nkwonly slots: ordinary slots that positional binding may not
// reach, which is exactly what makes them keyword-only. kwdefaults is a dict
// keyed by name rather than a tuple, because a REQUIRED keyword-only parameter
// is a hole and a tuple cannot hold one.
struct Bound { PycImpl impl; int nargs; int nkwonly; int nposonly; int nlocals;
               char* name; const char* const* argnames;
               int vararg; int kwarg; int nfree; int firstlineno;
               const int* locs; int nlocs; };

constexpr int kMoveCost = 2;
constexpr int kCaseCost = 1;
constexpr size_t kMaxSuggest = 40;

int subst_cost(char a, char b) {
    if ((a & 31) != (b & 31)) return kMoveCost;
    if (a == b) return 0;
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
    return a == b ? kCaseCost : kMoveCost;
}

Py_ssize_t edit_distance(const char* a, size_t na, const char* b, size_t nb,
                         size_t max_cost, size_t* buf) {
    while (na && nb && a[0] == b[0]) { a++; na--; b++; nb--; }
    while (na && nb && a[na - 1] == b[nb - 1]) { na--; nb--; }
    if (na == 0 || nb == 0) return static_cast<Py_ssize_t>((na + nb) * kMoveCost);
    if (na > kMaxSuggest || nb > kMaxSuggest) return static_cast<Py_ssize_t>(max_cost + 1);
    if (nb < na) { const char* t = a; a = b; b = t; size_t tn = na; na = nb; nb = tn; }
    if ((nb - na) * kMoveCost > max_cost) return static_cast<Py_ssize_t>(max_cost + 1);
    size_t tmp = kMoveCost;
    for (size_t i = 0; i < na; ++i) { buf[i] = tmp; tmp += kMoveCost; }
    size_t result = 0;
    for (size_t bi = 0; bi < nb; ++bi) {
        char code = b[bi];
        size_t distance = result = bi * kMoveCost;
        size_t minimum = static_cast<size_t>(-1);
        for (size_t i = 0; i < na; ++i) {
            size_t substitute = distance + static_cast<size_t>(subst_cost(code, a[i]));
            distance = buf[i];
            size_t insdel = (result < distance ? result : distance) + kMoveCost;
            result = insdel < substitute ? insdel : substitute;
            buf[i] = result;
            if (result < minimum) minimum = result;
        }
        if (minimum > max_cost) return static_cast<Py_ssize_t>(max_cost + 1);
    }
    return static_cast<Py_ssize_t>(result);
}

PyObject* keyword_suggestion(PyObject* dir, PyObject* name) {
    if (!dir || !name || !PyList_CheckExact(dir)) return nullptr;
    Py_ssize_t n = PyList_GET_SIZE(dir);
    if (n <= 0 || n >= 750) return nullptr;
    Py_ssize_t nsz = 0;
    const char* ns = PyUnicode_AsUTF8AndSize(name, &nsz);
    if (!ns) { PyErr_Clear(); return nullptr; }
    size_t buf[kMaxSuggest];
    Py_ssize_t best_d = PY_SSIZE_T_MAX;
    PyObject* best = nullptr;
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* item = PyList_GET_ITEM(dir, i);
        if (PyUnicode_Check(item) && PyUnicode_Compare(name, item) == 0) continue;
        Py_ssize_t isz = 0;
        const char* is = PyUnicode_AsUTF8AndSize(item, &isz);
        if (!is) { PyErr_Clear(); continue; }
        Py_ssize_t max_d = (nsz + isz + 3) * kMoveCost / 6;
        if (best_d != PY_SSIZE_T_MAX && max_d > best_d - 1) max_d = best_d - 1;
        Py_ssize_t d = edit_distance(ns, static_cast<size_t>(nsz), is,
                                     static_cast<size_t>(isz),
                                     static_cast<size_t>(max_d), buf);
        if (d > max_d) continue;
        if (!best || d < best_d) { best = item; best_d = d; }
    }
    if (best) Py_INCREF(best);
    return best;
}

void bound_capsule_dtor(PyObject* cap) {
    Bound* b = static_cast<Bound*>(PyCapsule_GetPointer(cap, "pyc.Bound"));
    if (!b) { PyErr_Clear(); return; }
    std::free(b->name);
    delete b;
}

Bound* bound_from_code(PyObject* code) {
    if (!code || !PyCode_Check(code)) return nullptr;
    auto* co = reinterpret_cast<PyCodeObject*>(code);
    if (!co->co_consts) return nullptr;
    Py_ssize_t n = PyTuple_GET_SIZE(co->co_consts);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* x = PyTuple_GET_ITEM(co->co_consts, i);
        if (PyCapsule_IsValid(x, "pyc.Bound"))
            return static_cast<Bound*>(PyCapsule_GetPointer(x, "pyc.Bound"));
    }
    return nullptr;
}

Bound* bound_from_func(PyObject* func) {
    if (!func || !PyFunction_Check(func)) return nullptr;
    return bound_from_code(PyFunction_GET_CODE(func));
}

PyCodeObject* make_func_code(Bound* b) {
    int nfree = b->nfree > 0 ? b->nfree : 0;
    int nfast = b->nlocals - nfree;
    if (nfast < 0) nfast = 0;
    PyObject* varnames = PyTuple_New(nfast);
    if (!varnames) { std::free(b->name); delete b; return nullptr; }
    for (int i = 0; i < nfast; ++i) {
        const char* s = (b->argnames && i < b->nlocals && b->argnames[i])
                            ? b->argnames[i] : "";
        PyObject* u = PyUnicode_FromString(s);
        if (!u) { Py_DECREF(varnames); std::free(b->name); delete b; return nullptr; }
        PyTuple_SET_ITEM(varnames, i, u);
    }
    PyObject* freevars = PyTuple_New(nfree);
    if (!freevars) { Py_DECREF(varnames); std::free(b->name); delete b; return nullptr; }
    for (int i = 0; i < nfree; ++i) {
        int slot = nfast + i;
        const char* s = (b->argnames && slot < b->nlocals && b->argnames[slot])
                            ? b->argnames[slot] : "";
        PyObject* u = PyUnicode_FromString(s);
        if (!u) { Py_DECREF(varnames); Py_DECREF(freevars); std::free(b->name); delete b; return nullptr; }
        PyTuple_SET_ITEM(freevars, i, u);
    }
    PyObject* cap = PyCapsule_New(b, "pyc.Bound", bound_capsule_dtor);
    if (!cap) { Py_DECREF(varnames); Py_DECREF(freevars); std::free(b->name); delete b; return nullptr; }
    static PyMethodDef run_md = {"__pyc_eval__", pyc_rt_run_from_frame, METH_NOARGS, nullptr};
    static PyObject* run_helper = nullptr;
    if (!run_helper) {
        run_helper = PyCFunction_New(&run_md, nullptr);
        if (!run_helper) {
            Py_DECREF(cap); Py_DECREF(varnames); Py_DECREF(freevars); return nullptr;
        }
    }
    // Nested marshalled code objects first (CPython puts nested codes at
    // the front of co_consts), then None, Bound capsule, eval helper.
    const int nextra = (int)g_extra_consts.size();
    const int helper_idx = nextra + 2;
    PyObject* consts = PyTuple_New(nextra + 3);
    if (consts) {
        for (int i = 0; i < nextra; ++i)
            PyTuple_SET_ITEM(consts, i, g_extra_consts[static_cast<size_t>(i)]);
        g_extra_consts.clear();
        Py_INCREF(Py_None); PyTuple_SET_ITEM(consts, nextra, Py_None);
        Py_INCREF(cap); PyTuple_SET_ITEM(consts, nextra + 1, cap);
        Py_INCREF(run_helper); PyTuple_SET_ITEM(consts, nextra + 2, run_helper);
    }
    Py_DECREF(cap);
    if (!consts) { Py_DECREF(varnames); Py_DECREF(freevars); return nullptr; }
    PyObject *bytecode = nullptr, *linetable = nullptr;
    if (!build_linemap(&bytecode, &linetable, b->locs, b->nlocs, b->firstlineno,
                       helper_idx)) {
        Py_XDECREF(bytecode); Py_XDECREF(linetable);
        Py_DECREF(varnames); Py_DECREF(freevars); Py_DECREF(consts);
        return nullptr;
    }
    PyObject* empty_bytes = PyBytes_FromStringAndSize("", 0);
    PyObject* empty_tuple = PyTuple_New(0);
    PyObject* filename = PyUnicode_FromString(g_source_file);
    const char* full = b->name ? b->name : "<fn>";
    const char* shortn = full;
    if (const char* dot = std::strrchr(full, '.')) shortn = dot + 1;
    PyObject* name = PyUnicode_FromString(shortn);
    PyObject* qual = PyUnicode_FromString(full);
    if (!bytecode || !linetable || !empty_bytes || !empty_tuple || !filename || !name || !qual) {
        Py_XDECREF(bytecode); Py_XDECREF(linetable); Py_XDECREF(empty_bytes);
        Py_XDECREF(empty_tuple); Py_XDECREF(filename); Py_XDECREF(name); Py_XDECREF(qual);
        Py_DECREF(varnames); Py_DECREF(freevars); Py_DECREF(consts);
        return nullptr;
    }
    int flags = CO_NEWLOCALS;
    if (b->vararg >= 0) flags |= CO_VARARGS;
    if (b->kwarg >= 0) flags |= CO_VARKEYWORDS;
    PyCodeObject* co = PyUnstable_Code_NewWithPosOnlyArgs(
        b->nargs, b->nposonly, b->nkwonly, nfast, 2, flags,
        bytecode, consts, empty_tuple, varnames,
        freevars, empty_tuple, filename, name, qual,
        b->firstlineno > 0 ? b->firstlineno : 1,
        linetable, empty_bytes);
    Py_DECREF(bytecode); Py_DECREF(linetable); Py_DECREF(empty_bytes);
    Py_DECREF(empty_tuple);
    Py_DECREF(filename); Py_DECREF(name); Py_DECREF(qual);
    Py_DECREF(varnames); Py_DECREF(freevars); Py_DECREF(consts);
    return co;
}

PyObject* trampoline(PyObject* func, PyObject* args, PyObject* kwargs) {
    Bound* b = bound_from_func(func);
    if (!b) return nullptr;
    PyObject* defaults = PyFunction_GET_DEFAULTS(func);
    PyObject* kwdefaults = PyFunction_GET_KW_DEFAULTS(func);
    PyObject* closure = PyFunction_GET_CLOSURE(func);
    Py_ssize_t npos = PyTuple_GET_SIZE(args);
    const char* nm = b->name ? b->name : "<fn>";
    if (func) {
        PyObject* nobj = reinterpret_cast<PyFunctionObject*>(func)->func_qualname;
        if (nobj && PyUnicode_Check(nobj)) {
            const char* s = PyUnicode_AsUTF8(nobj);
            if (s) nm = s;
        }
    }
    if (npos > b->nargs && b->vararg < 0) {
        // CPython names the whole accepted RANGE when defaults make the lower
        // bound differ: "takes from 1 to 2 positional arguments".
        Py_ssize_t ndef0 = defaults ? PyTuple_GET_SIZE(defaults) : 0;
        int nkwonly_given = 0;
        if (kwargs && b->nkwonly > 0 && b->argnames) {
            for (int i = b->nargs; i < b->nargs + b->nkwonly; ++i) {
                if (b->argnames[i] && PyDict_GetItemString(kwargs, b->argnames[i]))
                    nkwonly_given++;
            }
        }
        if (nkwonly_given > 0 && ndef0 == 0)
            PyErr_Format(PyExc_TypeError,
                         "%s() takes %d positional argument%s but %zd positional "
                         "argument%s (and %d keyword-only argument%s) were given",
                         nm, b->nargs, b->nargs == 1 ? "" : "s", npos,
                         npos == 1 ? "" : "s", nkwonly_given,
                         nkwonly_given == 1 ? "" : "s");
        else if (ndef0 > 0)
            PyErr_Format(PyExc_TypeError,
                         "%s() takes from %zd to %d positional argument%s "
                         "but %zd %s given",
                         nm, (Py_ssize_t)b->nargs - ndef0, b->nargs,
                         b->nargs == 1 ? "" : "s", npos,
                         npos == 1 ? "was" : "were");
        else
            PyErr_Format(PyExc_TypeError,
                         "%s() takes %d positional argument%s but %zd %s given",
                         nm, b->nargs, b->nargs == 1 ? "" : "s", npos,
                         npos == 1 ? "was" : "were");
        return nullptr;
    }
    std::vector<const char*> missing;
    std::vector<const char*> missing_kwonly;
    // Positional-only names that arrived as keywords. Collected rather than
    // reported on sight, because CPython names them all in one message:
    // "got some positional-only arguments passed as keyword arguments: 'a, b'".
    std::vector<const char*> posonly_kw;
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
        PyObject* cell = PyTuple_GET_ITEM(closure, i);
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
            // Starts at nposonly: a positional-only parameter is NOT bindable
            // by keyword, which is the whole point of the `/` marker. Ends at
            // nargs + nkwonly because a keyword-only parameter is bindable by
            // keyword, which is the whole point of `*`.
            for (int i = b->nposonly; i < b->nargs + b->nkwonly; ++i)
                if (std::strcmp(ks, b->argnames[i]) == 0) { slot = i; break; }
            if (slot < 0) {
                // Unmatched keywords go to **kwargs when the function has one.
                // This is checked BEFORE the positional-only diagnosis, and
                // the order is observable: `def g(a, /, **kw)` called as
                // `g(1, a=2)` binds a=1 positionally and puts {'a': 2} in kw,
                // rather than complaining that `a` was passed by keyword.
                if (b->kwarg >= 0) {
                    if (PyDict_SetItem(locals[b->kwarg], k, val) < 0) goto fail;
                    continue;
                }
                bool posonly_name = false;
                for (int i = 0; i < b->nposonly; ++i)
                    if (std::strcmp(ks, b->argnames[i]) == 0) { posonly_name = true; break; }
                if (posonly_name) { posonly_kw.push_back(ks); continue; }
                {
                    const char* sug = nullptr;
                    if (b->argnames) {
                        PyObject* cands = PyList_New(0);
                        if (cands) {
                            for (int i = b->nposonly; i < b->nargs + b->nkwonly; ++i) {
                                if (!b->argnames[i] || !b->argnames[i][0]) continue;
                                PyObject* u = PyUnicode_FromString(b->argnames[i]);
                                if (!u || PyList_Append(cands, u) < 0) { Py_XDECREF(u); break; }
                                Py_DECREF(u);
                            }
                            PyObject* kn = PyUnicode_FromString(ks);
                            if (kn) {
                                PyObject* s = keyword_suggestion(cands, kn);
                                if (s) {
                                    sug = PyUnicode_AsUTF8(s);
                                    PyErr_Format(PyExc_TypeError,
                                        "%s() got an unexpected keyword argument '%s'. Did you mean '%s'?",
                                        nm, ks, sug ? sug : "");
                                    Py_DECREF(s);
                                    Py_DECREF(kn); Py_DECREF(cands);
                                    goto fail;
                                }
                                Py_DECREF(kn);
                            }
                            Py_DECREF(cands);
                        }
                    }
                    PyErr_Format(PyExc_TypeError,
                                 "%s() got an unexpected keyword argument '%s'",
                                 nm, ks);
                }
                goto fail;
            }
            if (locals[slot]) {
                PyErr_Format(PyExc_TypeError,
                             "%s() got multiple values for argument '%s'",
                             nm, ks);
                goto fail;
            }
            Py_INCREF(val);
            locals[slot] = val;
        }
    }
    if (!posonly_kw.empty()) {
        // Reported before missing-argument analysis: CPython complains about
        // the keyword misuse itself, not about the positional slot it left
        // unfilled.
        std::string names;
        for (std::size_t i = 0; i < posonly_kw.size(); ++i) {
            if (i) names += ", ";
            names += posonly_kw[i];
        }
        PyErr_Format(PyExc_TypeError,
                     "%s() got some positional-only arguments passed as "
                     "keyword arguments: '%s'", nm, names.c_str());
        goto fail;
    }
    {
        // Defaults cover the LAST k parameters, so parameter i takes
        // defaults[i - (nargs - k)].
        Py_ssize_t ndef = defaults ? PyTuple_GET_SIZE(defaults) : 0;
        Py_ssize_t first_def = b->nargs - ndef;
        for (int i = 0; i < b->nargs; ++i) {
            if (locals[i]) continue;
            if (ndef && i >= first_def) {
                PyObject* d = PyTuple_GET_ITEM(defaults, i - first_def);
                Py_INCREF(d);
                locals[i] = d;
                continue;
            }
            missing.push_back(b->argnames[i]);
        }
        // Keyword-only parameters are reported SEPARATELY by CPython:
        // "missing 1 required keyword-only argument: 'b'". Collected apart so
        // the message says which kind, rather than lumping them in with
        // positional and being wrong about it.
        for (int i = b->nargs; i < b->nargs + b->nkwonly; ++i) {
            if (locals[i]) continue;
            PyObject* d = kwdefaults
                ? PyDict_GetItemString(kwdefaults, b->argnames[i]) : nullptr;
            if (d) { Py_INCREF(d); locals[i] = d; continue; }
            missing_kwonly.push_back(b->argnames[i]);
        }
        // CPython reports ALL missing parameters in one message, counted and
        // joined: "missing 2 required positional arguments: 'a' and 'b'"; three
        // or more use an Oxford comma. Positional and keyword-only get separate
        // messages, and positional is reported first when both are missing.
        auto join = [](const std::vector<const char*>& v) {
            std::string names;
            for (std::size_t k2 = 0; k2 < v.size(); ++k2) {
                if (k2) names += (v.size() == 2) ? " and "
                               : (k2 + 1 == v.size() ? ", and " : ", ");
                names += "'"; names += v[k2]; names += "'";
            }
            return names;
        };
        if (!missing.empty()) {
            std::string names = join(missing);
            PyErr_Format(PyExc_TypeError,
                         "%s() missing %zd required positional argument%s: %s",
                         nm, (Py_ssize_t)missing.size(),
                         missing.size() == 1 ? "" : "s", names.c_str());
            goto fail;
        }
        if (!missing_kwonly.empty()) {
            std::string names = join(missing_kwonly);
            PyErr_Format(PyExc_TypeError,
                         "%s() missing %zd required keyword-only argument%s: %s",
                         nm, (Py_ssize_t)missing_kwonly.size(),
                         missing_kwonly.size() == 1 ? "" : "s", names.c_str());
            goto fail;
        }
    }
    {
        // C1a: a Python frame so locals()/globals()/eval see this call.
        PyObject* fdict = PyDict_New();
        if (!fdict) goto fail;
        if (b->argnames) {
            for (int i = 0; i < b->nlocals; ++i) {
                if (!locals[i] || !b->argnames[i] || !b->argnames[i][0]) continue;
                PyObject* v = locals[i];
                if (PyCell_Check(v)) {
                    v = PyCell_GET(v);
                    if (!v) continue;
                }
                if (PyDict_SetItemString(fdict, b->argnames[i], v) < 0) {
                    Py_DECREF(fdict); goto fail;
                }
            }
        }
        PyObject* g = PyFunction_GET_GLOBALS(func);
        if (!g) g = globals_dict();
        auto* co = reinterpret_cast<PyCodeObject*>(PyFunction_GET_CODE(func));
        void* fr = (g && co) ? pyc_rt_interp_enter(co, g, fdict, func)
                             : nullptr;
        if (!fr) { Py_DECREF(fdict); goto fail; }
        pyc_rt_interp_fill_locals(fr, locals, b->nlocals);
        PyObject* prev_tls = tls_frame_locals;
        const char* const* prev_names = tls_frame_names;
        int prev_n = tls_frame_nnames;
        tls_frame_locals = fdict;
        tls_frame_names = b->argnames;
        tls_frame_nnames = b->nlocals;
        if (Py_EnterRecursiveCall("")) {
            tls_frame_locals = prev_tls;
            tls_frame_names = prev_names;
            tls_frame_nnames = prev_n;
            pyc_rt_interp_leave(fr);
            Py_DECREF(fdict);
            goto fail;
        }
        PyObject* r = b->impl(locals);
        Py_LeaveRecursiveCall();
        tls_frame_locals = prev_tls;
        tls_frame_names = prev_names;
        tls_frame_nnames = prev_n;
        pyc_rt_interp_leave(fr);
        Py_DECREF(fdict);
        for (int i = 0; i < b->nlocals; ++i) Py_XDECREF(locals[i]);
        delete[] locals;
        return r;
    }
fail:
    for (int i = 0; i < b->nlocals; ++i) Py_XDECREF(locals[i]);
    delete[] locals;
    return nullptr;
}

PyObject* func_vectorcall(PyObject* callable, PyObject* const* args,
                          size_t nargsf, PyObject* kwnames) {
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    PyObject* tup = PyTuple_New(nargs);
    if (!tup) return nullptr;
    for (Py_ssize_t i = 0; i < nargs; ++i) {
        PyObject* a = args[i];
        Py_INCREF(a);
        PyTuple_SET_ITEM(tup, i, a);
    }
    PyObject* kw = nullptr;
    if (kwnames) {
        Py_ssize_t nkw = PyTuple_GET_SIZE(kwnames);
        kw = PyDict_New();
        if (!kw) { Py_DECREF(tup); return nullptr; }
        for (Py_ssize_t i = 0; i < nkw; ++i) {
            if (PyDict_SetItem(kw, PyTuple_GET_ITEM(kwnames, i), args[nargs + i]) < 0) {
                Py_DECREF(kw); Py_DECREF(tup); return nullptr;
            }
        }
    }
    PyObject* r = trampoline(callable, tup, kw);
    Py_DECREF(tup);
    Py_XDECREF(kw);
    return r;
}

int pyc_func_watch(PyFunction_WatchEvent ev, PyFunctionObject* func, PyObject* new_value) {
    if (ev == PyFunction_EVENT_CREATE) {
        if (bound_from_func(reinterpret_cast<PyObject*>(func)))
            PyFunction_SetVectorcall(func, func_vectorcall);
    } else if (ev == PyFunction_EVENT_MODIFY_CODE && new_value) {
        if (bound_from_code(new_value))
            PyFunction_SetVectorcall(func, func_vectorcall);
    }
    return 0;
}

void ensure_func_watch() {
    static bool done = false;
    if (done) return;
    done = true;
    PyFunction_AddWatcher(pyc_func_watch);
}

}  // namespace

PyObject* pyc_rt_invoke_code(PyObject* code, PyObject** locals) {
    Bound* b = bound_from_code(code);
    if (!b) {
        PyErr_SetString(PyExc_SystemError, "pyc eval of non-pyc code");
        return nullptr;
    }
    if (Py_EnterRecursiveCall("")) return nullptr;
    PyObject* r = b->impl(locals);
    Py_LeaveRecursiveCall();
    return r;
}

PyObject* pyc_rt_make_function(const char* name, PycImpl impl,
                               int nargs, int nkwonly, int nposonly, int nlocals,
                               const char* const* argnames,
                               PyObject* defaults, PyObject* kwdefaults,
                               int vararg_slot, int kwarg_slot,
                               PyObject** closure, int nfree, int firstlineno,
                               const int* locs, int nlocs) {
    ensure_func_watch();
    char* owned = strdup(name);
    if (!owned) return PyErr_NoMemory();
    Bound* b = new (std::nothrow) Bound{impl, nargs, nkwonly, nposonly, nlocals, owned,
                                        argnames, vararg_slot, kwarg_slot, nfree,
                                        firstlineno > 0 ? firstlineno : 1,
                                        locs, nlocs};
    if (!b) { std::free(owned); return PyErr_NoMemory(); }
    PyCodeObject* co = make_func_code(b);
    if (!co) return nullptr;
    PyObject* g = globals_dict();
    if (!g) { Py_DECREF(co); PyErr_SetString(PyExc_RuntimeError, "no globals"); return nullptr; }
    PyObject* qn = PyUnicode_FromString(name);
    if (!qn) { Py_DECREF(co); return nullptr; }
    PyObject* fn = PyFunction_NewWithQualName(reinterpret_cast<PyObject*>(co), g, qn);
    Py_DECREF(co);
    Py_DECREF(qn);
    if (!fn) return nullptr;
    if (defaults && PyFunction_SetDefaults(fn, defaults) < 0) { Py_DECREF(fn); return nullptr; }
    if (kwdefaults && PyFunction_SetKwDefaults(fn, kwdefaults) < 0) { Py_DECREF(fn); return nullptr; }
    if (nfree > 0) {
        PyObject* clo = PyTuple_New(nfree);
        if (!clo) { Py_DECREF(fn); return nullptr; }
        for (int i = 0; i < nfree; ++i) {
            Py_INCREF(closure[i]);
            PyTuple_SET_ITEM(clo, i, closure[i]);
        }
        int rc = PyFunction_SetClosure(fn, clo);
        Py_DECREF(clo);
        if (rc < 0) { Py_DECREF(fn); return nullptr; }
    }
    PyFunction_SetVectorcall(reinterpret_cast<PyFunctionObject*>(fn), func_vectorcall);
    return fn;
}

extern "C" int pyc_rt_unbox_int(PyObject* o, int64_t* out) {
    pyc_gil_ensure();
    if (!o || !PyLong_CheckExact(o) || !out) return 0;
    int ovf = 0;
    long long v = PyLong_AsLongLongAndOverflow(o, &ovf);
    if (ovf || PyErr_Occurred()) {
        PyErr_Clear();
        return 0;
    }
    *out = (int64_t)v;
    return 1;
}

extern "C" int pyc_rt_range_native(PyObject* callee, PyObject* a0, PyObject* a1,
                                   PyObject* a2, int nargs,
                                   int64_t* start, int64_t* stop, int64_t* step) {
    PyObject* b = PyEval_GetBuiltins();
    if (!b || !callee) return 0;
    PyObject* rng = PyDict_GetItemString(b, "range");
    if (!rng || callee != rng) return 0;
    int64_t s = 0, e = 0, p = 1;
    if (nargs == 1) {
        if (!pyc_rt_unbox_int(a0, &e)) return 0;
    } else if (nargs == 2) {
        if (!pyc_rt_unbox_int(a0, &s) || !pyc_rt_unbox_int(a1, &e)) return 0;
    } else if (nargs == 3) {
        if (!pyc_rt_unbox_int(a0, &s) || !pyc_rt_unbox_int(a1, &e)
            || !pyc_rt_unbox_int(a2, &p)) return 0;
        if (p == 0) return 0;
    } else return 0;
    *start = s; *stop = e; *step = p;
    return 1;
}

extern "C" void pyc_rt_raise_unbound(const char* name) {
    pyc_gil_ensure();
    PyErr_Format(PyExc_UnboundLocalError,
                 "cannot access local variable '%s' where it is not "
                 "associated with a value", name ? name : "");
}

// Part of the periodic check CPython's interpreter loop performs, which
// compiled code did not do at all. Called at every loop head, amortised over a
// counter as CPython amortises its own eval-breaker check.
//
// WHAT THIS FIXES. Signals never ran: `signal.alarm(1)` with
// `while True: n += 1` never reached the handler, and neither did Ctrl-C, so a
// compiled program in a loop could not be interrupted AT ALL.
//
// GIL yield: CPython drops the GIL only when another thread asked
// (_PY_GIL_DROP_REQUEST_BIT), via _Py_HandlePending. Unconditional
// SaveThread/RestoreThread fixed thread_starvation and deadlocked
// test_logging. HandlePending is the request-only path (C2).
extern "C" int pyc_rt_periodic(void) {
    static thread_local unsigned n = 0;
    if (++n < 2048) return 0;
    n = 0;
    return pyc_rt_handle_pending();
}

PyObject* pyc_rt_call(PyObject* callable, PyObject** args, Py_ssize_t nargs) {
    return PyObject_Vectorcall(callable, args, (size_t)nargs, nullptr);
}

PyObject* pyc_rt_call_ex(PyObject* callable, PyObject* args, PyObject* kwargs) {
    PyObject* t = args;
    int own = 0;
    if (!t) {
        t = PyTuple_New(0);
        if (!t) return nullptr;
        own = 1;
    } else if (!PyTuple_Check(t)) {
        t = PySequence_Tuple(args);
        if (!t) return nullptr;
        own = 1;
    }
    PyObject* r = PyObject_Call(callable, t, kwargs);
    if (own) Py_DECREF(t);
    return r;
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
// `...` is a singleton like None, not a value to materialise.
// No Py_RETURN_ELLIPSIS macro exists -- that family covers None/True/False
// and stops there, so the reference is taken explicitly.
extern "C" PyObject* pyc_rt_ellipsis(void) { return Py_NewRef(Py_Ellipsis); }

// Resolve the metaclass for `class C(bases, metaclass=M, **kwds)`.
//
// An explicit `metaclass=` wins and is REMOVED from kwds, because it is
// consumed by class creation and must not be forwarded to the metaclass call
// or to __init_subclass__. Otherwise the most derived among the bases' types,
// which is what type.__call__ requires: using `type` unconditionally breaks
// any class whose base has a custom metaclass (ABCMeta, enum.EnumMeta), and
// does so with a confusing error far from the cause.
//
// The winner must then be a subclass of every candidate. CPython raises
// "metaclass conflict" for that, and so does this, rather than producing a
// class whose type is silently wrong.
extern "C" PyObject* pyc_rt_expand_bases(PyObject* bases) {
    if (!bases || !PyTuple_Check(bases)) {
        PyErr_SetString(PyExc_TypeError, "bases must be a tuple");
        return nullptr;
    }
    Py_ssize_t n = PyTuple_GET_SIZE(bases);
    PyObject* acc = nullptr;
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* base = PyTuple_GET_ITEM(bases, i);
        if (PyType_Check(base)) {
            if (acc && PyList_Append(acc, base) < 0) goto fail;
            continue;
        }
        PyObject* meth = nullptr;
        if (PyObject_GetOptionalAttrString(base, "__mro_entries__", &meth) < 0)
            goto fail;
        if (!meth) {
            if (acc && PyList_Append(acc, base) < 0) goto fail;
            continue;
        }
        PyObject* repl = PyObject_CallOneArg(meth, bases);
        Py_DECREF(meth);
        if (!repl) goto fail;
        if (!PyTuple_Check(repl)) {
            PyErr_SetString(PyExc_TypeError, "__mro_entries__ must return a tuple");
            Py_DECREF(repl);
            goto fail;
        }
        if (!acc) {
            acc = PyList_New(0);
            if (!acc) { Py_DECREF(repl); return nullptr; }
            for (Py_ssize_t j = 0; j < i; ++j) {
                if (PyList_Append(acc, PyTuple_GET_ITEM(bases, j)) < 0) {
                    Py_DECREF(repl); goto fail;
                }
            }
        }
        Py_ssize_t rn = PyTuple_GET_SIZE(repl);
        for (Py_ssize_t j = 0; j < rn; ++j) {
            if (PyList_Append(acc, PyTuple_GET_ITEM(repl, j)) < 0) {
                Py_DECREF(repl); goto fail;
            }
        }
        Py_DECREF(repl);
    }
    if (!acc) { Py_INCREF(bases); return bases; }
    {
        PyObject* out = PyList_AsTuple(acc);
        Py_DECREF(acc);
        return out;
    }
fail:
    Py_XDECREF(acc);
    return nullptr;
}

extern "C" int pyc_rt_set_orig_bases(PyObject* ns, PyObject* orig,
                                     PyObject* expanded) {
    if (!ns || orig == expanded) return 0;
    PyObject* key = PyUnicode_InternFromString("__orig_bases__");
    if (!key) return -1;
    int r = PyObject_SetItem(ns, key, orig);
    Py_DECREF(key);
    return r;
}

extern "C" PyObject* pyc_rt_class_meta(PyObject* bases, PyObject* kwds) {
    PyObject* meta = nullptr;
    if (kwds) {
        PyObject* explicit_meta = nullptr;
        if (PyDict_GetItemStringRef(kwds, "metaclass", &explicit_meta) < 0)
            return nullptr;
        if (explicit_meta) {
            if (PyDict_DelItemString(kwds, "metaclass") < 0) {
                Py_DECREF(explicit_meta);
                return nullptr;
            }
            meta = explicit_meta;                        // owned
        }
    }
    if (!meta) meta = Py_NewRef(reinterpret_cast<PyObject*>(&PyType_Type));

    // An explicit metaclass that is not a type at all is legal: CPython calls
    // it directly and skips the derivation entirely.
    if (!PyType_Check(meta)) return meta;

    Py_ssize_t n = PyTuple_GET_SIZE(bases);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* b = PyTuple_GET_ITEM(bases, i);        // borrowed
        PyObject* bt = reinterpret_cast<PyObject*>(Py_TYPE(b));
        int sub = PyObject_IsSubclass(bt, meta);
        if (sub < 0) { Py_DECREF(meta); return nullptr; }
        if (sub) { Py_INCREF(bt); Py_DECREF(meta); meta = bt; continue; }
        int rev = PyObject_IsSubclass(meta, bt);
        if (rev < 0) { Py_DECREF(meta); return nullptr; }
        if (!rev) {
            Py_DECREF(meta);
            PyErr_SetString(PyExc_TypeError,
                "metaclass conflict: the metaclass of a derived class must be "
                "a (non-strict) subclass of the metaclasses of all its bases");
            return nullptr;
        }
    }
    return meta;
}

// meta.__prepare__(name, bases, **kwds), or a plain dict when the metaclass
// does not define it.
//
// This is not optional sugar. enum.EnumMeta.__prepare__ returns an _EnumDict
// that records member order and rejects duplicates; building an enum in a
// plain dict instead produces a class that is wrong rather than one that
// fails, which is the outcome I1 exists to prevent.
extern "C" PyObject* pyc_rt_class_prepare(PyObject* meta, PyObject* name,
                                          PyObject* bases, PyObject* kwds) {
    PyObject* prep = nullptr;
    if (PyObject_GetOptionalAttrString(meta, "__prepare__", &prep) < 0)
        return nullptr;
    if (!prep) return PyDict_New();

    PyObject* args = PyTuple_Pack(2, name, bases);
    if (!args) { Py_DECREF(prep); return nullptr; }
    PyObject* ns = PyObject_Call(prep, args, kwds);
    Py_DECREF(args);
    Py_DECREF(prep);
    if (!ns) return nullptr;
    // CPython requires a mapping here and says so plainly.
    if (!PyMapping_Check(ns)) {
        PyErr_Format(PyExc_TypeError,
                     "%.200s.__prepare__() must return a mapping, not %.200s",
                     PyType_Check(meta) ? ((PyTypeObject*)meta)->tp_name : "<metaclass>",
                     Py_TYPE(ns)->tp_name);
        Py_DECREF(ns);
        return nullptr;
    }
    return ns;
}

// type.__new__ wraps three names when it finds a plain function under them:
// __new__ becomes a staticmethod, __init_subclass__ and __class_getitem__
// become classmethods. It tests PyFunction_Check. Compiled callables are
// real PyFunction objects, so this matches CPython's own wrap.
static int wrap_class_attr(PyObject* ns, const char* name, PyTypeObject* kind) {
    PyObject* key = PyUnicode_FromString(name);
    if (!key) return -1;
    PyObject* fn = nullptr;
    int rc = PyMapping_GetOptionalItem(ns, key, &fn);
    if (rc < 0 || !fn) { Py_DECREF(key); return rc < 0 ? -1 : 0; }
    if (!PyFunction_Check(fn)) {
        Py_DECREF(fn); Py_DECREF(key); return 0;
    }
    PyObject* wrapped = PyObject_CallOneArg(reinterpret_cast<PyObject*>(kind), fn);
    Py_DECREF(fn);
    if (!wrapped) { Py_DECREF(key); return -1; }
    int set = PyObject_SetItem(ns, key, wrapped);
    Py_DECREF(wrapped);
    Py_DECREF(key);
    return set;
}

extern "C" PyObject* pyc_rt_build_class(const char* name, PyObject* bases,
                                        PyObject* ns, PyObject* meta,
                                        PyObject* kwds) {
    if (wrap_class_attr(ns, "__new__", &PyStaticMethod_Type) < 0) return nullptr;
    if (wrap_class_attr(ns, "__init_subclass__", &PyClassMethod_Type) < 0) return nullptr;
    if (wrap_class_attr(ns, "__class_getitem__", &PyClassMethod_Type) < 0) return nullptr;
    // CPython's compiler emits `__module__ = __name__` into every class body,
    // so the namespace reaches the metaclass already carrying it. pyc did not,
    // so no compiled class had __module__ at all -- and unittest reads
    // cls.__module__ during discovery, which is why 55 Lib/test files died with
    // `AttributeError: __module__` before running a single test.
    //
    // Set it here rather than in the lowerer: it belongs to every class
    // regardless of how the body was lowered, and one site cannot drift from
    // another. Only when absent, so an explicit `__module__ = ...` in the body
    // still wins, as it does in CPython.
    PyObject* modkey = PyUnicode_InternFromString("__module__");
    if (!modkey) return nullptr;
    int has_mod = PyMapping_HasKey(ns, modkey);
    if (has_mod < 0) { Py_DECREF(modkey); return nullptr; }
    if (has_mod == 0) {
        PyObject* g = globals_dict();                       // borrowed
        PyObject* modname = g ? PyDict_GetItemString(g, "__name__") : nullptr;
        if (modname && PyDict_SetItem(ns, modkey, modname) < 0) {
            Py_DECREF(modkey);
            return nullptr;
        }
    }
    Py_DECREF(modkey);

    PyObject* nm = PyUnicode_FromString(name);
    if (!nm) return nullptr;
    PyObject* args = PyTuple_Pack(3, nm, bases, ns);
    Py_DECREF(nm);
    if (!args) return nullptr;
    // kwds carries whatever the class statement wrote besides metaclass, and
    // reaches both the metaclass and __init_subclass__ -- which is how
    // `class C(Base, boundary=STRICT)` and `class C(Base, x=1)` work at all.
    PyObject* cls = PyObject_Call(meta, args, kwds);
    Py_DECREF(args);
    return cls;
}

// `raise X from Y`.
//
// Measured against 3.14.7, and every line below is one of those observations:
//
//   raise T from v          __cause__ = v,      __suppress_context__ = True
//   raise T from ValueError __cause__ = ValueError()  -- the CLASS is called
//   raise T from None       __cause__ = None,   __suppress_context__ = True
//   raise T                 __cause__ = None,   __suppress_context__ = False
//   raise T from 42         TypeError: exception causes must derive from
//                           BaseException
//
// The `from None` row is the one worth stating twice: it does not merely leave
// __cause__ unset, it SUPPRESSES the implicit context, which is the entire
// point of writing it. __context__ itself is still recorded in every case;
// only its display is suppressed.
//
// The cause is validated BEFORE the exception is raised, so a bad cause
// produces the TypeError instead of the requested exception, as CPython does.
extern "C" int pyc_rt_raise_from(PyObject* exc, PyObject* cause) {
    PyObject* c = nullptr;                      // owned, or null for None
    if (cause == Py_None) {
        c = nullptr;
    } else if (PyExceptionClass_Check(cause)) {
        c = PyObject_CallNoArgs(cause);
        if (!c) return -1;
        if (!PyExceptionInstance_Check(c)) {
            PyErr_Format(PyExc_TypeError,
                         "calling %R should have returned an instance of "
                         "BaseException, not %R",
                         cause, Py_TYPE(c));
            Py_DECREF(c);
            return -1;
        }
    } else if (PyExceptionInstance_Check(cause)) {
        c = Py_NewRef(cause);
    } else {
        PyErr_SetString(PyExc_TypeError,
                        "exception causes must derive from BaseException");
        return -1;
    }

    if (pyc_rt_raise(exc) == 0) {               // never happens: it returns -1
        Py_XDECREF(c);
        return -1;
    }
    // Take the exception back so the cause can be attached to the NORMALISED
    // object -- `raise ValueError from e` raises a class, and only after
    // normalisation is there an instance to attach to.
    PyObject* raised = PyErr_GetRaisedException();
    if (!raised) { Py_XDECREF(c); return -1; }
    PyException_SetCause(raised, c);            // steals c; sets suppress_context
    PyErr_SetRaisedException(raised);           // steals raised
    return -1;
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
        PyObject* ae = PyObject_GetAttrString(t, "__aenter__");
        PyObject* ax = PyObject_GetAttrString(t, "__aexit__");
        PyErr_Clear();
        const bool async_cm = ae && ax;
        Py_XDECREF(ae);
        Py_XDECREF(ax);
        if (async_cm) {
            PyErr_Format(PyExc_TypeError,
                         "'%s' object does not support the context manager protocol "
                         "(missed %s method) but it supports the asynchronous "
                         "context manager protocol. Did you mean to use 'async with'?",
                         Py_TYPE(mgr)->tp_name, name);
        } else {
            PyErr_Format(PyExc_TypeError,
                         "'%s' object does not support the context manager protocol "
                         "(missed %s method)",
                         Py_TYPE(mgr)->tp_name, name);
        }
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
    if (!r) {
        PyObject* raised = PyErr_GetRaisedException();
        if (raised && raised != exc)
            PyException_SetContext(raised, exc);
        else
            Py_DECREF(exc);
        if (raised) PyErr_SetRaisedException(raised);
        return -1;
    }
    int suppress = PyObject_IsTrue(r);
    Py_DECREF(r);
    if (suppress < 0) {
        PyObject* raised = PyErr_GetRaisedException();
        if (raised && raised != exc)
            PyException_SetContext(raised, exc);
        else
            Py_DECREF(exc);
        if (raised) PyErr_SetRaisedException(raised);
        return -1;
    }
    if (suppress) { Py_DECREF(exc); return 1; }
    PyErr_SetRaisedException(exc);
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

// Append one traceback entry for a compiled function.
//
// pyc compiles Python functions to native functions, so there are no Python
// frames and a propagating exception carried no location at all: an uncaught
// error in a deployed binary printed its type and message and nothing else.
// A synthetic code object plus a frame over it gives PyTraceBack_Here
// something to record, which is the technique Cython uses for the same reason.
//
// Frames are appended INNERMOST FIRST, as the exception unwinds, which is the
// order PyTraceBack_Here expects.
//
// The caret markers CPython draws under the failing expression are NOT
// reproduced: they come from co_positions() indexed by an instruction offset,
// and a frame with no bytecode has no meaningful offset. File, line, function
// and the source text are exact; the carets are not attempted rather than
// approximated.
//
// Cost is paid only on the error path. The code object is cached per call
// site so a raise inside a loop does not rebuild it.
extern "C" void pyc_rt_add_traceback(PyObject** cache, const char* file,
                                     const char* func, int line) {
    (void)cache; (void)file; (void)func; (void)line;
    pyc_rt_traceback_here();
}

extern "C" int pyc_rt_tuple_maybe_untrack(PyObject* t) {
    if (!t || !PyTuple_CheckExact(t)) return 0;
    if (!PyObject_GC_IsTracked(t)) return 0;
    Py_ssize_t n = PyTuple_GET_SIZE(t);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* x = PyTuple_GET_ITEM(t, i);
        if (x && PyObject_GC_IsTracked(x)) return 0;
    }
    PyObject_GC_UnTrack(t);
    return 0;
}

// A generator FUNCTION. Same mechanism as a generator expression: the body was
// compiled by CPython at build time and is run by the linked interpreter.
// pyc supplies the closure cells and the DEFAULTS -- both are evaluated in the
// enclosing scope at def time, which is pyc's job and not the wrapper's.
extern "C" PyObject* pyc_rt_make_genfunc(const char* blob, Py_ssize_t len,
                                         PyObject** cache, PyObject* closure,
                                         PyObject* defaults, PyObject* kwdefaults) {
    if (!*cache) {
        *cache = PyMarshal_ReadObjectFromString(const_cast<char*>(blob), len);
        if (!*cache) return nullptr;
    }
    PyObject* g = globals_dict();
    if (!g) return nullptr;
    PyObject* fn = PyFunction_New(*cache, g);
    if (!fn) return nullptr;
    if (closure && closure != Py_None
        && PyFunction_SetClosure(fn, closure) < 0) { Py_DECREF(fn); return nullptr; }
    if (defaults && defaults != Py_None
        && PyFunction_SetDefaults(fn, defaults) < 0) { Py_DECREF(fn); return nullptr; }
    if (kwdefaults && kwdefaults != Py_None
        && PyFunction_SetKwDefaults(fn, kwdefaults) < 0) { Py_DECREF(fn); return nullptr; }
    return fn;
}

// --- structural pattern matching -------------------------------------------
//
// Three-way results, because "did not match" is not an error: these return
// Py_None for no-match, a tuple of the extracted values for a match, and NULL
// with an exception set for a real failure.

// A sequence pattern matches what CPython's MATCH_SEQUENCE matches: the type
// carries Py_TPFLAGS_SEQUENCE. That deliberately EXCLUDES str, bytes and
// bytearray -- `case [a, b]` must not match "ab" -- which a PySequence_Check
// test would get wrong, since that is true for str.
extern "C" PyObject* pyc_rt_match_sequence(PyObject* subj, Py_ssize_t nbefore,
                                           Py_ssize_t nafter, int has_star) {
    if (!PyType_HasFeature(Py_TYPE(subj), Py_TPFLAGS_SEQUENCE)) Py_RETURN_NONE;
    Py_ssize_t size = PySequence_Size(subj);
    if (size < 0) return nullptr;
    if (has_star) { if (size < nbefore + nafter) Py_RETURN_NONE; }
    else          { if (size != nbefore)         Py_RETURN_NONE; }

    Py_ssize_t out_n = nbefore + nafter + (has_star ? 1 : 0);
    PyObject* out = PyTuple_New(out_n);
    if (!out) return nullptr;
    Py_ssize_t k = 0;
    for (Py_ssize_t i = 0; i < nbefore; ++i) {
        PyObject* it = PySequence_GetItem(subj, i);
        if (!it) { Py_DECREF(out); return nullptr; }
        PyTuple_SET_ITEM(out, k++, it);                 // steals
    }
    if (has_star) {
        PyObject* mid = PySequence_GetSlice(subj, nbefore, size - nafter);
        if (!mid) { Py_DECREF(out); return nullptr; }
        // The star name binds a LIST, whatever the subject's own type is.
        PyObject* lst = PySequence_List(mid);
        Py_DECREF(mid);
        if (!lst) { Py_DECREF(out); return nullptr; }
        PyTuple_SET_ITEM(out, k++, lst);
    }
    for (Py_ssize_t i = 0; i < nafter; ++i) {
        PyObject* it = PySequence_GetItem(subj, size - nafter + i);
        if (!it) { Py_DECREF(out); return nullptr; }
        PyTuple_SET_ITEM(out, k++, it);
    }
    return out;
}

// A mapping pattern matches what MATCH_MAPPING matches: Py_TPFLAGS_MAPPING.
// A missing key is NOT an error -- it simply does not match -- so KeyError is
// caught here rather than propagated.
extern "C" PyObject* pyc_rt_match_mapping(PyObject* subj, PyObject* keys,
                                          int want_rest) {
    if (!PyType_HasFeature(Py_TYPE(subj), Py_TPFLAGS_MAPPING)) Py_RETURN_NONE;
    Py_ssize_t nk = PyTuple_Size(keys);
    if (nk < 0) return nullptr;
    PyObject* out = PyTuple_New(nk + (want_rest ? 1 : 0));
    if (!out) return nullptr;
    for (Py_ssize_t i = 0; i < nk; ++i) {
        PyObject* key = PyTuple_GET_ITEM(keys, i);          // borrowed
        PyObject* val = PyObject_GetItem(subj, key);
        if (!val) {
            if (PyErr_ExceptionMatches(PyExc_KeyError)) {
                PyErr_Clear();
                Py_DECREF(out);
                Py_RETURN_NONE;
            }
            Py_DECREF(out);
            return nullptr;
        }
        PyTuple_SET_ITEM(out, i, val);                      // steals
    }
    if (want_rest) {
        // **rest binds a dict of everything the pattern did not name.
        PyObject* rest = PyDict_New();
        if (!rest) { Py_DECREF(out); return nullptr; }
        PyObject* items = PyMapping_Items(subj);
        if (!items) { Py_DECREF(rest); Py_DECREF(out); return nullptr; }
        Py_ssize_t n = PyList_Size(items);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* kv = PyList_GET_ITEM(items, i);        // borrowed
            PyObject* k = PyTuple_GET_ITEM(kv, 0);
            PyObject* v = PyTuple_GET_ITEM(kv, 1);
            int seen = 0;
            for (Py_ssize_t j = 0; j < nk && !seen; ++j) {
                int eq = PyObject_RichCompareBool(k, PyTuple_GET_ITEM(keys, j), Py_EQ);
                if (eq < 0) { Py_DECREF(items); Py_DECREF(rest); Py_DECREF(out); return nullptr; }
                seen = eq;
            }
            if (!seen && PyDict_SetItem(rest, k, v) < 0) {
                Py_DECREF(items); Py_DECREF(rest); Py_DECREF(out); return nullptr;
            }
        }
        Py_DECREF(items);
        PyTuple_SET_ITEM(out, nk, rest);                    // steals
    }
    return out;
}

// A class pattern: isinstance, then positional attributes named by
// __match_args__ and keyword attributes by name. A missing attribute does not
// match rather than raising, which is why AttributeError is caught here.
extern "C" PyObject* pyc_rt_match_class(PyObject* subj, PyObject* cls,
                                        int npos, PyObject* kwnames) {
    int ins = PyObject_IsInstance(subj, cls);
    if (ins < 0) return nullptr;
    if (!ins) Py_RETURN_NONE;

    Py_ssize_t nkw = kwnames ? PyTuple_Size(kwnames) : 0;
    if (nkw < 0) return nullptr;
    PyObject* out = PyTuple_New(npos + nkw);
    if (!out) return nullptr;

    if (npos > 0) {
        // int(x), str(x) and friends bind the SUBJECT itself rather than an
        // attribute. CPython marks those types with _Py_TPFLAGS_MATCH_SELF;
        // reading its flag is better than keeping a list of type names here
        // that would drift from the language.
        if (PyType_Check(cls)
            && PyType_HasFeature((PyTypeObject*)cls, _Py_TPFLAGS_MATCH_SELF)) {
            if (npos != 1) {
                PyErr_Format(PyExc_TypeError,
                             "%s() accepts 1 positional sub-pattern (%d given)",
                             ((PyTypeObject*)cls)->tp_name, npos);
                Py_DECREF(out);
                return nullptr;
            }
            Py_INCREF(subj);
            PyTuple_SET_ITEM(out, 0, subj);
        } else {
            PyObject* margs = PyObject_GetAttrString(cls, "__match_args__");
            if (!margs) {
                if (!PyErr_ExceptionMatches(PyExc_AttributeError)) { Py_DECREF(out); return nullptr; }
                PyErr_Clear();
                PyErr_Format(PyExc_TypeError,
                             "%s() accepts 0 positional sub-patterns (%d given)",
                             PyType_Check(cls) ? ((PyTypeObject*)cls)->tp_name : "?", npos);
                Py_DECREF(out);
                return nullptr;
            }
            if (!PyTuple_Check(margs)) {
                Py_DECREF(margs); Py_DECREF(out);
                PyErr_SetString(PyExc_TypeError, "__match_args__ must be a tuple");
                return nullptr;
            }
            if (PyTuple_Size(margs) < npos) {
                // CPython pluralises on the ACCEPTED count, not the given one:
                // "accepts 1 positional sub-pattern (2 given)". Measured, not
                // guessed -- the plural form here was the single difference the
                // match stress test found.
                PyErr_Format(PyExc_TypeError,
                             "%s() accepts %zd positional sub-pattern%s (%d given)",
                             PyType_Check(cls) ? ((PyTypeObject*)cls)->tp_name : "?",
                             PyTuple_Size(margs),
                             PyTuple_Size(margs) == 1 ? "" : "s", npos);
                Py_DECREF(margs); Py_DECREF(out);
                return nullptr;
            }
            for (int i = 0; i < npos; ++i) {
                PyObject* nm = PyTuple_GET_ITEM(margs, i);
                PyObject* v = PyObject_GetAttr(subj, nm);
                if (!v) {
                    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                        PyErr_Clear(); Py_DECREF(margs); Py_DECREF(out); Py_RETURN_NONE;
                    }
                    Py_DECREF(margs); Py_DECREF(out); return nullptr;
                }
                PyTuple_SET_ITEM(out, i, v);
            }
            Py_DECREF(margs);
        }
    }
    for (Py_ssize_t i = 0; i < nkw; ++i) {
        PyObject* v = PyObject_GetAttr(subj, PyTuple_GET_ITEM(kwnames, i));
        if (!v) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear(); Py_DECREF(out); Py_RETURN_NONE;
            }
            Py_DECREF(out); return nullptr;
        }
        PyTuple_SET_ITEM(out, npos + i, v);
    }
    return out;
}

extern "C" int pyc_rt_super_fail(int has_args) {
    if (has_args && tls_frame_locals && tls_frame_names && tls_frame_nnames > 0
        && tls_frame_names[0]
        && !PyDict_GetItemString(tls_frame_locals, tls_frame_names[0])) {
        PyErr_SetString(PyExc_RuntimeError, "super(): arg[0] deleted");
        return -1;
    }
    PyErr_SetString(PyExc_RuntimeError,
                    has_args ? "super(): __class__ cell not found"
                             : "super(): no arguments");
    return -1;
}

extern "C" PyObject* pyc_rt_call_super0(PyObject* fn, PyObject* klass,
                                        PyObject* self) {
    PyObject* b = PyEval_GetBuiltins();
    PyObject* builtin = b ? PyDict_GetItemString(b, "super") : nullptr;
    if (fn != builtin)
        return PyObject_CallNoArgs(fn);
    PyObject* args[2] = {klass, self};
    return PyObject_Vectorcall(fn, args, 2, nullptr);
}

extern "C" PyObject* pyc_rt_super_classcell(PyObject* cell) {
    if (!cell || !PyCell_Check(cell)) {
        PyErr_SetString(PyExc_RuntimeError, "super(): bad __class__ cell");
        return nullptr;
    }
    PyObject* v = PyCell_GET(cell);
    if (!v) {
        PyErr_SetString(PyExc_RuntimeError, "super(): empty __class__ cell");
        return nullptr;
    }
    Py_INCREF(v);
    return v;
}

extern "C" int pyc_rt_check_classcell(PyObject* cell, PyObject* cls,
                                      PyObject* name) {
    if (!cell || !PyCell_Check(cell) || !cls || !PyType_Check(cls)) return 0;
    PyObject* cell_cls = PyCell_GET(cell);
    if (cell_cls == cls) return 0;
    if (!cell_cls) {
        PyErr_Format(PyExc_RuntimeError,
                     "__class__ not set defining %R as %R. "
                     "Was __classcell__ propagated to type.__new__?",
                     name, cls);
    } else {
        PyErr_Format(PyExc_TypeError,
                     "__class__ set to %R defining %R as %R",
                     cell_cls, name, cls);
    }
    return -1;
}

extern "C" int pyc_rt_assert_fail(PyObject* msg) {
    if (!msg) {
        PyErr_SetNone(PyExc_AssertionError);
        return -1;
    }
    PyObject* e = PyObject_CallOneArg(PyExc_AssertionError, msg);
    if (!e) return -1;
    PyErr_SetRaisedException(e);
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

extern "C" PyObject* pyc_rt_star_annotation(PyObject* v) {
    PyObject* typing = PyImport_ImportModule("typing");
    if (!typing) return nullptr;
    PyObject* unpack = PyObject_GetAttrString(typing, "Unpack");
    Py_DECREF(typing);
    if (!unpack) return nullptr;
    PyObject* r = PyObject_GetItem(unpack, v);
    Py_DECREF(unpack);
    return r;
}

extern "C" PyObject* pyc_rt_annotate_check_format(PyObject* format) {
    long f = PyLong_AsLong(format);
    if (f == -1 && PyErr_Occurred()) return nullptr;
    if (f > 2) {
        PyErr_SetNone(PyExc_NotImplementedError);
        return nullptr;
    }
    Py_RETURN_NONE;
}

extern "C" int pyc_rt_cell_set(PyObject* cell, PyObject* v, const char* name) {
    if (PyCell_Set(cell, v) < 0) return -1;
    if (tls_frame_locals && name && name[0]) {
        if (v) PyDict_SetItemString(tls_frame_locals, name, v);
        else if (PyDict_DelItemString(tls_frame_locals, name) < 0)
            PyErr_Clear();
    }
    return 0;
}

extern "C" PyObject* pyc_rt_cell_get(PyObject* cell, const char* name, int is_free) {
    PyObject* v = PyCell_Get(cell);
    if (!v && !PyErr_Occurred()) {
        const char* n = name ? name : "?";
        if (is_free)
            PyErr_Format(PyExc_NameError,
                         "cannot access free variable '%s' where it is not "
                         "associated with a value in enclosing scope", n);
        else
            PyErr_Format(PyExc_UnboundLocalError,
                         "cannot access local variable '%s' where it is not "
                         "associated with a value", n);
    }
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

// `from X import a, b` is NOT `import X` followed by getattr.
//
// CPython compiles it to __import__(X, ..., fromlist=("a","b"), 0), and the
// FROMLIST is what makes the import machinery load X.a and X.b when they are
// submodules and bind them on X. Importing X alone leaves them unbound, so
// `from test import support` raised
//   AttributeError: module 'test' has no attribute 'support'
// -- 113 of 389 Lib/test files died on exactly this.
//
// names is a comma-separated list rather than a tuple so the lowering stays one
// call with two string constants; building the tuple is this function's job.
extern "C" PyObject* pyc_rt_import_from(PyObject* module, PyObject* names,
                                        int level) {
    const char* csv = PyUnicode_AsUTF8(names);
    if (!csv) return nullptr;
    PyObject* fromlist = PyList_New(0);
    if (!fromlist) return nullptr;
    const char* p = csv;
    while (*p) {
        const char* comma = std::strchr(p, ',');
        Py_ssize_t len = comma ? (comma - p) : (Py_ssize_t)std::strlen(p);
        PyObject* item = PyUnicode_FromStringAndSize(p, len);
        if (!item || PyList_Append(fromlist, item) < 0) {
            Py_XDECREF(item); Py_DECREF(fromlist); return nullptr;
        }
        Py_DECREF(item);
        if (!comma) break;
        p = comma + 1;
    }
    PyObject* globals = globals_dict();          // borrowed, may be null
    PyObject* mod = PyImport_ImportModuleLevelObject(
        module, globals, globals, fromlist, level);
    Py_DECREF(fromlist);
    return mod;                                  // new reference
}

// The attribute half of `from X import Y`. CPython's IMPORT_FROM tries getattr
// and, when that fails, falls back to sys.modules["X.Y"] -- which is what makes
// a partially-initialised circular import work. Reproduced here rather than
// left as a bare getattr.
extern "C" PyObject* pyc_rt_import_attr(PyObject* mod, PyObject* name) {
    PyObject* v = PyObject_GetAttr(mod, name);
    if (v) return v;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return nullptr;

    PyObject *t = nullptr, *ev = nullptr, *tb = nullptr;
    PyErr_Fetch(&t, &ev, &tb);                   // keep the original error
    PyObject* pkg = PyObject_GetAttrString(mod, "__name__");
    if (pkg && PyUnicode_Check(pkg)) {
        PyObject* full = PyUnicode_FromFormat("%U.%U", pkg, name);
        if (full) {
            PyObject* sysmods = PyImport_GetModuleDict();      // borrowed
            PyObject* sub = sysmods ? PyDict_GetItem(sysmods, full) : nullptr;
            if (sub) {
                Py_INCREF(sub);
                Py_XDECREF(pkg); Py_DECREF(full);
                Py_XDECREF(t); Py_XDECREF(ev); Py_XDECREF(tb);
                return sub;
            }
            Py_DECREF(full);
        }
    }
    Py_XDECREF(pkg);
    PyErr_Clear();                               // from the fallback probing
    PyErr_Restore(t, ev, tb);                    // restore the real AttributeError
    return nullptr;
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

// except*: split `exc` into (match, rest). Non-groups are wrapped when they
// match, so the handler always sees an ExceptionGroup (PEP 654).
extern "C" PyObject* pyc_rt_except_star_split(PyObject* exc, PyObject* type) {
    if (!exc || !type) { PyErr_BadInternalCall(); return nullptr; }
    PyObject* splitf = PyObject_GetAttrString(exc, "split");
    if (splitf) {
        PyObject* pair = PyObject_CallOneArg(splitf, type);
        Py_DECREF(splitf);
        return pair;
    }
    PyErr_Clear();
    int m = PyErr_GivenExceptionMatches(exc, type);
    if (m < 0) return nullptr;
    PyObject* match;
    PyObject* rest;
    if (m) {
        PyObject* lst = PyList_New(1);
        if (!lst) return nullptr;
        Py_INCREF(exc);
        PyList_SET_ITEM(lst, 0, exc);
        PyObject* b = PyEval_GetBuiltins();
        PyObject* eg = b ? PyDict_GetItemString(b, "ExceptionGroup") : nullptr;
        match = eg ? PyObject_CallFunction(eg, "sO", "", lst) : nullptr;
        Py_DECREF(lst);
        if (!match) return nullptr;
        rest = Py_NewRef(Py_None);
    } else {
        match = Py_NewRef(Py_None);
        rest = Py_NewRef(exc);
    }
    PyObject* out = PyTuple_Pack(2, match, rest);
    Py_DECREF(match);
    Py_DECREF(rest);
    return out;
}

extern "C" PyObject* pyc_rt_newref(PyObject* o) {
    if (!o) { PyErr_BadInternalCall(); return nullptr; }
    return Py_NewRef(o);
}

extern "C" PyObject* pyc_rt_type_param(PyObject* kind, PyObject* name,
                                       PyObject* bound, PyObject* deflt) {
    if (!kind || !name) { PyErr_BadInternalCall(); return nullptr; }
    long k = PyLong_AsLong(kind);
    if (k < 0 && PyErr_Occurred()) return nullptr;
    PyObject* typing = PyImport_ImportModule("typing");
    if (!typing) return nullptr;
    const char* clsname = k == 1 ? "TypeVarTuple" : k == 2 ? "ParamSpec" : "TypeVar";
    PyObject* cls = PyObject_GetAttrString(typing, clsname);
    Py_DECREF(typing);
    if (!cls) return nullptr;
    PyObject* kw = PyDict_New();
    if (!kw) { Py_DECREF(cls); return nullptr; }
    if (k == 0 && PyDict_SetItemString(kw, "infer_variance", Py_True) < 0) {
        Py_DECREF(kw); Py_DECREF(cls); return nullptr;
    }
    if (deflt && deflt != Py_None) {
        if (PyDict_SetItemString(kw, "default", deflt) < 0) {
            Py_DECREF(kw); Py_DECREF(cls); return nullptr;
        }
    }
    PyObject* args;
    if (k == 0 && bound && bound != Py_None && PyTuple_Check(bound)) {
        PyObject* n = PyTuple_Pack(1, name);
        if (!n) { Py_DECREF(kw); Py_DECREF(cls); return nullptr; }
        args = PySequence_Concat(n, bound);
        Py_DECREF(n);
    } else {
        args = PyTuple_Pack(1, name);
        if (args && bound && bound != Py_None && k != 1) {
            if (PyDict_SetItemString(kw, "bound", bound) < 0) {
                Py_DECREF(args); Py_DECREF(kw); Py_DECREF(cls); return nullptr;
            }
        }
    }
    if (!args) { Py_DECREF(kw); Py_DECREF(cls); return nullptr; }
    PyObject* r = PyObject_Call(cls, args, kw);
    Py_DECREF(args); Py_DECREF(kw); Py_DECREF(cls);
    return r;
}

extern "C" int pyc_rt_del_if_same(PyObject* ns, PyObject* name, PyObject* tv) {
    if (!ns || !name || !tv) { PyErr_BadInternalCall(); return -1; }
    PyObject* cur = PyObject_GetItem(ns, name);
    if (!cur) { PyErr_Clear(); return 0; }
    int same = (cur == tv);
    Py_DECREF(cur);
    if (same && PyObject_DelItem(ns, name) < 0) return -1;
    return 0;
}

extern "C" PyObject* pyc_rt_type_alias(PyObject* name, PyObject* value,
                                       PyObject* params) {
    PyObject* typing = PyImport_ImportModule("typing");
    if (!typing) return nullptr;
    PyObject* cls = PyObject_GetAttrString(typing, "TypeAliasType");
    Py_DECREF(typing);
    if (!cls) return nullptr;
    PyObject* args = PyTuple_Pack(2, name, value);
    if (!args) { Py_DECREF(cls); return nullptr; }
    PyObject* kw = nullptr;
    if (params && params != Py_None) {
        kw = PyDict_New();
        if (!kw || PyDict_SetItemString(kw, "type_params", params) < 0) {
            Py_XDECREF(kw); Py_DECREF(args); Py_DECREF(cls); return nullptr;
        }
    }
    PyObject* r = PyObject_Call(cls, args, kw);
    Py_XDECREF(kw);
    Py_DECREF(args);
    Py_DECREF(cls);
    return r;
}

extern "C" PyObject* pyc_rt_interpolation(PyObject* value, PyObject* expr,
                                          PyObject* conv, PyObject* spec) {
    PyObject* mod = PyImport_ImportModule("string.templatelib");
    if (!mod) return nullptr;
    PyObject* cls = PyObject_GetAttrString(mod, "Interpolation");
    Py_DECREF(mod);
    if (!cls) return nullptr;
    PyObject* args = PyTuple_Pack(4, value, expr, conv, spec);
    if (!args) { Py_DECREF(cls); return nullptr; }
    PyObject* r = PyObject_Call(cls, args, nullptr);
    Py_DECREF(args);
    Py_DECREF(cls);
    return r;
}

extern "C" PyObject* pyc_rt_template(PyObject* parts) {
    PyObject* mod = PyImport_ImportModule("string.templatelib");
    if (!mod) return nullptr;
    PyObject* cls = PyObject_GetAttrString(mod, "Template");
    Py_DECREF(mod);
    if (!cls) return nullptr;
    PyObject* args = PySequence_Tuple(parts);
    if (!args) { Py_DECREF(cls); return nullptr; }
    PyObject* r = PyObject_Call(cls, args, nullptr);
    Py_DECREF(args);
    Py_DECREF(cls);
    return r;
}
