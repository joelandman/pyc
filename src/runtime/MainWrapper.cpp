// C entry-point wrapper for pyc-compiled programs.
//
// The pyc codegen emits an LLVM function called `main` that takes no
// arguments. The C runtime always calls `main(argc, argv)`, so we
// declare a C-level `main` here that intercepts the call, sets up
// a synthetic `sys` module (so user code can do `import sys` /
// `sys.argv[i]`), and then invokes the LLVM-generated `main`. The
// LLVM main signature is `PyObject* main()` (no args); we call it
// with no arguments, which matches its declared function type.
//
// This wrapper is only used when the codegen has not itself emitted
// a C main; the two are mutually exclusive. To keep build wiring
// simple, the wrapper always defines `main` and the LLVM IR's
// `main` is renamed to `pyc_user_main` via the codegen so the two
// don't clash.

#include "pyc/runtime.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration of the LLVM-generated user entry point.
PyObject* pyc_user_main(void);

int main(int argc, char** argv) {
    // Build the synthetic sys module so `import sys` and
    // `sys.argv[i]` work from user code.
    pyc_setup_sys(argc, argv);
    // Run the user code. The returned PyObject* is ignored.
    PyObject* result = pyc_user_main();
    if (result) {
        // Top-level return value isn't meaningful in normal Python
        // scripts; we just DECREF for cleanliness.
        Py_DECREF(result);
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
