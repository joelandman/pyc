#include "pyc/rt/entry.hpp"

#include <Python.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {

long long pyc_rt_total_refcount(void) {
#ifdef Py_REF_DEBUG
    return static_cast<long long>(_Py_GetGlobalRefTotal());
#else
    return -1;   // NOT zero: a release build must not look leak-free
#endif
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
    return 0;

fail:
    PyConfig_Clear(&config);
    if (PyStatus_IsExit(status)) return status.exitcode;
    std::fprintf(stderr, "pyc: interpreter initialisation failed: %s\n",
                 status.err_msg ? status.err_msg : "(no message)");
    return -1;
}

int pyc_rt_main(int argc, char** argv, PycModuleBody body) {
    int rc = configure(argc, argv);
    if (rc != 0) return rc < 0 ? 1 : rc;

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
