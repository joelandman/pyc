# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# KNOWN GAP: recursive deallocation exhausts the C stack in a compiled binary
# where CPython defers it. Lib/test/test_builtin's test_filter_dealloc builds a
# million nested filter objects for exactly this reason (gh-102356).
#
# Measured, 8 MB stack (the default here):
#     pyc       ok at 400_000, SIGSEGV at 600_000
#     CPython   ok at 4_000_000
#     pyc with `ulimit -s 65536` or unlimited: ok at 1_000_000
#
# So it is C-stack exhaustion during the dealloc chain, and CPython's deferral
# is what avoids it. In 3.14 that lives in _Py_Dealloc itself:
#
#     intptr_t margin = _Py_RecursionLimit_GetMargin(tstate);
#     if (margin < 2 && gc_flag) { _PyTrash_thread_deposit_object(...); return; }
#
# gc_flag is a type property and is identical either way, so the difference is
# `margin`, i.e. c_stack_soft_limit. WHY that differs is NOT established.
#
# Refuted: that the recursion limits are simply never initialised. Forcing a
# 500-deep Python recursion first (which exercises that machinery) does not
# prevent the crash, and pycore_interp_init calls _Py_InitializeRecursionLimits
# during Py_InitializeFromConfig, which a compiled binary does run.
#
# Kept at 600_000 -- above the measured pyc ceiling, far below CPython's.
import gc

n = 600000
i = filter(bool, range(n))
for _ in range(n):
    i = filter(bool, i)
del i
gc.collect()
print("survived recursive dealloc")
