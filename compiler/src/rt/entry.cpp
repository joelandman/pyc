#include <limits.h>
#include "pyc/rt/entry.hpp"

#include <Python.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

extern "C" {

long long pyc_rt_total_refcount(void) {
#ifdef Py_REF_DEBUG
    return static_cast<long long>(_Py_GetGlobalRefTotal());
#else
    return -1;   // NOT zero: a release build must not look leak-free
#endif
}

// `python script.py` puts the SCRIPT's directory on sys.path, which is how
// `import utils` finds a module sitting next to it. A compiled binary has no
// script at run time, so nothing was prepended and every sibling import failed
// with ModuleNotFoundError -- the compiler accepted the program and the binary
// then could not run it.
//
// The executable's own directory is the honest analogue: it is where modules
// that sat beside the source get deployed, and it keeps `./app` working from
// any working directory. Deliberately NOT the cwd -- CPython does not put the
// cwd on the path for a script, and doing so would let a stray file shadow a
// stdlib module, which is a divergence in the other direction.
static int add_program_dir_to_path(void) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string dir;
    if (n > 0) {
        buf[n] = '\0';
        std::string exe(buf);
        std::size_t slash = exe.find_last_of('/');
        if (slash != std::string::npos) dir = exe.substr(0, slash);
    }
    if (dir.empty()) return 0;      // cannot tell where we are: change nothing

    PyObject* path = PySys_GetObject("path");          // borrowed
    if (!path || !PyList_Check(path)) return 0;
    PyObject* d = PyUnicode_FromString(dir.c_str());
    if (!d) { PyErr_Clear(); return 0; }
    int rc = PyList_Insert(path, 0, d);
    Py_DECREF(d);
    if (rc < 0) { PyErr_Clear(); return 0; }
    return 0;
}

static int configure(int argc, char** argv) {
    PyConfig config;
    // PEP 587. The pre-587 Py_SetProgramName/Py_SetPath family is deprecated
    // and removed in 3.13+, so a compiled binary has no choice here.
    PyConfig_InitPythonConfig(&config);

    // The binary is a program, not an interpreter: it must never be steered
    // into running something else by its own argv.
    config.parse_argv = 0;
    config.install_signal_handlers = 1;
    // Writing .pyc files next to a deployed binary's stdlib is surprising and
    // often not permitted.
    config.write_bytecode = 0;

    PyStatus status = PyConfig_SetBytesArgv(&config, argc, argv);
    if (PyStatus_Exception(status)) goto fail;

    status = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(status)) goto fail;

    PyConfig_Clear(&config);
    return add_program_dir_to_path();

fail:
    PyConfig_Clear(&config);
    if (PyStatus_IsExit(status)) return status.exitcode;
    std::fprintf(stderr, "pyc: interpreter initialisation failed: %s\n",
                 status.err_msg ? status.err_msg : "(no message)");
    return -1;
}

// __main__.__file__ is the absolute path of the code that is executing, which
// for a compiled program is the binary itself. Absent, it was not merely
// missing: doctest.DocTestSuite() raised ValueError, unittest's load_tests
// contributed nothing, and Lib/test/test_unpack.py ran 1 test instead of 2 and
// reported OK -- a silent wrong answer (issue #9).
//
// /proc/self/exe rather than argv[0]: it is already absolute, survives a
// relative or PATH-resolved invocation, and cannot be spoofed by an exec that
// passes an arbitrary argv[0]. Falls back to realpath(argv[0]) where
// /proc is not mounted.
static void set_main_file(char** argv) {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string path;
    if (n > 0) {
        buf[n] = '\0';
        path = buf;
    } else if (argv && argv[0]) {
        char* rp = ::realpath(argv[0], nullptr);
        if (rp) { path = rp; std::free(rp); }
    }
    if (path.empty()) return;      // better no __file__ than a wrong one

    PyObject* m = PyImport_AddModule("__main__");            // borrowed
    if (!m) { PyErr_Clear(); return; }
    PyObject* v = PyUnicode_FromString(path.c_str());
    if (!v) { PyErr_Clear(); return; }
    if (PyObject_SetAttrString(m, "__file__", v) < 0) PyErr_Clear();
    Py_DECREF(v);
}

int pyc_rt_main(int argc, char** argv, PycModuleBody body) {
    int rc = configure(argc, argv);
    if (rc != 0) return rc < 0 ? 1 : rc;
    set_main_file(argv);

    int status = 0;
    if (body && body() != 0) {
        // SystemExit is control flow, not an error: honour its code and do not
        // print a traceback, which is what CPython itself does.
        if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
            PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
            PyErr_Fetch(&type, &value, &tb);
            PyErr_NormalizeException(&type, &value, &tb);
            status = 0;
            if (value) {
                PyObject* code = PyObject_GetAttrString(value, "code");
                if (code && code != Py_None) {
                    if (PyLong_Check(code)) status = (int)PyLong_AsLong(code);
                    else { PyObject_Print(code, stderr, Py_PRINT_RAW);
                           std::fprintf(stderr, "\n"); status = 1; }
                }
                Py_XDECREF(code);
            }
            Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(tb);
        } else {
            PyErr_Print();          // traceback to stderr, clears the error
            status = 1;
        }
    }

    if (Py_FinalizeEx() < 0) {
        // Finalisation failure is reported, never swallowed: it usually means
        // an atexit handler or __del__ raised.
        std::fprintf(stderr, "pyc: interpreter finalisation failed\n");
        if (status == 0) status = 120;
    }
    return status;
}

}  // extern "C"
