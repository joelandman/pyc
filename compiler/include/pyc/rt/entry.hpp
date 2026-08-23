#pragma once
// Program entry (INTERFACES.md §4). Owned by A2 because it is C-API
// sequencing, not code generation.
//
// A4 emits a main() that calls pyc_rt_main with the module body A3 lowered.
// Keeping the sequencing here means the Py_Initialize / finalize / error
// protocol is written once and verified once.

#ifdef __cplusplus
extern "C" {
#endif

// The compiled module body. C-API convention (INTERFACES §3): 0 on success,
// -1 with a Python exception set on failure. No setjmp/longjmp -- the old
// tree's jump-based frames are a documented frame-leak source.
typedef int (*PycModuleBody)(void);

// Full program lifecycle. Returns the process exit status.
int pyc_rt_main(int argc, char** argv, PycModuleBody body);

// Total reference count, or -1 when the interpreter is not a Py_REF_DEBUG
// build. The leak check depends on this, and a release build silently
// reporting 0 leaks would be worse than no check at all -- hence -1, not 0.
long long pyc_rt_total_refcount(void);

#ifdef __cplusplus
}
#endif
