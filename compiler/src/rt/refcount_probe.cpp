// A2 refcount-discipline harness.
//
// Runs a workload repeatedly inside one interpreter and reports the change in
// the global reference total. A correct sequence returns to its starting
// total; a leak grows it linearly with iterations, which is what makes the
// slope -- not the absolute number -- the signal.
//
// The first iterations are discarded: interned strings, cached small ints and
// lazily-imported modules legitimately raise the total once and never again.
// Counting warm-up as a leak would make every correct program look broken.
#include "pyc/rt/entry.hpp"

#include <Python.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct Workload { const char* name; int (*run)(); };

// Correct: every new reference is released.
int wl_clean() {
    PyObject* a = PyLong_FromLong(12345678);
    PyObject* b = PyLong_FromLong(87654321);
    if (!a || !b) { Py_XDECREF(a); Py_XDECREF(b); return -1; }
    PyObject* sum = PyNumber_Add(a, b);          // Owned
    Py_DECREF(a); Py_DECREF(b);
    if (!sum) return -1;
    Py_DECREF(sum);
    return 0;
}

// Correct, and exercises the steal contract: PyTuple_SetItem takes ownership,
// so the caller must NOT decref the item afterwards.
int wl_steal() {
    PyObject* t = PyTuple_New(2);
    if (!t) return -1;
    for (int i = 0; i < 2; ++i) {
        PyObject* v = PyLong_FromLong(1000 + i);
        if (!v) { Py_DECREF(t); return -1; }
        PyTuple_SetItem(t, i, v);                // steals v
    }
    Py_DECREF(t);
    return 0;
}

// Correct: a borrowed reference must NOT be decref'd.
int wl_borrow() {
    PyObject* l = PyList_New(0);
    if (!l) return -1;
    PyObject* v = PyLong_FromLong(7);
    if (!v || PyList_Append(l, v) != 0) { Py_XDECREF(v); Py_DECREF(l); return -1; }
    Py_DECREF(v);
    PyObject* got = PyList_GetItem(l, 0);        // Borrowed -- do not release
    if (!got) { Py_DECREF(l); return -1; }
    Py_DECREF(l);
    return 0;
}

// DELIBERATELY WRONG. Present so the harness is proved able to fail: a leak
// detector that has never detected a leak is not evidence of anything.
int wl_leak() {
    PyObject* a = PyLong_FromLong(999983);
    if (!a) return -1;
    return 0;                                    // never released
}

// DELIBERATELY WRONG the other way: decref'ing a reference the call stole.
// Under Py_REF_DEBUG this shows as a NEGATIVE slope before it corrupts.
int wl_overrelease() {
    PyObject* t = PyTuple_New(1);
    if (!t) return -1;
    PyObject* v = PyLong_FromLong(4242424);
    if (!v) { Py_DECREF(t); return -1; }
    PyTuple_SetItem(t, 0, v);                    // steals v
    Py_INCREF(v);                                // pretend we still own it...
    Py_DECREF(v);                                // ...balanced, so t stays valid
    Py_DECREF(t);
    return 0;
}

const Workload kWorkloads[] = {
    {"clean",       wl_clean},
    {"steal",       wl_steal},
    {"borrow",      wl_borrow},
    {"overrelease", wl_overrelease},
    {"leak",        wl_leak},        // expected to FAIL; see main()
};

}  // namespace

int main(int argc, char** argv) {
    const char* only = (argc > 1) ? argv[1] : nullptr;
    int iters = (argc > 2) ? atoi(argv[2]) : 2000;

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;
    if (PyStatus_Exception(Py_InitializeFromConfig(&config))) {
        std::fprintf(stderr, "init failed\n"); return 2;
    }
    PyConfig_Clear(&config);

    if (pyc_rt_total_refcount() < 0) {
        std::fprintf(stderr,
            "refcount_probe: interpreter is not a Py_REF_DEBUG build.\n"
            "  Build a sysroot with --debug; a release build would report\n"
            "  zero leaks because nothing is counting.\n");
        Py_FinalizeEx();
        return 2;
    }

    int failures = 0;
    for (const Workload& w : kWorkloads) {
        if (only && std::strcmp(only, w.name) != 0) continue;

        for (int i = 0; i < 200; ++i) if (w.run() != 0) { PyErr_Clear(); }
        long long before = pyc_rt_total_refcount();
        for (int i = 0; i < iters; ++i) if (w.run() != 0) { PyErr_Clear(); }
        long long after = pyc_rt_total_refcount();

        double per_iter = double(after - before) / iters;
        bool expected_leak = std::strcmp(w.name, "leak") == 0;
        bool leaked = (after - before) != 0;
        bool ok = leaked == expected_leak;
        failures += !ok;

        std::printf("  %-12s delta=%+6lld over %d iters  (%+.3f/iter)  %s%s\n",
                    w.name, after - before, iters, per_iter,
                    ok ? "OK" : "FAIL",
                    expected_leak ? "  [expected to leak]" : "");
    }
    Py_FinalizeEx();
    return failures ? 1 : 0;
}
